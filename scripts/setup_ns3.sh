#!/usr/bin/env bash
# =============================================================================
# setup_ns3.sh — Prepara o ambiente do ns-3.48 para o trabalho de MANET.
#
# O que faz (nesta ordem):
#   1. Verifica as dependências mínimas exigidas pelo ns-3.48.
#   2. Baixa e extrai o código-fonte oficial do ns-3.48 em $NS3_DIR.
#   3. Configura com perfil "optimized" e exemplos habilitados.
#   4. Compila.
#   5. Cria o link simbólico scratch/manet -> <repositório>/src.
#
# Por que um script em vez de comandos soltos no README?
#   Reprodutibilidade: qualquer pessoa obtém exatamente a mesma versão e as
#   mesmas opções de compilação com um único comando. O script é idempotente:
#   pode ser executado de novo sem refazer o que já está pronto.
#
# Este script NUNCA usa sudo nem altera arquivos do ns-3: se faltar alguma
# dependência, ele apenas mostra o comando de instalação e encerra.
#
# Uso:
#   ./scripts/setup_ns3.sh
# Variáveis opcionais:
#   NS3_DIR=~/ns-3   diretório de instalação do ns-3
#   JOBS=N           número de processos paralelos de compilação
# =============================================================================

# -e: aborta no primeiro erro; -u: erro ao usar variável não definida;
# -o pipefail: um erro em qualquer comando de um pipe faz o pipe falhar.
set -euo pipefail

NS3_VERSION="3.48"
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
NS3_URL="https://www.nsnam.org/releases/ns-${NS3_VERSION}.tar.bz2"

# Diretório do repositório = pai do diretório onde este script está.
# Assim o script funciona independentemente de onde o repositório foi clonado.
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

log()  { printf '\n[setup] %s\n' "$*"; }
fail() { printf '\n[setup][ERRO] %s\n' "$*" >&2; exit 1; }

# Compara versões no formato X.Y[.Z]: retorna sucesso se $1 >= $2.
# "sort -V" entende ordenação de versões (3.9 < 3.10), ao contrário de
# uma comparação de texto simples.
version_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" = "$2" ]; }

# -----------------------------------------------------------------------------
# 1. Dependências
# Requisitos mínimos do ns-3.48 (guia de instalação oficial):
#   g++ >= 11.1, cmake >= 3.25, python3 >= 3.10, make ou ninja,
#   tar + bunzip2 para extrair o pacote.
# -----------------------------------------------------------------------------
APT_CMD="apt update && apt install -y build-essential g++ cmake ninja-build python3 python3-dev python3-venv python3-pip pkg-config bzip2 wget ca-certificates git"

check_deps() {
    # O ./ns3 recusa-se a rodar como root (por segurança). Verificamos isso
    # antes de baixar ~100 MB, para falhar cedo com uma mensagem clara.
    if [ "$(id -u)" -eq 0 ]; then
        fail "Não execute como root: o ns-3 recusa rodar como root. Use um usuário comum."
    fi

    log "Verificando dependências..."
    local missing=0

    for tool in g++ cmake python3 bzip2 tar wget; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "  - ausente: $tool"; missing=1
        fi
    done
    if ! command -v ninja >/dev/null 2>&1 && ! command -v make >/dev/null 2>&1; then
        echo "  - ausente: ninja ou make"; missing=1
    fi

    if [ "$missing" -eq 0 ]; then
        local gxx cm py
        gxx="$(g++ -dumpfullversion -dumpversion)"
        cm="$(cmake --version | head -n1 | awk '{print $3}')"
        py="$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
        echo "  g++ $gxx | cmake $cm | python3 $py"
        version_ge "$gxx" "11.1" || { echo "  - g++ $gxx < 11.1"; missing=1; }
        version_ge "$cm"  "3.25" || { echo "  - cmake $cm < 3.25"; missing=1; }
        version_ge "$py"  "3.10" || { echo "  - python3 $py < 3.10"; missing=1; }
    fi

    if [ "$missing" -ne 0 ]; then
        fail "Dependências faltando. Instale (requer root/sudo) com:
    sudo bash -c \"$APT_CMD\"
e execute este script novamente."
    fi
}

# -----------------------------------------------------------------------------
# 2. Download e extração
# Usamos o tarball oficial da release (e não o git) porque ele corresponde
# exatamente à versão publicada 3.48, sem commits posteriores.
# -----------------------------------------------------------------------------
download_ns3() {
    if [ -x "$NS3_DIR/ns3" ]; then
        log "ns-3 já presente em $NS3_DIR — pulando download."
        return
    fi
    local tmp tarball
    tmp="$(mktemp -d)"
    tarball="$tmp/ns-${NS3_VERSION}.tar.bz2"
    log "Baixando $NS3_URL ..."
    wget -q --show-progress -O "$tarball" "$NS3_URL"
    log "Extraindo..."
    tar -xjf "$tarball" -C "$tmp"
    # O tarball extrai para ns-3.48/; movemos para $NS3_DIR (padrão ~/ns-3)
    # para que o caminho não dependa da versão.
    mv "$tmp/ns-${NS3_VERSION}" "$NS3_DIR"
    rm -rf "$tmp"
}

# -----------------------------------------------------------------------------
# 3. Configuração
#   --build-profile=optimized : compila com -O3 (simulações muito mais rápidas
#       que o perfil "debug", essencial para as 60+ execuções). Desativa os
#       NS_LOG e asserts — para depuração usar o perfil "debug".
#   --enable-examples : compila examples/, necessário para rodar o
#       manet-routing-compare usado como referência.
# Os testes ficam desabilitados (padrão) para reduzir o tempo de compilação.
# -----------------------------------------------------------------------------
configure_ns3() {
    log "Configurando ns-3 (optimized, examples habilitados)..."
    cd "$NS3_DIR"
    ./ns3 configure --build-profile=optimized --enable-examples
}

# -----------------------------------------------------------------------------
# 4. Compilação
# Número de jobs: limitado pela memória (~1,5 GB por processo do compilador
# em alguns arquivos do ns-3), para evitar que o WSL fique sem RAM e o
# compilador seja morto pelo kernel (OOM).
# -----------------------------------------------------------------------------
build_ns3() {
    local mem_gb max_by_mem
    mem_gb="$(awk '/MemTotal/ {printf "%d", $2/1024/1024}' /proc/meminfo)"
    max_by_mem=$(( mem_gb * 2 / 3 )); [ "$max_by_mem" -lt 1 ] && max_by_mem=1
    local jobs="${JOBS:-$(( $(nproc) < max_by_mem ? $(nproc) : max_by_mem ))}"
    log "Compilando com $jobs jobs (RAM: ${mem_gb} GB, CPUs: $(nproc))..."
    cd "$NS3_DIR"
    ./ns3 build -j "$jobs"
}

# -----------------------------------------------------------------------------
# 5. Link simbólico
# O código do trabalho fica no repositório, fora da árvore do ns-3. O link em
# scratch/ faz o ns-3 enxergá-lo como um programa do scratch, sem copiar nem
# modificar nada do ns-3. -sfn: recria o link se já existir.
# -----------------------------------------------------------------------------
link_project() {
    log "Ligando $NS3_DIR/scratch/manet -> $PROJECT_DIR/src"
    ln -sfn "$PROJECT_DIR/src" "$NS3_DIR/scratch/manet"
}

check_deps
download_ns3
configure_ns3
build_ns3
link_project

log "Pronto. Teste com:
    cd $NS3_DIR && ./ns3 run hello-simulator"
