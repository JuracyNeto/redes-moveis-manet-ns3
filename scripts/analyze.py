#!/usr/bin/env python3
"""
analyze.py — Validação, estatística e gráficos dos resultados do manet-routing.

Lê data/raw/<experimento>/results.csv (gerado pelo run_batch.sh) e produz:
  data/processed/<experimento>/
      validacao.txt             relatório de validação dos dados brutos
      metricas_por_execucao.csv métricas de REDE por execução (4 fluxos agregados)
      estatisticas.csv          por configuração e métrica: n, média, dp, mín,
                                máx, IC95% (t-Student)
      tabela_resumo.csv         "média ± IC95%" por configuração (para o artigo)
      comparacoes_pareadas.csv  diferenças entre protocolos no mesmo run
  figures/<experimento>/        gráficos PNG (300 dpi) e PDF

Uso (a partir da raiz do repositório):
  .venv/bin/python scripts/analyze.py                      # experimento-base
  .venv/bin/python scripts/analyze.py --compare-speed 10   # outro comparativo
  .venv/bin/python scripts/analyze.py --exp var-pauseTime  # variável adicional
"""
import argparse
import sys
from itertools import combinations
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # gera arquivos sem precisar de interface gráfica
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from scipy import stats

ROOT = Path(__file__).resolve().parent.parent
PROTOCOLS = ["AODV", "OLSR", "DSDV"]

# Identidade visual: uma cor fixa por protocolo, sempre a mesma em todos os
# gráficos (cor segue a entidade). Paleta validada para daltonismo (as 3 cores
# são distinguíveis par a par). Marcador e tipo de linha diferentes por
# protocolo garantem a leitura também em impressão preto e branco.
STYLE = {
    "AODV": {"color": "#2a78d6", "marker": "o", "linestyle": "-"},
    "OLSR": {"color": "#eb6834", "marker": "s", "linestyle": "--"},
    "DSDV": {"color": "#1baf7a", "marker": "^", "linestyle": ":"},
}
TEXT = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
GRID = "#e3e2de"

# (coluna, nome curto, rótulo do eixo com unidade, casas decimais)
METRICS = [
    ("pdrPct", "PDR", "PDR (%)", 1),
    ("throughputMbps", "Throughput", "Throughput agregado da rede (Mb/s)", 4),
    ("delayMs", "Atraso", "Atraso médio fim a fim (ms)", 2),
    ("jitterMs", "Jitter", "Jitter médio (ms)", 2),
]

REQUIRED = [
    "experiment", "varName", "varValue", "protocol", "speed", "pauseTime", "nodes",
    "seed", "run", "flowId", "txPackets", "rxPackets", "lostPackets", "rxBytes",
    "throughputMbps", "pdrPct", "delayMs", "jitterMs",
]
CHANNEL_RATE_MBPS = 11.0  # taxa física do 802.11b: teto absoluto do throughput

# Rótulos (com unidade) das variáveis adicionais aceitas pelo run_batch.sh.
VAR_LABELS = {
    "pauseTime": "Tempo de pausa (s)",
    "nodes": "Número de nós",
    "flows": "Número de fluxos",
    "packetSize": "Tamanho do pacote (bytes)",
    "areaSize": "Lado da área quadrada (m)",
    "pktRate": "Taxa de envio por fluxo (pacotes/s)",
    "txPower": "Potência de transmissão (dBm)",
    "propagation": "Modelo de propagação",
}


# =============================================================================
# 1. Validação dos dados brutos
# Antes de qualquer gráfico, procurar valores impossíveis e verificar o número
# de execuções por configuração (slide 24). Erros graves interrompem a análise:
# um gráfico bonito de dados errados é pior do que nenhum gráfico.
# =============================================================================
def validate(df: pd.DataFrame, report: list) -> bool:
    errors = []

    def check(name, bad_mask):
        n = int(bad_mask.sum())
        report.append(f"[{'OK' if n == 0 else 'ERRO'}] {name}: {n} linha(s)")
        if n:
            errors.append(name)

    missing = [c for c in REQUIRED if c not in df.columns]
    if missing:
        report.append(f"[ERRO] colunas ausentes: {missing}")
        return False

    report.append(f"Linhas: {len(df)}")
    counts = ["txPackets", "rxPackets", "lostPackets", "rxBytes"]
    check("contadores negativos", (df[counts] < 0).any(axis=1))
    check("recebidos > transmitidos", df.rxPackets > df.txPackets)
    check("tx != rx + perdidos", df.txPackets != df.rxPackets + df.lostPackets)
    check("PDR fora de [0, 100]", (df.pdrPct < 0) | (df.pdrPct > 100))
    check("PDR inconsistente com rx/tx",
          (df.pdrPct - 100 * df.rxPackets / df.txPackets).abs() > 1e-3)
    check(f"throughput negativo ou acima da taxa do canal ({CHANNEL_RATE_MBPS} Mb/s)",
          (df.throughputMbps < 0) | (df.throughputMbps > CHANNEL_RATE_MBPS))
    check("atraso ou jitter negativo", (df.delayMs < 0) | (df.jitterMs < 0))
    # NA só é legítimo quando a métrica é indefinida (divisão por zero tratada).
    check("atraso NA com rx > 0 (ou numérico com rx = 0)",
          df.delayMs.isna() != (df.rxPackets == 0))
    check("jitter NA com rx >= 2 (ou numérico com rx < 2)",
          df.jitterMs.isna() != (df.rxPackets < 2))
    keys = ["experiment", "protocol", "speed", "varValue", "run", "flowId"]
    check("linhas duplicadas (mesma configuração, run e fluxo)", df.duplicated(keys))

    # Informativos: confirmam propriedades esperadas do desenho experimental.
    report.append(f"[INFO] fluxos sem recepção (atraso NA): {int(df.delayMs.isna().sum())}")
    report.append(f"[INFO] txPackets distintos (CBR determinístico => 1 valor): "
                  f"{sorted(df.txPackets.unique().tolist())}")
    with np.errstate(divide="ignore", invalid="ignore"):
        bpp = (df.rxBytes / df.rxPackets).dropna().round(6).unique()
    report.append(f"[INFO] bytes por pacote recebido (camada IP): {sorted(bpp.tolist())}")
    report.append(f"[INFO] seeds: {sorted(df.seed.unique().tolist())}")

    # Número de execuções e de fluxos por configuração: deve ser igual em todas,
    # senão as médias teriam precisões diferentes sem que isso apareça.
    cfg = ["protocol", "speed", "varValue"]
    runs = df.groupby(cfg).run.nunique()
    flows = df.groupby(cfg + ["run"]).flowId.nunique()
    report.append(f"[INFO] configurações: {len(runs)} | execuções por configuração: "
                  f"mín {runs.min()}, máx {runs.max()} | fluxos por execução: "
                  f"mín {flows.min()}, máx {flows.max()}")
    if runs.min() != runs.max():
        report.append("[ERRO] número de execuções difere entre configurações:")
        report.append(runs.to_string())
        errors.append("execuções desbalanceadas")
    if runs.min() < 5:
        report.append("[ERRO] menos de 5 execuções por configuração (mínimo do enunciado)")
        errors.append("execuções insuficientes")
    return not errors


# =============================================================================
# 2. Métricas de REDE por execução
# A unidade estatística é a execução: os 4 fluxos de um mesmo run compartilham
# o canal e a mobilidade, então não são amostras independentes. Tratar cada
# fluxo como amostra inflaria n e estreitaria artificialmente os ICs.
#   PDR        = soma rx / soma tx (todos os fluxos)
#   throughput = soma dos throughputs dos fluxos (vazão agregada da rede)
#   atraso     = média ponderada por pacotes recebidos (= delaySum total / rx total)
#   jitter     = média ponderada por (rx - 1), o número de pares consecutivos
# Fluxos sem recepção não entram no atraso (indefinido), mas entram no PDR
# e no throughput (como zero), porque a perda deles é real.
# =============================================================================
def per_run(df: pd.DataFrame) -> pd.DataFrame:
    keys = ["experiment", "varName", "varValue", "protocol", "speed", "pauseTime",
            "nodes", "seed", "run"]
    d = df.copy()
    d["_delayW"] = d.delayMs.fillna(0) * d.rxPackets
    d["_jitN"] = (d.rxPackets - 1).clip(lower=0).where(d.jitterMs.notna(), 0)
    d["_jitW"] = d.jitterMs.fillna(0) * d._jitN
    g = d.groupby(keys, sort=False).agg(
        flows=("flowId", "nunique"), txPackets=("txPackets", "sum"),
        rxPackets=("rxPackets", "sum"), lostPackets=("lostPackets", "sum"),
        rxBytes=("rxBytes", "sum"), throughputMbps=("throughputMbps", "sum"),
        _delayW=("_delayW", "sum"), _jitW=("_jitW", "sum"), _jitN=("_jitN", "sum"),
    ).reset_index()
    g["pdrPct"] = 100 * g.rxPackets / g.txPackets
    g["delayMs"] = (g._delayW / g.rxPackets).where(g.rxPackets > 0)
    g["jitterMs"] = (g._jitW / g._jitN).where(g._jitN > 0)
    return g.drop(columns=["_delayW", "_jitW", "_jitN"])


# =============================================================================
# 3. Estatística por configuração
# IC 95% pela t de Student: com n execuções, o desvio-padrão populacional é
# estimado pela amostra, e a t (n-1 graus de liberdade) corrige isso. Para
# n = 100, t ≈ 1,98, quase igual ao 1,96 da normal.
# =============================================================================
def summarize(runs: pd.DataFrame, group: list) -> pd.DataFrame:
    rows = []
    for key, sub in runs.groupby(group, sort=True):
        key = key if isinstance(key, tuple) else (key,)
        for col, name, _, _ in METRICS:
            x = sub[col].dropna().to_numpy()
            n = len(x)
            mean = x.mean() if n else np.nan
            sd = x.std(ddof=1) if n > 1 else np.nan   # ddof=1: desvio-padrão amostral
            half = stats.t.ppf(0.975, n - 1) * sd / np.sqrt(n) if n > 1 else np.nan
            # A mediana acompanha a média porque o atraso tem cauda longa: poucas
            # execuções com atraso muito alto (ex.: AODV segurando pacotes
            # durante a descoberta de rota) puxam a média para cima.
            rows.append({**dict(zip(group, key)), "metric": col, "n": n,
                         "n_excluded_na": len(sub) - n, "mean": mean,
                         "median": np.median(x) if n else np.nan, "std": sd,
                         "min": x.min() if n else np.nan, "max": x.max() if n else np.nan,
                         "ci95_half": half, "ci95_low": mean - half, "ci95_high": mean + half})
    return pd.DataFrame(rows)


def summary_table(st: pd.DataFrame, group: list) -> pd.DataFrame:
    """Tabela larga "média ± IC95%" por configuração, pronta para o artigo."""
    out = st[group].drop_duplicates().reset_index(drop=True)
    for col, name, _, dec in METRICS:
        s = st[st.metric == col].set_index(group)
        vals = s.apply(lambda r: f"{r['mean']:.{dec}f} ± {r['ci95_half']:.{dec}f}", axis=1)
        out[name] = out.set_index(group).index.map(vals)
    return out


# =============================================================================
# 4. Comparações pareadas entre protocolos
# Como os três protocolos usam a MESMA mobilidade em cada run (seeds comuns),
# comparamos run a run: diferença = métrica(A) - métrica(B) no mesmo run. A
# variação de mobilidade entre runs se cancela na diferença, então o IC fica
# mais estreito que o da comparação entre médias independentes. O p-valor é o
# do teste t pareado.
# =============================================================================
def paired(runs: pd.DataFrame, cond: list) -> pd.DataFrame:
    rows = []
    for key, sub in runs.groupby(cond, sort=True):
        key = key if isinstance(key, tuple) else (key,)
        wide = {p: sub[sub.protocol == p].set_index("run") for p in PROTOCOLS}
        for a, b in combinations(PROTOCOLS, 2):
            for col, name, _, _ in METRICS:
                pair = pd.concat([wide[a][col], wide[b][col]], axis=1, keys=[a, b]).dropna()
                diff = pair[a] - pair[b]
                n = len(diff)
                if n < 2:
                    continue
                half = stats.t.ppf(0.975, n - 1) * diff.std(ddof=1) / np.sqrt(n)
                p = stats.ttest_rel(pair[a], pair[b]).pvalue
                rows.append({**dict(zip(cond, key)), "metric": col, "A": a, "B": b, "n": n,
                             "mean_diff_A_minus_B": diff.mean(), "ci95_half": half,
                             "ci95_low": diff.mean() - half, "ci95_high": diff.mean() + half,
                             "p_value": p, "significant_5pct": p < 0.05})
    return pd.DataFrame(rows)


# =============================================================================
# 5. Gráficos
# Uma série por protocolo, barras de erro = IC 95% da média (slide 25: "inclua
# barras de erro quando apresentar médias de repetições"). Pequeno deslocamento
# horizontal entre protocolos para as barras de erro não se sobreporem.
# =============================================================================
def setup_style():
    plt.rcParams.update({
        "font.size": 10, "axes.edgecolor": TEXT_SECONDARY, "axes.labelcolor": TEXT,
        "xtick.color": TEXT_SECONDARY, "ytick.color": TEXT_SECONDARY,
        "axes.spines.top": False, "axes.spines.right": False,
        "axes.grid": True, "axes.grid.axis": "y", "grid.color": GRID, "grid.linewidth": 0.8,
        "axes.axisbelow": True,  # grade atrás dos dados, nunca por cima das barras
        "legend.frameon": False, "savefig.bbox": "tight", "savefig.dpi": 300,
    })


def save(fig, outdir: Path, name: str):
    for ext in ("png", "pdf"):
        fig.savefig(outdir / f"{name}.{ext}")
    plt.close(fig)


def line_chart(st, x, xlabel, metric, outdir, fname, title):
    col, name, ylabel, _ = metric
    s = st[st.metric == col].copy()
    xs = sorted(s[x].unique())
    # Variável textual (ex.: propagation = Friis/LogDistance): categorias em
    # posições 0, 1, 2... com os nomes como rótulos do eixo.
    categorical = not all(isinstance(v, (int, float, np.number)) for v in xs)
    if categorical:
        s["_x"] = s[x].map({v: i for i, v in enumerate(xs)})
        ticks = list(range(len(xs)))
    else:
        s["_x"] = s[x]
        ticks = xs
    span = (max(ticks) - min(ticks)) or 1
    dodge = {"AODV": -0.012 * span, "OLSR": 0.0, "DSDV": 0.012 * span}
    fig, ax = plt.subplots(figsize=(6.4, 4.0))
    ends = []
    for p in PROTOCOLS:
        d = s[s.protocol == p].sort_values("_x")
        if d.empty:
            continue
        # Categorias não têm "meio-termo": sem linha ligando os pontos.
        style = {**STYLE[p], "linestyle": "none"} if categorical else STYLE[p]
        ax.errorbar(d["_x"] + dodge[p], d["mean"], yerr=d["ci95_half"], label=p,
                    capsize=3, elinewidth=1, markersize=7, linewidth=2, **style)
        ends.append([d["mean"].iloc[-1], p, d["_x"].iloc[-1] + dodge[p]])
    # Rótulo direto no fim de cada linha: identidade sem depender só da cor.
    # Se as linhas terminam em valores próximos, os rótulos são afastados
    # verticalmente (espaço mínimo de 6% da escala) para não se sobreporem.
    lo, hi = ax.get_ylim()
    gap = 0.06 * (hi - lo)
    ends.sort()
    for i in range(1, len(ends)):
        ends[i][0] = max(ends[i][0], ends[i - 1][0] + gap)
    for y, p, xe in ends:
        ax.annotate(p, (xe, y), xytext=(10, 0), textcoords="offset points",
                    va="center", color=TEXT, fontsize=9)
    ax.set_xticks(ticks, [str(v) for v in xs] if categorical else ticks)
    xs = ticks
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title, color=TEXT, fontsize=11, loc="left")
    ax.set_xlim(min(xs) - 0.05 * span, max(xs) + 0.12 * span)
    ax.legend(loc="best")
    fig.text(0.0, -0.07, "Barras de erro: IC 95% da média (t-Student) sobre as execuções.",
             color=TEXT_SECONDARY, fontsize=8)
    save(fig, outdir, fname)


def compare_chart(st, cond_label, outdir, fname):
    """Comparativo agregado dos 3 protocolos numa condição de mobilidade."""
    fig, axes = plt.subplots(1, 4, figsize=(12, 3.4))
    for ax, (col, name, ylabel, dec) in zip(axes, METRICS):
        s = st[st.metric == col].set_index("protocol").reindex(PROTOCOLS)
        pos = np.arange(len(PROTOCOLS))
        # Barras começam em zero (comprimento = valor) e têm espaço entre si.
        ax.bar(pos, s["mean"], width=0.7, yerr=s["ci95_half"], capsize=4,
               color=[STYLE[p]["color"] for p in PROTOCOLS], edgecolor="white",
               linewidth=2, error_kw={"elinewidth": 1, "ecolor": TEXT})
        for i, p in enumerate(PROTOCOLS):
            top = s.loc[p, "mean"] + s.loc[p, "ci95_half"]
            ax.annotate(f"{s.loc[p, 'mean']:.{dec}f}", (i, top), xytext=(0, 5),
                        textcoords="offset points", ha="center", fontsize=8, color=TEXT)
        ax.set_xticks(pos, PROTOCOLS)
        ax.set_ylabel(ylabel, fontsize=9)
        ax.set_title(name, color=TEXT, fontsize=10, loc="left")
        ax.margins(y=0.15)
    fig.suptitle(f"Comparativo dos protocolos — {cond_label}", color=TEXT, fontsize=11,
                 x=0.0, ha="left")
    fig.text(0.0, -0.04, "Média de todas as execuções; barras de erro: IC 95% (t-Student).",
             color=TEXT_SECONDARY, fontsize=8)
    fig.tight_layout()
    save(fig, outdir, fname)


# =============================================================================
# Programa principal
# =============================================================================
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exp", default="base", help="experimento (pasta em data/raw/)")
    ap.add_argument("--compare-speed", type=float, default=20.0,
                    help="velocidade (m/s) do gráfico comparativo agregado (padrão: 20)")
    args = ap.parse_args()

    raw = ROOT / "data" / "raw" / args.exp / "results.csv"
    if not raw.exists():
        sys.exit(f"[analyze][ERRO] não encontrado: {raw}")
    proc = ROOT / "data" / "processed" / args.exp
    figs = ROOT / "figures" / args.exp
    proc.mkdir(parents=True, exist_ok=True)
    figs.mkdir(parents=True, exist_ok=True)

    # "NA" no CSV = métrica indefinida (ex.: atraso sem pacotes recebidos).
    # keep_default_na=False evita que o varValue "NA" do experimento-base vire NaN.
    df = pd.read_csv(raw, keep_default_na=False, na_values={"delayMs": ["NA"], "jitterMs": ["NA"]})
    for c in ["delayMs", "jitterMs", "pdrPct"]:
        df[c] = pd.to_numeric(df[c], errors="coerce")

    report = [f"Validação de {raw.relative_to(ROOT)}"]
    ok = validate(df, report)
    (proc / "validacao.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n".join(report))
    if not ok:
        sys.exit("[analyze][ERRO] validação falhou — corrija os dados antes de analisar.")

    runs = per_run(df)
    runs.to_csv(proc / "metricas_por_execucao.csv", index=False)

    # Eixo X: velocidade no experimento-base; valor da variável nos demais.
    var_name = df.varName.iloc[0]
    is_base = var_name == "none"
    if not is_base:
        try:  # valores numéricos (ex.: pauseTime) viram números para o eixo X
            runs["varValue"] = pd.to_numeric(runs.varValue)
        except ValueError:
            pass  # valores textuais (ex.: propagation=Friis/LogDistance)
    group = ["protocol", "speed"] if is_base else ["protocol", "speed", "varValue"]

    st = summarize(runs, group)
    st.to_csv(proc / "estatisticas.csv", index=False, float_format="%.6g")
    summary_table(st, group).to_csv(proc / "tabela_resumo.csv", index=False)
    paired(runs, group[1:]).to_csv(proc / "comparacoes_pareadas.csv", index=False,
                                   float_format="%.6g")

    setup_style()
    if is_base:
        for m in METRICS:
            line_chart(st, "speed", "Velocidade dos nós (m/s)", m, figs,
                       f"{m[1].lower()}_vs_velocidade", f"{m[1]} × velocidade")
        cs = st[st.speed == args.compare_speed]
        if cs.empty:
            sys.exit(f"[analyze][ERRO] --compare-speed {args.compare_speed} não existe nos dados")
        compare_chart(cs, f"{args.compare_speed:g} m/s", figs,
                      f"comparativo_{args.compare_speed:g}ms")
    else:
        for speed in sorted(st.speed.unique()):
            sub = st[st.speed == speed]
            for m in METRICS:
                label = VAR_LABELS.get(var_name, var_name)
                short = label.split(" (")[0].lower()  # "Tempo de pausa (s)" -> "tempo de pausa"
                line_chart(sub, "varValue", label, m, figs,
                           f"{m[1].lower()}_vs_{var_name}_v{speed:g}",
                           f"{m[1]} × {short} (velocidade {speed:g} m/s)")

    print(f"\n[analyze] tabelas em {proc.relative_to(ROOT)}/ | gráficos em {figs.relative_to(ROOT)}/")


if __name__ == "__main__":
    main()
