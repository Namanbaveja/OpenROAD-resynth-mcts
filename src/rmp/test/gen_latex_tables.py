#!/usr/bin/env python3
"""Generate LaTeX tables from MCTS/SA benchmark logs."""

import re, os

BASE = "/home/mani-deep-g/OpenROAD/src/rmp/test"
DESIGNS_MCTS = ["gcd", "spi", "uart", "usb_phy", "sasc_top", "b13", "i2c_master_top"]
DESIGNS_SA   = ["gcd", "spi", "uart", "usb_phy", "sasc_top", "b13", "i2c_master_top"]
DESIGNS      = DESIGNS_SA   # for comparison table (shared designs only)
PCTS = [10, 15, 20]
SEED = 2345
ITERS = 1000

DESIGN_LABELS = {
    "gcd":           "GCD",
    "spi":           "SPI",
    "uart":          "UART",
    "usb_phy":       "USB-PHY",
    "sasc_top":      "SASC",
    "b13":           "B13",
    "i2c_master_top":"I2C",
    "b12":           "B12",
}

# --- parsing -------------------------------------------------------------------

def parse_log(path):
    """Return dict with keys: baseline, repair_only, restructure_only,
    restructure_repair (each has wns_ps, tns_ps, viols), plus elapsed_s."""
    if not os.path.exists(path):
        return None
    with open(path) as f:
        text = f.read()

    stage_re = re.compile(
        r"QOR_STAGE stage=(\w+)\s+wns_ps=([-\d.]+)\s+tns_ps=([-\d.]+)\s+viols=(\d+)"
    )
    result_re = re.compile(r"elapsed_ms=(\d+)")

    stages = {}
    for m in stage_re.finditer(text):
        stages[m.group(1)] = {
            "wns": float(m.group(2)),
            "tns": float(m.group(3)),
            "viols": int(m.group(4)),
        }

    elapsed_s = None
    m = result_re.search(text)
    if m:
        elapsed_s = int(m.group(1)) / 1000.0

    if not stages:
        return None
    return {"stages": stages, "elapsed_s": elapsed_s}


def load_all(algo):
    designs = DESIGNS_MCTS if algo == "mcts" else DESIGNS_SA
    data = {}
    for design in designs:
        data[design] = {}
        for pct in PCTS:
            fname = f"{algo}_seed{SEED}_pct{pct}_iters{ITERS}.log"
            path = os.path.join(BASE, f"qor_{algo}", design, fname)
            data[design][pct] = parse_log(path)
    return data


# --- LaTeX generation ---------------------------------------------------------

def fmt_wns(v):
    """Format WNS in ps, 1 decimal place."""
    if v is None: return r"\multicolumn{1}{c}{--}"
    return f"{v:.1f}"

def fmt_tns(v):
    """Format TNS in ps, 0 decimal places."""
    if v is None: return r"\multicolumn{1}{c}{--}"
    return f"{v:.0f}"

def fmt_viols(v):
    if v is None: return "--"
    return str(v)

def fmt_delta(v):
    if v is None: return "--"
    s = f"{v:+.1f}"
    return s

def fmt_time(v):
    if v is None: return "--"
    m = int(v // 60)
    s = int(v % 60)
    return f"{m}m{s:02d}s"


def make_table(algo, data):
    """Build a LaTeX longtable for one algorithm."""
    ALGO_NAME = algo.upper()

    lines = []
    a = lines.append

    a(r"\begin{longtable}{@{}l r " + "r r r " * 4 + r"r r r r@{}}")
    a(r"\caption{Post-CTS Timing QoR: " + ALGO_NAME + r" (seed=2345, iters=1000)} \\")
    a(r"\label{tab:" + algo + r"_qor} \\")
    a(r"\toprule")

    # Header row 1
    a(r"\multirow{2}{*}{\textbf{Design}} & \multirow{2}{*}{\textbf{Pct}} &"
      r" \multicolumn{3}{c}{\textbf{Baseline}} &"
      r" \multicolumn{3}{c}{\textbf{After Repair}} &"
      r" \multicolumn{3}{c}{\textbf{After Restr.}} &"
      r" \multicolumn{3}{c}{\textbf{Restr.+Repair (Final)}} &"
      r" \multicolumn{3}{c}{\textbf{$\Delta$ (Final$-$Base)}} &"
      r" \textbf{Time} \\")
    a(r"\cmidrule(lr){3-5} \cmidrule(lr){6-8} \cmidrule(lr){9-11}"
      r" \cmidrule(lr){12-14} \cmidrule(lr){15-17}")

    # Header row 2
    a(r"& &"
      r" \textbf{WNS} & \textbf{TNS} & \textbf{FEP} &"
      r" \textbf{WNS} & \textbf{TNS} & \textbf{FEP} &"
      r" \textbf{WNS} & \textbf{TNS} & \textbf{FEP} &"
      r" \textbf{WNS} & \textbf{TNS} & \textbf{FEP} &"
      r" \textbf{$\Delta$WNS} & \textbf{$\Delta$TNS} & \textbf{$\Delta$FEP} &"
      r" \\")
    a(r"& & (ps) & (ps) & & (ps) & (ps) & & (ps) & (ps) & & (ps) & (ps) & & (ps) & (ps) & & \\")
    a(r"\midrule")
    a(r"\endfirsthead")
    a(r"\multicolumn{18}{c}{\tablename\ \thetable\ -- continued} \\")
    a(r"\toprule")
    a(r"\multirow{2}{*}{\textbf{Design}} & \multirow{2}{*}{\textbf{Pct}} &"
      r" \multicolumn{3}{c}{\textbf{Baseline}} &"
      r" \multicolumn{3}{c}{\textbf{After Repair}} &"
      r" \multicolumn{3}{c}{\textbf{After Restr.}} &"
      r" \multicolumn{3}{c}{\textbf{Restr.+Repair (Final)}} &"
      r" \multicolumn{3}{c}{\textbf{$\Delta$ (Final$-$Base)}} &"
      r" \textbf{Time} \\")
    a(r"\cmidrule(lr){3-5} \cmidrule(lr){6-8} \cmidrule(lr){9-11}"
      r" \cmidrule(lr){12-14} \cmidrule(lr){15-17}")
    a(r"& &"
      r" \textbf{WNS} & \textbf{TNS} & \textbf{FEP} &"
      r" \textbf{WNS} & \textbf{TNS} & \textbf{FEP} &"
      r" \textbf{WNS} & \textbf{TNS} & \textbf{FEP} &"
      r" \textbf{WNS} & \textbf{TNS} & \textbf{FEP} &"
      r" \textbf{$\Delta$WNS} & \textbf{$\Delta$TNS} & \textbf{$\Delta$FEP} &"
      r" \\")
    a(r"& & (ps) & (ps) & & (ps) & (ps) & & (ps) & (ps) & & (ps) & (ps) & & (ps) & (ps) & & \\")
    a(r"\midrule")
    a(r"\endhead")
    a(r"\midrule \multicolumn{18}{r}{\textit{Continued on next page}} \\")
    a(r"\endfoot")
    a(r"\bottomrule")
    a(r"\endlastfoot")

    designs = DESIGNS_MCTS if algo == "mcts" else DESIGNS_SA
    for design in designs:
        label = DESIGN_LABELS[design]
        first_row = True
        for pct in PCTS:
            rec = data[design][pct]
            if rec is None:
                row_cells = [label if first_row else "", str(pct)] + ["--"] * 15 + ["--"]
            else:
                s = rec["stages"]
                base = s.get("baseline", {})
                rep  = s.get("repair_only", {})
                restr = s.get("restructure_only", {})
                final = s.get("restructure_repair", {})

                def g(d, k): return d.get(k)

                bwns = g(base,"wns"); btns = g(base,"tns"); bv = g(base,"viols")
                rwns = g(rep,"wns");  rtns = g(rep,"tns");  rv = g(rep,"viols")
                xwns = g(restr,"wns"); xtns = g(restr,"tns"); xv = g(restr,"viols")
                fwns = g(final,"wns"); ftns = g(final,"tns"); fv = g(final,"viols")

                dwns = (fwns - bwns) if (fwns is not None and bwns is not None) else None
                dtns = (ftns - btns) if (ftns is not None and btns is not None) else None
                dfep = (fv - bv) if (fv is not None and bv is not None) else None

                # Bold the delta if improvement (positive WNS delta = better)
                def bold_if_pos(v, fmt_fn):
                    if v is None: return "--"
                    s = fmt_fn(v)
                    if v > 0:
                        return r"\textbf{" + s + r"}"
                    return s

                cells = [
                    label if first_row else "",
                    str(pct),
                    fmt_wns(bwns), fmt_tns(btns), fmt_viols(bv),
                    fmt_wns(rwns), fmt_tns(rtns), fmt_viols(rv),
                    fmt_wns(xwns), fmt_tns(xtns), fmt_viols(xv),
                    fmt_wns(fwns), fmt_tns(ftns), fmt_viols(fv),
                    bold_if_pos(dwns, fmt_delta),
                    bold_if_pos(dtns, fmt_delta),
                    bold_if_pos(dfep, lambda x: f"{x:+d}"),
                    fmt_time(rec["elapsed_s"]),
                ]

            a(" & ".join(str(c) for c in cells) + r" \\")
            first_row = False
        a(r"\midrule")

    a(r"\end{longtable}")
    return "\n".join(lines)


def make_summary_comparison_table(mcts_data, sa_data):
    """One table with MCTS vs SA side-by-side final results."""
    lines = []
    a = lines.append

    a(r"\begin{table*}[t]")
    a(r"\centering")
    a(r"\caption{Summary: MCTS vs.\ SA Final Timing Improvement (seed=2345, iters=1000, pct=10/15/20 best).}")
    a(r"\label{tab:comparison}")
    a(r"\small")
    a(r"\begin{tabular}{@{}l r rr rr rr@{}}")
    a(r"\toprule")
    a(r"& & \multicolumn{2}{c}{\textbf{MCTS}} & \multicolumn{2}{c}{\textbf{SA}}"
      r" & \multicolumn{2}{c}{\textbf{Speedup}} \\")
    a(r"\cmidrule(lr){3-4} \cmidrule(lr){5-6} \cmidrule(lr){7-8}")
    a(r"\textbf{Design} & \textbf{Pct}"
      r" & \textbf{$\Delta$WNS (ps)} & \textbf{Time}"
      r" & \textbf{$\Delta$WNS (ps)} & \textbf{Time}"
      r" & \textbf{MCTS/SA} \\")
    a(r"\midrule")

    for design in DESIGNS:
        label = DESIGN_LABELS[design]
        first = True
        for pct in PCTS:
            mr = mcts_data[design][pct]
            sr = sa_data[design][pct]

            def get_dwns(rec):
                if rec is None: return None
                s = rec["stages"]
                b = s.get("baseline",{}).get("wns")
                f = s.get("restructure_repair",{}).get("wns")
                if b is None or f is None: return None
                return f - b

            md = get_dwns(mr)
            sd = get_dwns(sr)
            mt = mr["elapsed_s"] if mr else None
            st = sr["elapsed_s"] if sr else None

            speedup = ""
            if mt and st and mt > 0:
                ratio = st / mt
                speedup = f"{ratio:.1f}$\\times$"

            def fmt_d(v):
                if v is None: return "--"
                s = f"{v:+.1f}"
                return r"\textbf{" + s + "}" if v > 0 else s

            row = [
                label if first else "",
                str(pct),
                fmt_d(md), fmt_time(mt),
                fmt_d(sd), fmt_time(st),
                speedup,
            ]
            a(" & ".join(str(c) for c in row) + r" \\")
            first = False
        a(r"\midrule")

    a(r"\bottomrule")
    a(r"\end{tabular}")
    a(r"\end{table*}")
    return "\n".join(lines)


def make_full_document(mcts_data, sa_data):
    """Wrap tables in a compilable LaTeX document."""
    return r"""\documentclass[10pt]{article}
\usepackage[landscape, margin=0.5in]{geometry}
\usepackage{booktabs}
\usepackage{longtable}
\usepackage{multirow}
\usepackage{xcolor}
\usepackage{caption}
\usepackage{siunitx}

\begin{document}

\section*{Post-CTS Logic Resynthesis: MCTS vs.\ SA Benchmark Results}

\textbf{Setup:} ASAP7 7nm PDK, slow corner, estimated wire RC
(resistance=1.334e-1\,$\Omega$/\textmu m, capacitance=8.6e-2\,fF/\textmu m).
Seed=2345, 1000 iterations. Positive $\Delta$WNS/TNS indicates timing improvement.

""" + make_summary_comparison_table(mcts_data, sa_data) + "\n\n\\newpage\n\n" + \
make_table("mcts", mcts_data) + "\n\n\\newpage\n\n" + \
make_table("sa", sa_data) + "\n\n\\end{document}\n"


# --- main ---------------------------------------------------------------------

if __name__ == "__main__":
    mcts = load_all("mcts")
    sa   = load_all("sa")

    out_dir = "/home/mani-deep-g/Desktop/benchmark_results"
    os.makedirs(out_dir, exist_ok=True)

    # Full document
    doc = make_full_document(mcts, sa)
    doc_path = os.path.join(out_dir, "benchmark_tables.tex")
    with open(doc_path, "w") as f:
        f.write(doc)
    print(f"Written: {doc_path}")

    # Individual snippet files (easier to include in paper)
    for algo, data in [("mcts", mcts), ("sa", sa)]:
        snippet = make_table(algo, data)
        p = os.path.join(out_dir, f"{algo}_table.tex")
        with open(p, "w") as f:
            f.write(snippet)
        print(f"Written: {p}")

    comp = make_summary_comparison_table(mcts, sa)
    p = os.path.join(out_dir, "comparison_table.tex")
    with open(p, "w") as f:
        f.write(comp)
    print(f"Written: {p}")

    print("\nFiles saved to:", out_dir)
    print("To compile: pdflatex benchmark_tables.tex")
