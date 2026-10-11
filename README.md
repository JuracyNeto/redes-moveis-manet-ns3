# Mobilidade e roteamento em MANET com ns-3: AODV × OLSR × DSDV

Trabalho final de **Redes Móveis**, Engenharia de Software, IFG Câmpus Inhumas, 2026/2.

Este repositório contém tudo o que é necessário para reproduzir o experimento:
código da simulação, scripts de execução em lote e de análise, dados brutos,
tabelas estatísticas e gráficos usados no artigo.

---

## 1. Objetivo

Avaliar, por simulação no **ns-3.48**, como a **velocidade dos nós** afeta o
desempenho de três protocolos de roteamento em uma rede móvel ad hoc (MANET):

- **AODV**, reativo: descobre rotas sob demanda;
- **OLSR**, proativo: estado de enlace com Multipoint Relays;
- **DSDV**, proativo: vetor de distância com números de sequência.

Métricas: **PDR** (taxa de entrega de pacotes), **throughput**, **atraso médio
fim a fim** e **jitter médio**.

Pergunta de pesquisa: *como diferentes níveis de mobilidade afetam o desempenho
de protocolos de roteamento em uma rede móvel ad hoc?*

Variável adicional do grupo: **número de nós** (10 a 100), a 20 m/s (seção 9).

---

## 2. Ambiente utilizado

| Item | Versão |
|---|---|
| Simulador | **ns-3.48** (lançado em 02/06/2026), tarball oficial |
| Sistema operacional | Ubuntu 24.04.5 LTS (WSL2 no Windows 11) |
| Compilador | g++ 13.3.0 |
| Build | CMake 3.28.3 + Ninja; perfil `optimized` com exemplos habilitados |
| Python (análise) | 3.12.3, com pandas 3.0.6, numpy 2.5.3, scipy 1.18.1, matplotlib 3.11.2 (`requirements.txt`) |
| Máquina | Intel Core i7-1255U (12 threads), 16 GB de RAM (7,6 GB visíveis no WSL) |

Requisitos mínimos do ns-3.48: g++ ≥ 11.1, CMake ≥ 3.25, Python ≥ 3.10.
Ubuntu 20.04 **não** atende; use Ubuntu 22.04 ou 24.04.

> **Observação:** o perfil `optimized` do ns-3 compila com `-march=native`
> (otimização para a CPU local). Com a mesma seed e o mesmo run, os resultados
> são idênticos na mesma máquina; em outra CPU podem diferir minimamente, por
> causa de arredondamentos de ponto flutuante.

---

## 3. Instalação

### 3.1 (Somente Windows) WSL com Ubuntu 24.04

No PowerShell:

```bash
wsl --install -d Ubuntu-24.04
```

Crie um **usuário comum** quando solicitado. O ns-3 se recusa a rodar como root.
A partir daqui, todos os comandos são executados **dentro do Ubuntu**.

### 3.2 Dependências (requer sudo)

```bash
sudo apt update && sudo apt install -y build-essential g++ cmake ninja-build python3 python3-dev python3-venv python3-pip pkg-config bzip2 wget ca-certificates git
```

### 3.3 Clonar o repositório

```bash
git clone https://github.com/JuracyNeto/redes-moveis-manet-ns3.git ~/manet-ns3
```

### 3.4 Instalar e compilar o ns-3.48

```bash
cd ~/manet-ns3 && ./scripts/setup_ns3.sh
```

O script [`scripts/setup_ns3.sh`](scripts/setup_ns3.sh):

1. verifica as dependências e as versões mínimas;
2. baixa `https://www.nsnam.org/releases/ns-3.48.tar.bz2` e extrai em `~/ns-3`;
3. configura: `./ns3 configure --build-profile=optimized --enable-examples`;
4. compila: `./ns3 build` (~25 min; o número de processos é limitado pela RAM);
5. cria o link simbólico `~/ns-3/scratch/manet -> ~/manet-ns3/src`.

**Nenhum arquivo do ns-3 é modificado.** O código do trabalho entra no ns-3
apenas pelo link simbólico em `scratch/`. O script pode ser executado de novo
com segurança, porque pula etapas já concluídas. Variáveis opcionais:
`NS3_DIR` (padrão `~/ns-3`) e `JOBS` (processos de compilação).

### 3.5 Ambiente Python da análise

```bash
cd ~/manet-ns3 && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

---

## 4. Compilação do programa da simulação

O `setup_ns3.sh` já compila tudo. Após alterar `src/manet-routing.cc`:

```bash
cd ~/ns-3 && ./ns3 build manet-routing
```

---

## 5. Execução

### 5.1 Um cenário

```bash
cd ~/ns-3 && ./ns3 run "manet-routing --protocol=OLSR --speed=20 --run=1 --outFile=$HOME/teste.csv"
```

Cada execução simula um cenário (protocolo, velocidade, run) e **anexa** uma
linha por fluxo ao CSV indicado. No terminal, imprime um resumo: o protocolo
efetivamente instalado, as métricas de cada fluxo e uma "assinatura" da
mobilidade, que é idêntica para os três protocolos no mesmo run.

Exemplo de saída:

```
== manet-routing | protocolo=OLSR speed=20 m/s run=1 seed=12345
   roteamento instalado (nó 0): ns3::olsr::RoutingProtocol
   fluxo 1 10.1.1.1->10.1.1.20: tx=379 (aceitos pela rede=..., vistos pelo FlowMonitor=...) rx=... PDR=...%
   ...
   assinatura-mobilidade: 4430.663733 3961.919890
```

Para gravar as tabelas de rota (validação de que o protocolo está ativo),
use `--routesFile=rotas.txt`. As tabelas são gravadas em t = 20 s e t = 30 s.

### 5.2 Experimento-base completo (1200 execuções, ~16 min)

```bash
cd ~/manet-ns3 && ./scripts/run_batch.sh
```

### 5.3 Variável adicional: número de nós (3000 execuções, ~21 h com 10 processos)

```bash
cd ~/manet-ns3 && ./scripts/run_batch.sh -v nodes -V "10 20 30 40 50 60 70 80 90 100" -s 20 -r 100 -j 10
```

### 5.4 Opções do `run_batch.sh`

| Opção | Significado | Padrão |
|---|---|---|
| `-n NOME` | nome do experimento (pasta em `data/raw/`) | `base` ou `var-<parâmetro>` |
| `-p LISTA` | protocolos | `"AODV OLSR DSDV"` |
| `-s LISTA` | velocidades (m/s) | `"1 5 10 20"` |
| `-r RUNS` | `N` (runs 1..N) ou `A-B` | `100` |
| `-v PARAM` / `-V LISTA` | parâmetro adicional e seus valores | — |
| `-x ARGS` | argumentos fixos extras para todas as execuções | — |
| `-j N` | execuções em paralelo | metade das CPUs |
| `-f` | refaz tudo (apaga as execuções anteriores do experimento) | — |
| `-d` | dry-run: mostra o plano sem executar | — |

Características:
- **Uso moderado de recursos:** por padrão usa metade das CPUs, e as simulações
  rodam com prioridade baixa (`nice`). Cada simulação usa de ~50 MB (20 nós) a
  ~240 MB (100 nós) de RAM.
- **Retomável:** cada execução grava o próprio CSV, de forma atômica. Se o lote
  for interrompido (Ctrl+C, suspensão, reinício), rodar o mesmo comando continua
  de onde parou.
- **Seguro:** uma trava impede dois lotes simultâneos no mesmo experimento, e a
  variável adicional nunca grava na pasta `base/`.

### 5.5 Análise estatística e gráficos (~5 s)

```bash
cd ~/manet-ns3 && .venv/bin/python scripts/analyze.py
```

```bash
cd ~/manet-ns3 && .venv/bin/python scripts/analyze.py --exp var-nodes
```

`--compare-speed V` escolhe a velocidade do gráfico comparativo agregado
(padrão: 20 m/s).

---

## 6. Parâmetros da simulação

Todos são argumentos de linha de comando do `manet-routing`. Os valores padrão
reproduzem o cenário-base do enunciado.

| Parâmetro | Significado | Unidade | Padrão |
|---|---|---|---|
| `--protocol` | protocolo de roteamento: `AODV`, `OLSR` ou `DSDV` | — | `AODV` |
| `--speed` | velocidade **fixa** dos nós (> 0) | m/s | 10 |
| `--run` | número da repetição (`RngSeedManager::SetRun`) | — | 1 |
| `--simTime` | duração total da simulação | s | 120 |
| `--nodes` | número de nós | — | 20 |
| `--flows` | fluxos CBR simultâneos (≤ nodes/2) | — | 4 |
| `--packetSize` | carga UDP por pacote | bytes | 1024 |
| `--pauseTime` | pausa do Random Waypoint | s | 0 |
| `--areaSize` | lado da área quadrada | m | 500 |
| `--pktRate` | taxa de envio de cada fluxo | pacotes/s | 4 |
| `--txPower` | potência de transmissão | dBm | 6 |
| `--propagation` | modelo de perda: `Friis` ou `LogDistance` | — | `Friis` |
| `--warmup` | aquecimento antes do início do tráfego | s | 20 |
| `--drain` | escoamento entre o fim do tráfego e o fim da simulação | s | 5 |
| `--outFile` | CSV de saída (linhas são anexadas) | — | `manet-routing.csv` |
| `--routesFile` | arquivo das tabelas de rota (vazio = não grava) | — | — |

### 6.1 Cenário-base

| Item | Configuração |
|---|---|
| Rede | IEEE 802.11b em modo ad hoc, canal 1 (2,412 GHz) |
| Taxa física | 11 Mb/s fixa (DSSS/CCK, `ConstantRateWifiManager`), inclusive para broadcast |
| Propagação | Friis (espaço livre) a 2,412 GHz + atraso à velocidade da luz |
| Potência / alcance | 6 dBm → alcance medido de ~250 m (100% de entrega até 245 m, 0% a 250 m) |
| Nós / área | 20 nós em 500 × 500 m |
| Mobilidade | Random Waypoint, velocidade fixa de 1, 5, 10 ou 20 m/s, pausa de 0 s |
| Tráfego | 4 fluxos UDP CBR: nó 0→19, 1→18, 2→17, 3→16; 4 pacotes/s de 1024 B |
| Tempo | 120 s: aquecimento de 0–20 s, tráfego de ~20–115 s, escoamento de 5 s |
| Repetições | 100 runs por configuração (seed mestre 12345) |

---

## 7. Decisões metodológicas (e por quê)

1. **Velocidade fixa no Random Waypoint (mínimo = máximo).** Com uma faixa
   larga, como U(0, 20) m/s, a velocidade média cai ao longo da simulação: nós
   lentos demoram mais para cruzar a área e passam a dominar a amostra (Yoon,
   Liu e Noble, 2003). Com velocidade fixa, "10 m/s" vale para toda a simulação.

2. **Frequência do Friis fixada em 2,412 GHz.** O modelo Friis do ns-3 usa
   5,15 GHz por padrão e **não herda** a frequência do Wi-Fi. Com 802.11b, isso
   reduzia o alcance de ~295 m para 135 m (medido). O exemplo oficial
   `manet-routing-compare.cc` tem essa inconsistência.

3. **Potência de 6 dBm (alcance de ~250 m).** 250 m é o alcance nominal clássico
   dos estudos de MANET (Broch et al., 1998), o que facilita a comparação com a
   literatura. A área de 500 × 500 m (diagonal de 707 m) exige rotas de
   múltiplos saltos.

4. **Mesma taxa física para broadcast e unicast.** As mensagens de controle dos
   protocolos são broadcast; por padrão iriam a 1 Mb/s, com alcance maior que o
   dos dados. Haveria vizinhos "fantasmas", e rotas descobertas não
   funcionariam para os dados.

5. **Aplicação CBR própria, em vez do `OnOffApplication`.** Com OLSR e DSDV,
   quando não há rota, o socket UDP recusa o pacote **antes** da camada IP, e o
   FlowMonitor não o conta como transmitido. O PDR desses protocolos seria
   superestimado (observado: 379 tentativas contra 265 pacotes vistos pelo
   FlowMonitor em um fluxo do OLSR). A aplicação própria conta **toda tentativa
   de envio**, e esse é o denominador do PDR, igual para os três protocolos.

6. **Taxa de 4 pacotes/s por fluxo (~135 kb/s no total).** Medido a 1 m/s: o PDR
   fica estável de 1 a 8 pacotes/s e só cai a partir de 16–32 pacotes/s. As
   perdas refletem mobilidade e roteamento, não congestionamento.

7. **Aquecimento de 20 s.** Os protocolos proativos precisam montar as tabelas
   (OLSR: HELLO a cada 2 s e TC a cada 5 s; DSDV: atualização completa a cada
   15 s). Em t = 20 s, OLSR e DSDV já tinham rotas para praticamente todos os 19
   destinos de cada nó. O AODV usa a mesma janela, para os três serem comparados
   em condições idênticas.

8. **Escoamento de 5 s.** Pacotes enviados nos últimos instantes precisam de
   tempo para chegar. Sem isso, seriam contados como perdas artificiais.

9. **Mesma mobilidade para os três protocolos (seeds comuns).** A mobilidade usa
   substreams fixas do gerador (`AssignStreams`). No mesmo run, as trajetórias
   são idênticas nos três protocolos, o que é verificado pela "assinatura" no
   terminal. Isso torna a comparação justa e permite a **análise pareada**.

10. **Apenas os fluxos de dados.** O FlowMonitor também classifica o tráfego de
    controle (por exemplo, a porta UDP 654 do AODV). As métricas usam apenas os
    fluxos com porta de destino 9000 (a aplicação).

11. **100 runs por configuração** (o enunciado exige ≥ 5). Com 5 runs, a
    meia-largura do IC 95% do PDR chegava a ±21 pontos percentuais, e diferenças
    reais entre protocolos ficavam inconclusivas. Com 100 runs, fica entre
    ±0,2 e ±2,2 pp no experimento-base.

---

## 8. Aleatoriedade e reprodutibilidade

- **Seed mestre:** `12345`, fixa no código (`RngSeedManager::SetSeed`).
- **Runs:** `--run` = 1..100 (`RngSeedManager::SetRun`). Cada run é uma
  substream independente do gerador.
- **Política:** a mesma seed e os mesmos runs são usados para todos os
  protocolos e valores de parâmetro.
- **Determinismo:** a mesma configuração (seed + run + parâmetros) produz um
  resultado idêntico, o que foi verificado.
- Cada linha do CSV registra `seed` e `run`.

---

## 9. Variável adicional: número de nós

**Por que o número de nós:** a densidade afeta de forma diferente as duas
estratégias de roteamento. Os protocolos proativos mantêm rotas para *todos* os
nós, e o custo de controle cresce com a rede. O AODV só busca as rotas de que
precisa, mas suas descobertas usam broadcast, que em redes densas pode
congestionar o canal.

| Item | Configuração |
|---|---|
| Valores | 10, 20, 30, …, 100 nós |
| Velocidade | 20 m/s, fixa (maior estresse de mobilidade) |
| Demais parâmetros | iguais ao cenário-base (área de 500 × 500 m, 4 fluxos etc.) |
| Repetições | 100 runs por configuração → 3000 execuções |
| Dados | `data/raw/var-nodes/`, separados do experimento-base |

**Custo computacional:** o tempo de simulação cresce aproximadamente com o cubo
do número de nós. Com 100 nós, uma execução levou de ~8 min (DSDV) a ~25–30 min
(OLSR). O lote completo levou 21 h 06 min com 10 processos em paralelo
(i7-1255U). Resultados: seção 12.2.

---

## 10. Saídas

### 10.1 Arquivos

| Caminho | Conteúdo |
|---|---|
| `data/raw/<exp>/results.csv` | dados brutos: uma linha por fluxo de cada execução |
| `data/raw/<exp>/batch.log` | log do lote: versão do ns-3, commit, máquina, parâmetros e uma linha por execução (horário, protocolo, speed, run, duração, status) |
| `data/raw/<exp>/runs/` | um CSV por execução (intermediário, fora do git) |
| `data/processed/<exp>/validacao.txt` | relatório de validação dos dados brutos |
| `data/processed/<exp>/metricas_por_execucao.csv` | métricas de rede por execução (4 fluxos agregados) |
| `data/processed/<exp>/estatisticas.csv` | por configuração e métrica: n, média, mediana, dp, mín, máx, IC 95% |
| `data/processed/<exp>/tabela_resumo.csv` | "média ± IC 95%" por configuração |
| `data/processed/<exp>/comparacoes_pareadas.csv` | diferenças entre protocolos no mesmo run (IC 95% e p-valor) |
| `figures/<exp>/*.png` / `*.pdf` | gráficos (PNG a 300 dpi e PDF vetorial) |

### 10.2 Colunas do `results.csv`

| Coluna | Descrição |
|---|---|
| `experiment`, `varName`, `varValue` | experimento e variável adicional (`none`/`NA` no experimento-base) |
| `protocol`, `speed`, `pauseTime`, `nodes` | configuração simulada |
| `seed`, `run` | aleatoriedade |
| `flowId` | índice do fluxo (1..4), estável entre execuções |
| `txPackets` | tentativas de envio da aplicação (carga oferecida) |
| `rxPackets` | pacotes entregues ao destino |
| `lostPackets` | `txPackets − rxPackets` |
| `rxBytes` | bytes recebidos na camada IP (1052 B por pacote: 1024 + 28 de cabeçalhos IP/UDP) |
| `throughputMbps` | `rxBytes × 8 / duração do tráfego` (Mb/s; janela de 94,7 s, igual para todos os fluxos) |
| `pdrPct` | `rxPackets / txPackets × 100` (%) |
| `delayMs` | `delaySum / rxPackets` (ms); `NA` se nenhum pacote foi recebido |
| `jitterMs` | `jitterSum / (rxPackets − 1)` (ms); `NA` se menos de 2 pacotes foram recebidos |

---

## 11. Análise

O [`scripts/analyze.py`](scripts/analyze.py) segue estes passos:

1. **Validação** (interrompe a análise se algo falhar): contadores negativos,
   recebidos > enviados, `tx ≠ rx + perdidos`, PDR fora de [0, 100] ou
   inconsistente, throughput acima da taxa do canal, `NA` fora dos casos
   indefinidos, linhas duplicadas, número de execuções diferente entre
   configurações ou menor que 5.
2. **Unidade estatística = execução.** Os 4 fluxos de um run compartilham a rede
   e não são independentes, então são agregados em métricas de rede:
   - PDR = Σrx / Σtx;
   - throughput = soma dos fluxos;
   - atraso médio ponderado pelos pacotes recebidos;
   - jitter médio ponderado pelos pares consecutivos.
3. **Estatística por configuração:** média, mediana, desvio-padrão amostral,
   mínimo, máximo e IC 95% pela t de Student.
4. **Comparações pareadas:** diferença entre protocolos no mesmo run (mesma
   mobilidade), com IC 95% e teste t pareado.
5. **Gráficos:** uma série por protocolo, barras de erro = IC 95%. Cores
   validadas para daltonismo e marcadores e tipos de linha distintos (legíveis
   em preto e branco).

Gráficos do experimento-base (`figures/base/`): `pdr_vs_velocidade`,
`throughput_vs_velocidade`, `atraso_vs_velocidade`, `jitter_vs_velocidade` e
`comparativo_20ms` (os 3 protocolos a 20 m/s).

**Por que 20 m/s no comparativo:** é a condição de maior estresse de
mobilidade. Nela, as diferenças pareadas entre os três protocolos são as
maiores e todas estatisticamente significativas, o que evidencia o contraste
entre as estratégias reativa e proativa.

---

## 12. Resultados resumidos

### 12.1 Experimento-base (média ± IC 95%, 100 execuções por configuração)

| Protocolo | Velocidade (m/s) | PDR (%) | Throughput (Mb/s) | Atraso (ms) | Jitter (ms) |
|---|---|---|---|---|---|
| AODV | 1 | 94,5 ± 1,5 | 0,1273 ± 0,0020 | 4,22 ± 0,50 | 2,78 ± 0,48 |
| AODV | 5 | 91,8 ± 1,4 | 0,1237 ± 0,0019 | 5,77 ± 1,06 | 4,50 ± 0,68 |
| AODV | 10 | 88,3 ± 1,6 | 0,1189 ± 0,0021 | 8,11 ± 3,22 | 5,56 ± 0,72 |
| AODV | 20 | 82,2 ± 1,8 | 0,1108 ± 0,0024 | 12,06 ± 4,82 | 7,13 ± 0,84 |
| OLSR | 1 | 99,5 ± 0,2 | 0,1340 ± 0,0003 | 1,82 ± 0,14 | 0,33 ± 0,09 |
| OLSR | 5 | 93,4 ± 1,0 | 0,1259 ± 0,0013 | 1,95 ± 0,19 | 0,66 ± 0,14 |
| OLSR | 10 | 85,2 ± 1,5 | 0,1147 ± 0,0020 | 3,09 ± 0,48 | 1,73 ± 0,37 |
| OLSR | 20 | 70,6 ± 1,9 | 0,0951 ± 0,0026 | 6,92 ± 1,25 | 4,82 ± 0,90 |
| DSDV | 1 | 98,9 ± 0,3 | 0,1333 ± 0,0005 | 2,58 ± 0,28 | 1,18 ± 0,24 |
| DSDV | 5 | 90,0 ± 1,2 | 0,1213 ± 0,0017 | 2,67 ± 0,31 | 1,55 ± 0,27 |
| DSDV | 10 | 80,4 ± 1,8 | 0,1083 ± 0,0024 | 2,69 ± 0,38 | 1,53 ± 0,33 |
| DSDV | 20 | 66,3 ± 2,2 | 0,0893 ± 0,0030 | 3,47 ± 0,58 | 1,96 ± 0,37 |

Principais observações:
- **A mobilidade degrada a entrega nos três protocolos.**
- **Os proativos são melhores em baixa mobilidade, e o AODV em alta.** A 1 m/s,
  o OLSR entrega 99,5% e o AODV 94,5%. As curvas se cruzam entre 5 e 10 m/s, e
  a 20 m/s o AODV supera o OLSR em +11,6 pp e o DSDV em +16,0 pp (diferenças
  pareadas, p < 10⁻²⁴).
- **O AODV entrega mais à custa de atraso e jitter maiores**, porque mantém os
  pacotes em fila enquanto descobre as rotas. O atraso dele tem cauda longa: a
  20 m/s, a média é 12,1 ms e a mediana 6,5 ms.
- **O throughput acompanha o PDR**, porque a carga oferecida é fixa (CBR).

### 12.2 Variável adicional: número de nós (20 m/s, 100 execuções por configuração)

**PDR (%)** (média ± IC 95%)

| Nós | AODV | OLSR | DSDV |
|---|---|---|---|
| 10 | 84,9 ± 1,6 | 66,6 ± 2,2 | 64,8 ± 2,1 |
| 20 | 82,2 ± 1,8 | 70,6 ± 1,9 | 66,3 ± 2,2 |
| 30 | 81,5 ± 1,8 | 72,7 ± 2,2 | 68,9 ± 2,5 |
| 40 | 80,5 ± 1,5 | 74,7 ± 2,1 | 70,9 ± 2,2 |
| 50 | 76,9 ± 1,4 | 75,0 ± 2,0 | 71,5 ± 2,2 |
| 60 | 74,3 ± 1,7 | 75,4 ± 2,2 | 70,6 ± 2,5 |
| 70 | 67,3 ± 1,9 | 73,1 ± 1,8 | 67,9 ± 2,1 |
| 80 | 64,9 ± 1,9 | 75,4 ± 1,9 | 70,3 ± 2,1 |
| 90 | 61,7 ± 2,0 | 74,5 ± 2,1 | 67,4 ± 2,6 |
| 100 | 60,0 ± 2,3 | 74,3 ± 2,2 | 67,7 ± 2,3 |

**Atraso médio (ms)** (média ± IC 95%)

| Nós | AODV | OLSR | DSDV |
|---|---|---|---|
| 10 | 30,5 ± 8,9 | 4,5 ± 0,7 | 3,0 ± 0,6 |
| 20 | 12,1 ± 4,8 | 6,9 ± 1,3 | 3,5 ± 0,6 |
| 30 | 12,1 ± 1,1 | 7,1 ± 0,9 | 5,0 ± 1,0 |
| 40 | 20,3 ± 1,3 | 7,3 ± 1,1 | 4,7 ± 0,6 |
| 50 | 28,7 ± 2,2 | 8,5 ± 1,1 | 6,2 ± 0,7 |
| 60 | 35,5 ± 2,8 | 9,5 ± 1,1 | 7,8 ± 1,0 |
| 70 | 42,5 ± 2,9 | 11,4 ± 1,6 | 9,1 ± 0,8 |
| 80 | 47,8 ± 3,2 | 11,0 ± 1,2 | 11,2 ± 1,0 |
| 90 | 51,9 ± 3,2 | 14,2 ± 1,5 | 14,1 ± 1,1 |
| 100 | 56,8 ± 3,6 | 15,4 ± 1,6 | 15,8 ± 1,1 |

Tabelas completas (throughput, jitter, mediana etc.) estão em
`data/processed/var-nodes/`, e os gráficos em `figures/var-nodes/`.

Principais observações:
- **A densidade tem efeito oposto sobre as duas estratégias.** O PDR do AODV
  cai continuamente com mais nós: de 84,9% com 10 nós para 60,0% com 100. O
  OLSR e o DSDV melhoram de 10 para ~40–60 nós (mais conectividade, menos
  partições) e depois se estabilizam (OLSR ~74–75%, DSDV ~67–71%).
- **As curvas se cruzam entre 50 e 70 nós.** Com 50 e 60 nós, a diferença entre
  AODV e OLSR não é significativa (p = 0,06 e p = 0,26). Com 70 nós, o OLSR já
  supera o AODV (+5,7 pp, p < 10⁻⁵), e com 100 nós a vantagem chega a +14,3 pp
  (p < 10⁻²¹). Com 10 nós, ao contrário, o AODV supera o OLSR em +18,3 pp.
- **O atraso do AODV cresce muito com a densidade:** de ~12 ms (20–30 nós) para
  56,8 ms com 100 nós, contra ~15 ms dos proativos. Uma explicação compatível
  com o funcionamento dos protocolos é o custo das descobertas de rota: o RREQ é
  inundado por broadcast e, com 100 nós ao alcance uns dos outros, isso disputa
  o canal. O OLSR limita a retransmissão do controle aos MPRs. O tráfego de
  controle não foi medido neste trabalho, então essa explicação é uma hipótese.
- **Com 10 nós (rede esparsa), o AODV tem atraso alto e variável** (30,5 ± 8,9
  ms): com partições frequentes, os pacotes esperam na fila até a rota ser
  (re)descoberta.
- **Verificação cruzada:** a configuração de 20 nós a 20 m/s aparece nos dois
  experimentos e produziu resultados idênticos (mesmas seeds e runs), o que
  confirma a reprodutibilidade.

---

## 13. Limitações

- **Propagação em espaço livre (Friis):** é otimista, sem obstáculos nem
  desvanecimento, o que gera um alcance com corte brusco.
- **O Random Waypoint não representa padrões reais de movimento**, e a
  distribuição espacial dos nós não é uniforme (há concentração no centro da
  área).
- **Tráfego CBR de baixa taxa:** os resultados não se estendem diretamente a
  cargas altas ou a TCP.
- **Implementações do ns-3:** AODV, OLSR (RFC 3626) e DSDV, com parâmetros padrão.
- **Variável adicional avaliada em uma única velocidade** (20 m/s).
- **O tráfego de controle (overhead de roteamento) não foi medido.** As
  explicações para o efeito da densidade, como a inundação de RREQ do AODV e os
  MPRs do OLSR, são hipóteses compatíveis com os dados, não medições diretas.

---

## 14. Estrutura de diretórios

```
manet-ns3/
├── README.md                 este arquivo
├── requirements.txt          dependências Python (versões fixadas)
├── docs/                     teste_reproducao.log (evidência do teste do slide 29)
├── src/
│   └── manet-routing.cc      programa da simulação (link em ~/ns-3/scratch/manet)
├── scripts/
│   ├── setup_ns3.sh          instala e compila o ns-3.48
│   ├── run_batch.sh          execução em lote (retomável, paralela)
│   └── analyze.py            validação, estatística e gráficos
├── data/
│   ├── raw/
│   │   ├── base/             experimento-base: results.csv + batch.log
│   │   └── var-nodes/        variável adicional: results.csv + batch.log
│   └── processed/            tabelas estatísticas (por experimento)
├── figures/                  gráficos PNG/PDF (por experimento)
├── logs/                     saídas de terminal (fora do git)
└── article/                  artigo SBC
```

---

## 15. Rastreabilidade

- O `batch.log` de cada experimento registra o **commit do repositório** usado
  para gerar os dados. Antes da publicação, o histórico local foi reescrito
  para remover arquivos que não fazem parte do repositório público (material da
  disciplina e anotações locais). Por isso, os identificadores registrados nos
  logs mudaram. A correspondência é:

  | Log | Commit registrado (histórico local) | Commit equivalente no GitHub |
  |---|---|---|
  | `data/raw/base/batch.log` | `cf0015a` (Fase 2) | `c3f3fa4` |
  | `data/raw/var-nodes/batch.log` | `15bfa84` (Fase 4) | `c88b04f` |

  O conteúdo de `src/` é idêntico nos dois casos e não mudou desde a Fase 2.
- A versão do repositório correspondente ao artigo entregue é marcada com a tag
  `v1.0-artigo`. Tags `v*` são protegidas: não podem ser apagadas nem movidas, e
  uma revisão posterior recebe uma tag nova (ex.: `v1.1-artigo`).
- **Teste de reprodução (slide 29), feito em 10/10/2026:** a partir de uma cópia
  limpa do repositório e de uma instalação **nova** do ns-3.48, seguindo apenas
  este README, todas as etapas funcionaram:
  - a instalação e a compilação terminaram em 22 min;
  - um cenário isolado e um lote de 30 execuções reproduziram **exatamente** as
    linhas correspondentes de `data/raw/base/results.csv`;
  - a análise regenerou tabelas **idênticas** às do repositório.

  O log completo está em [`docs/teste_reproducao.log`](docs/teste_reproducao.log).

---

## 16. Referências

- ns-3 Tutorial, Manual e Model Library (AODV, OLSR, DSDV, Flow Monitor):
  https://www.nsnam.org/documentation/
- Perkins, C.; Belding-Royer, E.; Das, S. *Ad hoc On-Demand Distance Vector
  (AODV) Routing*. RFC 3561, 2003.
- Clausen, T.; Jacquet, P. *Optimized Link State Routing Protocol (OLSR)*.
  RFC 3626, 2003.
- Perkins, C.; Bhagwat, P. *Highly Dynamic Destination-Sequenced Distance-Vector
  Routing (DSDV) for Mobile Computers*. ACM SIGCOMM, 1994.
- Broch, J. et al. *A Performance Comparison of Multi-Hop Wireless Ad Hoc
  Network Routing Protocols*. ACM MobiCom, 1998.
- Yoon, J.; Liu, M.; Noble, B. *Random Waypoint Considered Harmful*.
  IEEE INFOCOM, 2003.
