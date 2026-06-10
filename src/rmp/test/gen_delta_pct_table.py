#!/usr/bin/env python3
"""Generate a combined MCTS+SA delta-percentage table (WNS & TNS, initial vs final)."""

import re, os

BASE = "/home/mani-deep-g/OpenROAD/src/rmp/test"
OUT  = "/home/mani-deep-g/OpenROAD/src/rmp/test/results"

DESIGNS_MCTS = ["gcd", "spi", "uart", "usb_phy", "sasc_top", "b13", "i2c_master_top"]
DESIGNS_SA   = ["gcd", "spi", "uart", "usb_phy", "sasc_top", "b13", "i2c_master_top"]
PCTS  = [10, 15, 20]
SEED  = 2345
ITERS = 1000

LABELS = {
    "gcd": "GCD", "spi": "SPI", "uart": "UART", "usb_phy": "USB-PHY",
    "sasc_top": "SASC", "b13": "B13", "i2c_master_top": "I2C", "b12": "B12",
}

# ── parse ──────────────────────────────────────────────────────────────────────
def parse(path):
    if not os.path.exists(path):
        return None
    stage_re = re.compile(
        r"QOR_STAGE stage=(\w+)\s+wns_ps=([-\d.]+)\s+tns_ps=([-\d.]+)")
    stages = {}
    with open(path) as f:
        for line in f:
            m = stage_re.search(line)
            if m:
                stages[m.group(1)] = {"wns": float(m.group(2)),
                                       "tns": float(m.group(3))}
    if "baseline" not in stages or "restructure_repair" not in stages:
        return None
    return stages

def pct_change(before, after):
    if before == 0:
        return None
    return 100.0 * (after - before) / abs(before)

def fmt(v, improved_positive=True):
    """Format a percentage value; mark improvements in bold."""
    if v is None:
        return "--", False
    improved = (v > 0) if improved_positive else (v < 0)
    return f"{v:+.2f}\\%", improved

# ── build data ─────────────────────────────────────────────────────────────────
rows = []
all_designs = DESIGNS_MCTS   # use MCTS list (superset); SA missing b12 handled below

for design in all_designs:
    for pct in PCTS:
        row = {"design": design, "pct": pct}

        for algo in ("mcts", "sa"):
            if algo == "sa" and design == "b12":
                row[algo] = None
                continue
            fname = f"{algo}_seed{SEED}_pct{pct}_iters{ITERS}.log"
            path  = os.path.join(BASE, f"qor_{algo}", design, fname)
            stages = parse(path)
            if stages is None:
                row[algo] = None
                continue
            b = stages["baseline"]
            f = stages["restructure_repair"]
            elapsed_ms = None
            with open(path) as fh:
                for line in fh:
                    m = re.search(r"elapsed_ms=(\d+)", line)
                    if m:
                        elapsed_ms = int(m.group(1))
            row[algo] = {
                "wns_before": b["wns"], "wns_after": f["wns"],
                "tns_before": b["tns"], "tns_after": f["tns"],
                "dwns_pct": pct_change(b["wns"], f["wns"]),
                "dtns_pct": pct_change(b["tns"], f["tns"]),
                "elapsed_ms": elapsed_ms,
            }
        rows.append(row)

# ── LaTeX table ────────────────────────────────────────────────────────────────
def make_latex():
    lines = []
    a = lines.append

    a(r"\documentclass[10pt]{article}")
    a(r"\usepackage[landscape,margin=0.6in]{geometry}")
    a(r"\usepackage{booktabs,multirow,xcolor,array,caption}")
    a(r"\definecolor{posclr}{RGB}{0,150,0}")
    a(r"\definecolor{negclr}{RGB}{210,0,0}")
    a(r"\newcommand{\pos}[1]{{\color{posclr}\textbf{#1}}}")
    a(r"\newcommand{\bad}[1]{{\color{negclr}\textbf{#1}}}")
    a(r"\begin{document}")
    a(r"\begin{table}[h]")
    a(r"\centering")
    a(r"\caption{WNS and TNS Percentage Improvement: Initial vs.\ Final"
      r" (Baseline $\to$ Restr.+Repair), MCTS and SA Combined."
      r" Positive = improvement. {\color{posclr}\textbf{Bold green}} = improved, "
      r"{\color{negclr}\textbf{Bold red}} = regressed. Seed=2345, 1000 iters, ASAP7 slow corner.}")
    a(r"\label{tab:delta_pct}")
    a(r"\small")
    a(r"\begin{tabular}{@{} l r | rr | rr | r r r @{}}")
    a(r"\toprule")
    a(r"\multirow{2}{*}{\textbf{Design}} & \multirow{2}{*}{\textbf{Pct}}"
      r" & \multicolumn{2}{c|}{\textbf{MCTS}}"
      r" & \multicolumn{2}{c|}{\textbf{SA}}"
      r" & \multicolumn{3}{c}{\textbf{Runtime}} \\")
    a(r"\cmidrule(lr){3-4}\cmidrule(lr){5-6}\cmidrule(lr){7-9}")
    a(r" & & \textbf{$\Delta$WNS\%} & \textbf{$\Delta$TNS\%}"
      r"   & \textbf{$\Delta$WNS\%} & \textbf{$\Delta$TNS\%}"
      r"   & \textbf{MCTS} & \textbf{SA} & \textbf{Speedup} \\")
    a(r"\midrule")

    prev_design = None
    for row in rows:
        design = row["design"]
        pct    = row["pct"]
        label  = LABELS[design] if design != prev_design else ""

        def cell(algo, metric):
            d = row[algo]
            if d is None:
                return r"\multicolumn{1}{c}{--}"
            v = d[metric]
            # WNS: positive delta% means WNS moved toward 0 (less negative) = good
            # TNS: same sign convention
            improved = (v is not None and v > 0)
            s, _ = fmt(v)
            if improved:
                return r"\pos{" + s + "}"
            elif v is not None and v < 0:
                return r"\bad{" + s + "}"
            else:
                return s

        def fmt_time(algo):
            d = row[algo]
            if d is None or d.get("elapsed_ms") is None:
                return r"\multicolumn{1}{c}{--}"
            s = d["elapsed_ms"] / 1000
            m = int(s // 60); sec = int(s % 60)
            return f"{m}m{sec:02d}s"

        mt = row["mcts"]["elapsed_ms"] if row["mcts"] else None
        st = row["sa"]["elapsed_ms"]   if row["sa"]   else None
        if mt and st and mt > 0:
            spd = f"{st/mt:.1f}$\\times$"
        else:
            spd = r"\multicolumn{1}{c}{--}"

        cells = [
            label,
            str(pct) + r"\%",
            cell("mcts", "dwns_pct"),
            cell("mcts", "dtns_pct"),
            cell("sa",   "dwns_pct"),
            cell("sa",   "dtns_pct"),
            fmt_time("mcts"),
            fmt_time("sa"),
            spd,
        ]
        a(" & ".join(cells) + r" \\")

        if pct == 20:
            a(r"\midrule")

        prev_design = design

    a(r"\bottomrule")
    a(r"\end{tabular}")
    a(r"\end{table}")
    a(r"\end{document}")
    return "\n".join(lines)


# ── plain-text table ───────────────────────────────────────────────────────────
def make_plain():
    col = ["Design", "Pct", "MCTS ΔWNS%", "MCTS ΔTNS%", "SA ΔWNS%", "SA ΔTNS%",
           "MCTS Time", "SA Time", "Speedup"]
    data_rows = []
    for row in rows:
        def v(algo, metric):
            d = row[algo]
            if d is None: return "N/A"
            val = d[metric]
            return f"{val:+.2f}%" if val is not None else "--"
        def t(algo):
            d = row[algo]
            if d is None or d.get("elapsed_ms") is None: return "N/A"
            s = d["elapsed_ms"] / 1000
            return f"{int(s//60)}m{int(s%60):02d}s"
        mt = row["mcts"]["elapsed_ms"] if row["mcts"] else None
        st = row["sa"]["elapsed_ms"]   if row["sa"]   else None
        spd = f"{st/mt:.1f}x" if mt and st and mt > 0 else "N/A"
        data_rows.append([
            LABELS[row["design"]], f"{row['pct']}%",
            v("mcts","dwns_pct"), v("mcts","dtns_pct"),
            v("sa",  "dwns_pct"), v("sa",  "dtns_pct"),
            t("mcts"), t("sa"), spd,
        ])
    widths = [max(len(col[i]), max(len(r[i]) for r in data_rows)) for i in range(len(col))]
    sep = "+-" + "-+-".join("-"*w for w in widths) + "-+"
    def rstr(r): return "| " + " | ".join(str(v).ljust(widths[i]) for i,v in enumerate(r)) + " |"
    lines = [sep, rstr(col), sep]
    prev = None
    for r in data_rows:
        if prev and r[0] != prev: lines.append(sep)
        lines.append(rstr(r))
        prev = r[0]
    lines.append(sep)
    return "\n".join(lines)


# ── write & compile ────────────────────────────────────────────────────────────
if __name__ == "__main__":
    tex = make_latex()
    tex_path = os.path.join(OUT, "delta_pct_table.tex")
    with open(tex_path, "w") as f:
        f.write(tex)
    print(f"Written: {tex_path}")

    plain = make_plain()
    txt_path = os.path.join(OUT, "delta_pct_table.txt")
    with open(txt_path, "w") as f:
        f.write(plain)
    print(f"Written: {txt_path}")
    print()
    print(plain)
