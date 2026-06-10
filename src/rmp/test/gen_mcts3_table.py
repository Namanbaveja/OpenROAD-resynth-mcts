#!/usr/bin/env python3
"""
Parse 3-iter MCTS and SA logs and generate a combined LaTeX table.
Table shows WNS, TNS, FEP after each iteration for both algorithms.
Run after all benchmarks complete.
"""
import re, os

BASE    = "/home/mani-deep-g/OpenROAD/src/rmp/test"
OUT_DIR = os.path.join(BASE, "results")
os.makedirs(OUT_DIR, exist_ok=True)

DESIGNS = ["gcd", "spi", "uart", "usb_phy", "sasc_top", "b13", "i2c_master_top"]
PCTS    = [10, 15, 20]
SEED    = 2345
ITERS   = 1000

LABELS = {
    "gcd": "GCD", "spi": "SPI", "uart": "UART", "usb_phy": "USB-PHY",
    "sasc_top": "SASC", "b13": "B13", "i2c_master_top": "I2C",
}

# ── parse ─────────────────────────────────────────────────────────────────────
def parse_log(path):
    """Return dict: baseline, iter1, iter2, iter3 → {wns, tns, viols},
    plus ms1, ms2, ms3, total_ms."""
    if not os.path.exists(path):
        return None
    stage_re = re.compile(
        r"QOR_STAGE stage=(\w+)\s+wns_ps=([-\d.]+)\s+tns_ps=([-\d.]+)\s+viols=(\d+)")
    result_re = re.compile(r"^RESULT (.+)$", re.MULTILINE)
    kv_re = re.compile(r"(\w+)=([-\d.]+)")

    stages, result = {}, {}
    with open(path) as f:
        text = f.read()

    for m in stage_re.finditer(text):
        name = m.group(1)
        entry = {"wns": float(m.group(2)), "tns": float(m.group(3)), "viols": int(m.group(4))}
        # Keep first occurrence of 'baseline' (our proc's snapshot before any resynth call).
        # iter1/iter2/iter3 only appear from our proc so last-write is fine.
        if name == "baseline" and "baseline" not in stages:
            stages[name] = entry
        elif name != "baseline":
            stages[name] = entry

    rm = result_re.search(text)
    if rm:
        for k, v in kv_re.findall(rm.group(1)):
            try: result[k] = float(v)
            except: pass

    if not stages.get("baseline") or not stages.get("iter3"):
        return None

    return {
        "baseline": stages["baseline"],
        "iter1":    stages.get("iter1", {}),
        "iter2":    stages.get("iter2", {}),
        "iter3":    stages.get("iter3", {}),
        "ms1":      result.get("ms1", 0),
        "ms2":      result.get("ms2", 0),
        "ms3":      result.get("ms3", 0),
        "total_ms": result.get("total_ms", 0),
    }

def load_all(tag):
    """tag: mcts3 or sa3"""
    data = {}
    for design in DESIGNS:
        data[design] = {}
        for pct in PCTS:
            fname = f"{tag}_seed{SEED}_pct{pct}_iters{ITERS}.log"
            path  = os.path.join(BASE, f"qor_{tag}", design, fname)
            data[design][pct] = parse_log(path)
    return data

# ── formatting ────────────────────────────────────────────────────────────────
def fw(v):   return f"{v:.1f}" if v is not None else "--"
def ft(v):   return f"{v:.0f}" if v is not None else "--"
def fv(v):   return str(v)    if v is not None else "--"
def fdp(v):  # delta % of WNS improvement
    if v is None: return "--"
    s = f"{v:+.2f}\\%"
    return r"\textbf{" + s + "}" if v > 0 else s

def fmt_ms(ms):
    if not ms: return "--"
    m = int(ms/1000//60); s = int(ms/1000%60)
    return f"{m}m{s:02d}s"

def delta_pct(base, cur):
    if base is None or cur is None or base == 0: return None
    return 100.0 * (cur - base) / abs(base)

# ── LaTeX table ───────────────────────────────────────────────────────────────
def make_latex(mcts, sa):
    lines = []
    a = lines.append

    a(r"\documentclass[10pt]{article}")
    a(r"\usepackage[landscape,top=0.5in,bottom=0.5in,left=0.4in,right=0.4in]{geometry}")
    a(r"\usepackage{booktabs,multirow,longtable,xcolor,array,caption}")
    a(r"\definecolor{posclr}{RGB}{0,140,0}")
    a(r"\definecolor{negclr}{RGB}{210,0,0}")
    a(r"\newcommand{\pos}[1]{{\color{posclr}\textbf{#1}}}")
    a(r"\newcommand{\bad}[1]{{\color{negclr}\textbf{#1}}}")
    a(r"\setlength{\tabcolsep}{4pt}")
    a(r"\begin{document}")
    a(r"\footnotesize")

    # ── Table 1: WNS/TNS/FEP after each of 3 iterations ─────────────────────
    a(r"\begin{longtable}{@{} l r | rrr | rrr | rrr | rrr | rrr | rrr @{}}")
    a(r"\caption{Effect of 3 Sequential Calls to \texttt{resynth\_mcts} and "
      r"\texttt{resynth\_annealing} (MCTS max\_depth=10, 1000 iters/call, "
      r"seed=2345/2346/2347, ASAP7 slow corner). "
      r"Each call operates on the netlist produced by the previous call. "
      r"WNS and TNS in ps; positive $\Delta$WNS\% = improvement.} \\")
    a(r"\label{tab:iter3}")
    a(r"\toprule")

    # header row 1
    a(r"\multirow{2}{*}{\textbf{Design}} & \multirow{2}{*}{\textbf{Pct}}"
      r" & \multicolumn{3}{c|}{\textbf{Baseline}}"
      r" & \multicolumn{3}{c|}{\textbf{MCTS Iter 1}}"
      r" & \multicolumn{3}{c|}{\textbf{MCTS Iter 2}}"
      r" & \multicolumn{3}{c|}{\textbf{MCTS Iter 3}}"
      r" & \multicolumn{3}{c|}{\textbf{SA Iter 1}}"
      r" & \multicolumn{3}{c|}{\textbf{SA Iter 2}}"
      r" & \multicolumn{3}{c}{\textbf{SA Iter 3}} \\")
    a(r"\cmidrule(lr){3-5}\cmidrule(lr){6-8}\cmidrule(lr){9-11}"
      r"\cmidrule(lr){12-14}\cmidrule(lr){15-17}\cmidrule(lr){18-20}\cmidrule(lr){21-23}")
    # header row 2
    hdr_unit = r"\textbf{WNS} & \textbf{TNS} & \textbf{FEP}"
    hdr_sep  = " & ".join([hdr_unit] * 7)
    a("& & " + hdr_sep + r" \\")
    a(r"\midrule")
    a(r"\endfirsthead")
    a(r"\multicolumn{23}{c}{\tablename\ \thetable\ -- continued} \\ \toprule")
    a(r"\multirow{2}{*}{\textbf{Design}} & \multirow{2}{*}{\textbf{Pct}}"
      r" & \multicolumn{3}{c|}{\textbf{Baseline}}"
      r" & \multicolumn{3}{c|}{\textbf{MCTS Iter 1}}"
      r" & \multicolumn{3}{c|}{\textbf{MCTS Iter 2}}"
      r" & \multicolumn{3}{c|}{\textbf{MCTS Iter 3}}"
      r" & \multicolumn{3}{c|}{\textbf{SA Iter 1}}"
      r" & \multicolumn{3}{c|}{\textbf{SA Iter 2}}"
      r" & \multicolumn{3}{c}{\textbf{SA Iter 3}} \\")
    a(r"\cmidrule(lr){3-5}\cmidrule(lr){6-8}\cmidrule(lr){9-11}"
      r"\cmidrule(lr){12-14}\cmidrule(lr){15-17}\cmidrule(lr){18-20}\cmidrule(lr){21-23}")
    a("& & " + hdr_sep + r" \\ \midrule")
    a(r"\endhead")
    a(r"\midrule\multicolumn{23}{r}{\textit{Continued\ldots}}\\\endfoot")
    a(r"\bottomrule\endlastfoot")

    prev = None
    for design in DESIGNS:
        for pct in PCTS:
            md = mcts.get(design, {}).get(pct)
            sd = sa.get(design,   {}).get(pct)

            label = LABELS[design] if design != prev else ""
            prev  = design

            def col(rec, stage):
                if rec is None or not rec.get(stage):
                    return r"\multicolumn{1}{c}{--} & \multicolumn{1}{c}{--} & \multicolumn{1}{c}{--}"
                s = rec[stage]
                bwns = rec["baseline"]["wns"] if rec else None
                wns_v = s.get("wns"); tns_v = s.get("tns"); viol_v = s.get("viols")
                dp = delta_pct(bwns, wns_v)
                # color WNS cell
                wns_str = fw(wns_v)
                if dp is not None and dp > 0:
                    wns_str = r"\pos{" + wns_str + "}"
                elif dp is not None and dp < 0:
                    wns_str = r"\bad{" + wns_str + "}"
                return f"{wns_str} & {ft(tns_v)} & {fv(viol_v)}"

            base_col = col(md, "baseline")  # same baseline for both
            cells = [
                label, f"{pct}\\%",
                base_col,
                col(md, "iter1"), col(md, "iter2"), col(md, "iter3"),
                col(sd, "iter1"), col(sd, "iter2"), col(sd, "iter3"),
            ]
            a(" & ".join(cells) + r" \\")
        a(r"\midrule")

    a(r"\end{longtable}")

    # ── Table 2: ΔWNS% per iteration + cumulative runtime ────────────────────
    a(r"\newpage")
    a(r"\begin{longtable}{@{} l r | rr | rr | rr | r || rr | rr | rr | r @{}}")
    a(r"\caption{WNS Improvement (\%) and Cumulative Runtime per Iteration. "
      r"\pos{Green} = improvement over baseline; "
      r"\bad{Red} = regression. "
      r"Time columns show wall-clock for each individual call.} \\")
    a(r"\label{tab:iter3_pct}")
    a(r"\toprule")
    a(r"\multirow{2}{*}{\textbf{Design}} & \multirow{2}{*}{\textbf{Pct}}"
      r" & \multicolumn{7}{c||}{\textbf{MCTS (max\_depth=10)}}"
      r" & \multicolumn{7}{c}{\textbf{SA}} \\")
    a(r"\cmidrule(lr){3-9}\cmidrule(lr){10-16}")
    a(r" & "
      r" & \textbf{It.1 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{It.2 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{It.3 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{Total}"
      r" & \textbf{It.1 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{It.2 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{It.3 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{Total} \\")
    a(r"\midrule\endfirsthead")
    a(r"\multicolumn{16}{c}{\tablename\ \thetable\ -- continued}\\\toprule")
    a(r" & "
      r" & \textbf{It.1 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{It.2 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{It.3 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{Total}"
      r" & \textbf{It.1 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{It.2 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{It.3 $\Delta$WNS\%} & \textbf{Time}"
      r" & \textbf{Total} \\\midrule")
    a(r"\endhead\midrule\multicolumn{16}{r}{\textit{Continued\ldots}}\\\endfoot\bottomrule\endlastfoot")

    prev = None
    for design in DESIGNS:
        for pct in PCTS:
            md = mcts.get(design, {}).get(pct)
            sd = sa.get(design,   {}).get(pct)
            label = LABELS[design] if design != prev else ""
            prev  = design

            def dpct_cell(rec, stage):
                if rec is None or not rec.get(stage) or not rec.get("baseline"):
                    return r"\multicolumn{1}{c}{--}"
                v = delta_pct(rec["baseline"]["wns"], rec[stage]["wns"])
                if v is None: return "--"
                s = f"{v:+.2f}\\%"
                if v > 0: return r"\pos{" + s + "}"
                if v < 0: return r"\bad{" + s + "}"
                return s

            def time_cell(rec, key):
                if rec is None: return r"\multicolumn{1}{c}{--}"
                return fmt_ms(rec.get(key, 0))

            cells = [
                label, f"{pct}\\%",
                dpct_cell(md,"iter1"), time_cell(md,"ms1"),
                dpct_cell(md,"iter2"), time_cell(md,"ms2"),
                dpct_cell(md,"iter3"), time_cell(md,"ms3"),
                time_cell(md,"total_ms"),
                dpct_cell(sd,"iter1"), time_cell(sd,"ms1"),
                dpct_cell(sd,"iter2"), time_cell(sd,"ms2"),
                dpct_cell(sd,"iter3"), time_cell(sd,"ms3"),
                time_cell(sd,"total_ms"),
            ]
            a(" & ".join(cells) + r" \\")
        a(r"\midrule")

    a(r"\end{longtable}")
    a(r"\end{document}")
    return "\n".join(lines)

# ── plain text table ──────────────────────────────────────────────────────────
def make_plain(mcts, sa):
    rows = [["Design","Pct",
             "Base WNS","Base TNS","Base FEP",
             "M-It1 WNS","M-It1 TNS","M-It1 FEP","M-It1 ΔWNS%",
             "M-It2 WNS","M-It2 TNS","M-It2 FEP","M-It2 ΔWNS%",
             "M-It3 WNS","M-It3 TNS","M-It3 FEP","M-It3 ΔWNS%",
             "SA-It1 WNS","SA-It1 TNS","SA-It1 FEP","SA-It1 ΔWNS%",
             "SA-It2 WNS","SA-It2 TNS","SA-It2 FEP","SA-It2 ΔWNS%",
             "SA-It3 WNS","SA-It3 TNS","SA-It3 FEP","SA-It3 ΔWNS%"]]

    for design in DESIGNS:
        for pct in PCTS:
            md = mcts.get(design, {}).get(pct)
            sd = sa.get(design,   {}).get(pct)

            def g(rec, stage, key, fmt_fn):
                if rec is None or not rec.get(stage): return "--"
                v = rec[stage].get(key)
                return fmt_fn(v) if v is not None else "--"

            def dp(rec, stage):
                if rec is None or not rec.get(stage) or not rec.get("baseline"): return "--"
                v = delta_pct(rec["baseline"]["wns"], rec[stage]["wns"])
                return f"{v:+.2f}%" if v is not None else "--"

            bwns = g(md or sd, "baseline", "wns", fw)
            btns = g(md or sd, "baseline", "tns", ft)
            bv   = g(md or sd, "baseline", "viols", fv)

            row = [LABELS[design], f"{pct}%", bwns, btns, bv]
            for stage in ["iter1","iter2","iter3"]:
                row += [g(md,stage,"wns",fw), g(md,stage,"tns",ft),
                        g(md,stage,"viols",fv), dp(md,stage)]
            for stage in ["iter1","iter2","iter3"]:
                row += [g(sd,stage,"wns",fw), g(sd,stage,"tns",ft),
                        g(sd,stage,"viols",fv), dp(sd,stage)]
            rows.append(row)

    col_w = [max(len(str(r[i])) for r in rows) for i in range(len(rows[0]))]
    sep   = "+-" + "-+-".join("-"*w for w in col_w) + "-+"
    def rstr(r): return "| " + " | ".join(str(v).ljust(col_w[i]) for i,v in enumerate(r)) + " |"
    out = [sep, rstr(rows[0]), sep]
    prev = None
    for r in rows[1:]:
        if prev and r[0] != prev: out.append(sep)
        out.append(rstr(r))
        prev = r[0]
    out.append(sep)
    return "\n".join(out)

# ── main ──────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    mcts = load_all("mcts3")
    sa   = load_all("sa3")

    mc = sum(1 for d in DESIGNS for p in PCTS if mcts.get(d,{}).get(p))
    sc = sum(1 for d in DESIGNS for p in PCTS if sa.get(d,{}).get(p))
    total = len(DESIGNS) * len(PCTS)
    print(f"MCTS3 results: {mc}/{total}   SA3 results: {sc}/{total}")

    if mc == 0 and sc == 0:
        print("No results yet — run the benchmarks first."); exit(0)

    # plain text
    plain = make_plain(mcts, sa)
    txt_path = os.path.join(OUT_DIR, "iter3_results_table.txt")
    with open(txt_path, "w") as f: f.write(plain)
    print(f"Written: {txt_path}")
    print(plain)

    # LaTeX + PDF
    tex = make_latex(mcts, sa)
    tex_path = os.path.join(OUT_DIR, "iter3_results_table.tex")
    with open(tex_path, "w") as f: f.write(tex)
    print(f"Written: {tex_path}")

    import subprocess
    r = subprocess.run(
        ["pdflatex", "-interaction=nonstopmode", "iter3_results_table.tex"],
        cwd=OUT_DIR, capture_output=True, text=True
    )
    if "Output written" in r.stdout + r.stderr:
        print(f"PDF compiled: {OUT_DIR}/iter3_results_table.pdf")
        subprocess.Popen(["xdg-open", os.path.join(OUT_DIR, "iter3_results_table.pdf")])
    else:
        print("pdflatex output:", r.stdout[-500:])
