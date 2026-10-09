#!/usr/bin/env bash
# =============================================================================
# run_batch.sh — Executa em lote as simulações do manet-routing.
#
# Experimento-base (padrão): 3 protocolos × velocidades 1, 5, 10, 20 m/s ×
# runs 1..100 = 1200 execuções.
#
# Etapa da variável adicional: -v <parâmetro> -V "<valores>" varia um parâmetro
# do programa (ex.: pauseTime), mantendo o resto fixo. Os dados vão para outra
# pasta (data/raw/<experimento>/), separados do experimento-base, como pede o
# slide 9 ("a extensão precisa ser claramente separada do experimento-base").
#
# Uso:
#   ./scripts/run_batch.sh                         # experimento-base completo
#   ./scripts/run_batch.sh -r 5 -n teste           # teste rápido (5 runs)
#   ./scripts/run_batch.sh -v pauseTime -V "0 30 60 120" -s "10"
#   ./scripts/run_batch.sh -d                      # só mostra o plano (dry-run)
#
# Opções:
#   -n NOME   nome do experimento (pasta em data/raw/). Padrão: "base", ou
#             "var-<parâmetro>" quando -v é usado.
#   -p LISTA  protocolos (padrão: "AODV OLSR DSDV")
#   -s LISTA  velocidades em m/s (padrão: "1 5 10 20")
#   -r RUNS   N (= runs 1..N) ou A-B (padrão: 100)
#   -v PARAM  parâmetro adicional a variar: nodes, flows, packetSize, pauseTime,
#             areaSize, pktRate, txPower, propagation
#   -V LISTA  valores do parâmetro adicional
#   -x ARGS   argumentos fixos extras para todas as execuções (ex.: "--nodes=30")
#   -j N      execuções em paralelo (padrão: metade das CPUs)
#   -o DIR    diretório-raiz da saída (padrão: <repo>/data/raw)
#   -f        refaz tudo (apaga as execuções anteriores deste experimento)
#   -d        dry-run: mostra o plano e sai
#   -h        ajuda
#
# Saídas em <saída>/<experimento>/:
#   results.csv  todas as execuções consolidadas (uma linha por fluxo), com as
#                colunas experiment,varName,varValue antes das colunas do programa
#   batch.log    log: cabeçalho (versão do ns-3, commit, máquina, parâmetros) e
#                uma linha por execução (horário, protocolo, speed, run, duração)
#   runs/        um CSV por execução (intermediário; fora do git)
# =============================================================================
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"

# ----------------------------- valores padrão --------------------------------
EXP_NAME=""
PROTOCOLS="AODV OLSR DSDV"
SPEEDS="1 5 10 20"
RUNS_SPEC="100"
VAR=""
VAR_VALUES=""
EXTRA_ARGS=""
# Metade das CPUs: deixa folga para o Windows e para o uso interativo do
# computador. Cada simulação usa ~50 MB de RAM, então a CPU é o limite, não a
# memória.
JOBS=$(( $(nproc) / 2 )); [ "$JOBS" -lt 1 ] && JOBS=1
OUT_ROOT="$PROJECT_DIR/data/raw"
FORCE=0
DRY_RUN=0

usage() { sed -n '2,/^# =====/p' "$0" | sed 's/^# \{0,1\}//; /^=====/d'; exit "${1:-0}"; }
die()   { echo "[run_batch][ERRO] $*" >&2; exit 1; }

while getopts "n:p:s:r:v:V:x:j:o:fdh" opt; do
    case "$opt" in
        n) EXP_NAME="$OPTARG" ;;
        p) PROTOCOLS="$OPTARG" ;;
        s) SPEEDS="$OPTARG" ;;
        r) RUNS_SPEC="$OPTARG" ;;
        v) VAR="$OPTARG" ;;
        V) VAR_VALUES="$OPTARG" ;;
        x) EXTRA_ARGS="$OPTARG" ;;
        j) JOBS="$OPTARG" ;;
        o) OUT_ROOT="$OPTARG" ;;
        f) FORCE=1 ;;
        d) DRY_RUN=1 ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done

# ------------------------------- validação -----------------------------------
[ "$(id -u)" -ne 0 ] || die "não execute como root (o ns-3 recusa)."

case "$RUNS_SPEC" in
    *-*) RUN_FIRST="${RUNS_SPEC%-*}"; RUN_LAST="${RUNS_SPEC#*-}" ;;
    *)   RUN_FIRST=1; RUN_LAST="$RUNS_SPEC" ;;
esac
[[ "$RUN_FIRST" =~ ^[0-9]+$ && "$RUN_LAST" =~ ^[0-9]+$ && "$RUN_FIRST" -ge 1 && "$RUN_LAST" -ge "$RUN_FIRST" ]] \
    || die "-r inválido: '$RUNS_SPEC' (use N ou A-B, com runs >= 1)"
[[ "$JOBS" =~ ^[0-9]+$ && "$JOBS" -ge 1 ]] || die "-j inválido: '$JOBS'"

if [ -n "$VAR" ]; then
    # Só parâmetros que o programa aceita. "speed" já é variada por -s, e
    # "protocol"/"run" são os fatores do próprio desenho experimental.
    case "$VAR" in
        nodes|flows|packetSize|pauseTime|areaSize|pktRate|txPower|propagation) ;;
        *) die "-v inválido: '$VAR'" ;;
    esac
    [ -n "$VAR_VALUES" ] || die "-v exige -V com os valores"
    EXP_NAME="${EXP_NAME:-var-$VAR}"
    [ "$EXP_NAME" != "base" ] || die "a variável adicional não pode gravar em 'base' (dados devem ficar separados)"
else
    [ -z "$VAR_VALUES" ] || die "-V exige -v"
    EXP_NAME="${EXP_NAME:-base}"
    VAR_VALUES="NA" # um único "valor" fictício: o laço abaixo roda uma vez
fi

EXP_DIR="$OUT_ROOT/$EXP_NAME"
RUNS_DIR="$EXP_DIR/runs"
LOG="$EXP_DIR/batch.log"
RESULTS="$EXP_DIR/results.csv"

# ------------------------------ plano de jobs --------------------------------
# Uma linha por execução: "protocolo speed valorDaVariável run". A ordem é
# determinística; ela também define a ordem das linhas no results.csv.
JOBLIST="$(
    for p in $PROTOCOLS; do
        for s in $SPEEDS; do
            for v in $VAR_VALUES; do
                for r in $(seq "$RUN_FIRST" "$RUN_LAST"); do
                    echo "$p $s $v $r"
                done
            done
        done
    done
)"
TOTAL=$(echo "$JOBLIST" | wc -l)

echo "[run_batch] experimento: $EXP_NAME  ->  $EXP_DIR"
echo "[run_batch] protocolos: $PROTOCOLS | velocidades: $SPEEDS | runs: $RUN_FIRST..$RUN_LAST"
[ -n "$VAR" ] && echo "[run_batch] variável adicional: $VAR = $VAR_VALUES"
[ -n "$EXTRA_ARGS" ] && echo "[run_batch] argumentos fixos: $EXTRA_ARGS"
echo "[run_batch] total: $TOTAL execuções, $JOBS em paralelo"
[ "$DRY_RUN" -eq 1 ] && { sed -n '1,5p' <<< "$JOBLIST"; echo "..."; exit 0; }

# ------------------------------- compilação ----------------------------------
# Compila uma vez antes do lote, para garantir que o binário corresponde ao
# src/ atual. Depois chamamos o binário diretamente: N chamadas simultâneas ao
# "./ns3 run" disputariam o lock do sistema de build e seriam mais lentas.
echo "[run_batch] compilando manet-routing..."
( cd "$NS3_DIR" && ./ns3 build manet-routing >/dev/null ) || die "falha na compilação"
BIN="$(ls "$NS3_DIR"/build/scratch/manet/ns3.*-manet-routing-optimized 2>/dev/null | head -n1 || true)"
[ -x "$BIN" ] || die "binário optimized não encontrado (o ns-3 foi configurado com --build-profile=optimized?)"
NS3_VERSION="$(cat "$NS3_DIR/VERSION")"

# --------------------------- preparação da saída -----------------------------
mkdir -p "$EXP_DIR"
# Trava: impede dois lotes simultâneos no MESMO experimento (disputariam os
# mesmos arquivos). Experimentos diferentes podem rodar juntos.
exec 9> "$EXP_DIR/.lock"
flock -n 9 || die "já existe um lote rodando no experimento '$EXP_NAME'"

if [ "$FORCE" -eq 1 ]; then
    rm -rf "$RUNS_DIR" "$RESULTS"
fi
mkdir -p "$RUNS_DIR"

GIT_COMMIT="$(git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || echo desconhecido)"
GIT_DIRTY="$(git -C "$PROJECT_DIR" status --porcelain -- src 2>/dev/null | grep -q . && echo " (src/ com alterações não commitadas)" || true)"
{
    echo "# ============================================================"
    echo "# Lote iniciado em: $(date --iso-8601=seconds)"
    echo "# experimento: $EXP_NAME"
    echo "# ns-3: $NS3_VERSION | binário: $(basename "$BIN")"
    echo "# commit do repositório: $GIT_COMMIT$GIT_DIRTY"
    echo "# máquina: $(lscpu | sed -n 's/^Model name: *//p') | $(nproc) CPUs | $(uname -sr)"
    echo "# seed mestre: 12345 (fixa no programa) | runs: $RUN_FIRST..$RUN_LAST"
    echo "# protocolos: $PROTOCOLS | velocidades: $SPEEDS"
    echo "# variável adicional: ${VAR:-nenhuma} ${VAR:+= $VAR_VALUES}"
    echo "# argumentos fixos: ${EXTRA_ARGS:-nenhum} | paralelismo: $JOBS"
    echo "# formato: horário | status | protocolo speed variável run | duração"
} >> "$LOG"

# ----------------------------- uma execução ----------------------------------
# Cada execução grava o PRÓPRIO CSV. Se todas anexassem no mesmo arquivo ao
# mesmo tempo, linhas poderiam se misturar. O CSV é gravado num .tmp e renomeado
# só no sucesso: um arquivo final existente significa execução completa, o que
# permite retomar um lote interrompido sem refazer o que já terminou.
run_one() {
    local p="$1" s="$2" v="$3" r="$4"
    local tag="${p}_v${s}"
    local varArg=""
    if [ "$v" != "NA" ]; then tag="${tag}_${VAR}${v}"; varArg="--${VAR}=${v}"; fi
    tag="${tag}_r$(printf '%03d' "$r")"
    local final="$RUNS_DIR/$tag.csv"

    [ -s "$final" ] && return 0   # já concluída em um lote anterior

    local tmp="$RUNS_DIR/$tag.csv.tmp" t0 t1 status
    rm -f "$tmp"
    t0=$(date +%s.%N)
    # nice -n 10: prioridade baixa; o sistema operacional favorece os
    # programas interativos do usuário quando houver disputa por CPU.
    # shellcheck disable=SC2086
    if nice -n 10 "$BIN" --protocol="$p" --speed="$s" --run="$r" $varArg $EXTRA_ARGS \
            --outFile="$tmp" > "$RUNS_DIR/$tag.out" 2>&1; then
        mv "$tmp" "$final"; status="OK"
    else
        status="FALHA"
    fi
    t1=$(date +%s.%N)
    # Escritas pequenas com >> (O_APPEND) são atômicas: linhas de processos
    # paralelos não se misturam no log.
    printf '%s | %-5s | protocol=%s speed=%s %s run=%s | %.1f s\n' \
        "$(date --iso-8601=seconds)" "$status" "$p" "$s" "${varArg:---}" "$r" \
        "$(echo "$t1 - $t0" | bc)" >> "$LOG"
    [ "$status" = "OK" ]
}
export -f run_one
export BIN RUNS_DIR LOG VAR EXTRA_ARGS

# ------------------------------- execução ------------------------------------
DONE_BEFORE=$(find "$RUNS_DIR" -name '*.csv' | wc -l)
echo "[run_batch] já concluídas anteriormente: $DONE_BEFORE (serão puladas)"
echo "# execuções já concluídas antes deste lote (retomada): $DONE_BEFORE" >> "$LOG"
START=$(date +%s)

# Progresso: a cada 10 s, conta os CSVs finais. Fica numa linha só no
# terminal, em vez de 1200 linhas.
(
    while sleep 10; do
        n=$(find "$RUNS_DIR" -name '*.csv' | wc -l)
        el=$(( $(date +%s) - START ))
        printf '\r[run_batch] progresso: %d/%d (%d s)   ' "$n" "$TOTAL" "$el"
    done
) &
PROGRESS_PID=$!

# Interrupção (Ctrl+C ou fechamento do terminal): encerra também as simulações
# em andamento. Processos em segundo plano de um script ignoram o Ctrl+C, então
# o script mata explicitamente toda a árvore do lote (xargs -> workers ->
# simulações), e só ela: um "kill 0" atingiria também quem chamou o script.
# Execuções incompletas ficam só como .tmp e são refeitas na retomada.
kill_tree() {
    local child
    for child in $(pgrep -P "$1"); do kill_tree "$child"; done
    kill -TERM "$1" 2>/dev/null || true
}
on_interrupt() {
    trap - INT TERM
    kill_tree "$XARGS_PID"
    kill_tree "$PROGRESS_PID"
    echo; echo "[run_batch] interrompido — rode o mesmo comando de novo para retomar."
    echo "# Lote INTERROMPIDO em: $(date --iso-8601=seconds)" >> "$LOG"
    exit 130
}
trap on_interrupt INT TERM

# xargs -P: mantém exatamente $JOBS execuções simultâneas. Roda em segundo
# plano + wait, para o trap poder agir durante o lote. "|| true": uma falha não
# aborta o lote; as falhas são contadas e reportadas no fim.
xargs -P "$JOBS" -L 1 bash -c 'run_one "$@"' _ <<< "$JOBLIST" &
XARGS_PID=$!
wait "$XARGS_PID" || true

kill $PROGRESS_PID 2>/dev/null || true
trap - INT TERM
ELAPSED=$(( $(date +%s) - START ))
echo

# ------------------------------ consolidação ---------------------------------
# Junta os CSVs individuais num único results.csv, na ordem do plano, com
# colunas que identificam o experimento e a variável adicional. O programa só
# grava pauseTime e nodes; sem varName/varValue, uma variável como pktRate não
# apareceria nos dados.
# -print -quit em vez de "| head -n1": com pipefail, o head fecharia o pipe e o
# find (ainda listando 1200 arquivos) morreria por SIGPIPE, abortando o script.
FIRST_CSV="$(find "$RUNS_DIR" -name '*.csv' -print -quit)"
[ -n "$FIRST_CSV" ] || die "nenhuma execução concluída — veja os .out em $RUNS_DIR"
MISSING=0
{
    printf 'experiment,varName,varValue,'
    head -n1 "$FIRST_CSV"
    while read -r p s v r; do
        tag="${p}_v${s}"; [ "$v" != "NA" ] && tag="${tag}_${VAR}${v}"
        f="$RUNS_DIR/${tag}_r$(printf '%03d' "$r").csv"
        if [ -s "$f" ]; then
            tail -n +2 "$f" | sed "s/^/$EXP_NAME,${VAR:-none},$v,/"
        else
            MISSING=$((MISSING + 1))
        fi
    done <<< "$JOBLIST"
} > "$RESULTS"

LINES=$(( $(wc -l < "$RESULTS") - 1 ))
{
    echo "# Lote concluído em: $(date --iso-8601=seconds) | duração: ${ELAPSED} s"
    echo "# execuções completas: $((TOTAL - MISSING))/$TOTAL | linhas no results.csv: $LINES"
} >> "$LOG"

echo "[run_batch] concluído em ${ELAPSED} s: $((TOTAL - MISSING))/$TOTAL execuções completas"
echo "[run_batch] resultados: $RESULTS ($LINES linhas)"
echo "[run_batch] log: $LOG"
if [ "$MISSING" -gt 0 ]; then
    echo "[run_batch] ATENÇÃO: $MISSING execuções faltando/falharam — veja 'FALHA' no log e os .out em runs/" >&2
    exit 1
fi
