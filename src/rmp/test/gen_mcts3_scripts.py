#!/usr/bin/env python3
"""
Generate all scripts for 3-iteration experiments:
  - MCTS 3-iter: resynth_mcts × 3 (each on netlist from previous)
  - SA   3-iter: resynth_annealing × 3 (each on netlist from previous)
Separate runs, compared in one combined table.
"""
import os, stat

BASE = "/home/mani-deep-g/OpenROAD/src/rmp/test"

DESIGNS = {
    "gcd":            "../../benchmark_cts/new/gcd",
    "spi":            "../../benchmark_cts/Opencore/spi",
    "uart":           "../../benchmark_cts/new/uart",
    "usb_phy":        "../../benchmark_cts/more_design/usb_phy",
    "sasc_top":       "../../benchmark_cts/Opencore/sasc_top",
    "b13":            "../../benchmark_cts/more_design/b13",
    "i2c_master_top": "../../benchmark_cts/Opencore/i2c_master_top",
}

SLOW_LIBS = [
    "../../asap7/asap7sc7p5t_AO_RVT_SS_nldm_211120.lib.gz",
    "../../asap7/asap7sc7p5t_INVBUF_RVT_SS_nldm_220122.lib.gz",
    "../../asap7/asap7sc7p5t_OA_RVT_SS_nldm_211120.lib.gz",
    "../../asap7/asap7sc7p5t_SEQ_RVT_SS_nldm_220123.lib",
    "../../asap7/asap7sc7p5t_SIMPLE_RVT_SS_nldm_211120.lib.gz",
]
FAST_LIBS = [
    "../../asap7/asap7sc7p5t_AO_RVT_FF_nldm_211120.lib.gz",
    "../../asap7/asap7sc7p5t_INVBUF_RVT_FF_nldm_220122.lib.gz",
    "../../asap7/asap7sc7p5t_OA_RVT_FF_nldm_211120.lib.gz",
    "../../asap7/asap7sc7p5t_SEQ_RVT_FF_nldm_220123.lib",
    "../../asap7/asap7sc7p5t_SIMPLE_RVT_FF_nldm_211120.lib.gz",
]

# ── shared TCL preamble (liberty + lef + def + wire_rc + helpers) ────────────
def preamble(design, def_path):
    slow = "\n".join(f"  {f}" for f in SLOW_LIBS)
    fast = "\n".join(f"  {f}" for f in FAST_LIBS)
    return f"""
define_corners fast slow
foreach f {{
{slow}
}} {{ read_liberty -corner slow $f }}
foreach f {{
{fast}
}} {{ read_liberty -corner fast $f }}

read_lef ../../asap7/asap7_tech_1x_201209.lef
read_lef ../../asap7/asap7sc7p5t_28_R_1x_220121a.lef
read_def {def_path}/4_cts.def
read_sdc {def_path}/4_cts.sdc

set_wire_rc -signal -resistance 1.3340e-1 -capacitance 8.6000e-2

proc get_wns_ps {{}} {{
    set w 0.0; catch {{ set w [sta::worst_slack_cmd "max"] }}
    if {{$w > 0.0}} {{ set w 0.0 }}
    return [expr {{$w * 1.0e12}}]
}}
proc get_tns_ps {{}} {{
    set t 0.0; catch {{ set t [sta::total_negative_slack_cmd "max"] }}
    return [expr {{$t * 1.0e12}}]
}}
proc get_viols {{}} {{
    set v 0; catch {{ set v [sta::endpoint_violation_count "max"] }}
    return $v
}}
proc log_stage {{stage}} {{
    set w [get_wns_ps]; set t [get_tns_ps]; set v [get_viols]
    puts [format "QOR_STAGE stage=%s wns_ps=%.4f tns_ps=%.4f viols=%d" $stage $w $t $v]
    return [list $w $t $v]
}}
"""

# ── MCTS 3-iter TCL ──────────────────────────────────────────────────────────
def make_mcts_tcl(design, def_path):
    pre = preamble(design, def_path)
    return f"""# MCTS 3-iteration chained resynthesis — {design} (ASAP7 post-CTS)
# Calls resynth_mcts 3 times; each call starts from the netlist the previous left.
# max_depth=10, 1000 iters per call, seeds 2345/2346/2347.

set seed         [expr {{[info exists ::env(SEED)]         ? $::env(SEED)         : 2345}}]
set iters        [expr {{[info exists ::env(ITERS)]        ? $::env(ITERS)        : 1000}}]
set corner       [expr {{[info exists ::env(CORNER)]       ? $::env(CORNER)       : "slow"}}]
set pct          [expr {{[info exists ::env(PERCENTAGE)]   ? $::env(PERCENTAGE)   : 20}}]
set max_depth    [expr {{[info exists ::env(MAX_DEPTH)]    ? $::env(MAX_DEPTH)    : 10}}]
set ucb_constant [expr {{[info exists ::env(UCB_CONSTANT)] ? $::env(UCB_CONSTANT) : 0.3}}]
set debug_lvl    [expr {{[info exists ::env(DEBUG_LEVEL)]  ? $::env(DEBUG_LEVEL)  : 1}}]
{pre}
remove_fillers
set_debug_level RMP mcts    $debug_lvl
set_debug_level RMP sa_eval $debug_lvl

lassign [log_stage baseline] wns0 tns0 viols0
set t_start [clock milliseconds]

# ---- MCTS iteration 1 -------------------------------------------------------
set s1 $seed
puts "\\n>>> MCTS ITER 1: seed=$s1 pct=$pct iters=$iters max_depth=$max_depth"
set t1 [clock milliseconds]
resynth_mcts -percentage $pct -corner $corner -seed $s1 \\
             -iters $iters -max_depth $max_depth -ucb_constant $ucb_constant
set ms1 [expr {{[clock milliseconds] - $t1}}]
lassign [log_stage iter1] wns1 tns1 viols1

# ---- MCTS iteration 2 -------------------------------------------------------
set s2 [expr {{$seed + 1}}]
puts "\\n>>> MCTS ITER 2: seed=$s2 pct=$pct iters=$iters max_depth=$max_depth"
set t2 [clock milliseconds]
resynth_mcts -percentage $pct -corner $corner -seed $s2 \\
             -iters $iters -max_depth $max_depth -ucb_constant $ucb_constant
set ms2 [expr {{[clock milliseconds] - $t2}}]
lassign [log_stage iter2] wns2 tns2 viols2

# ---- MCTS iteration 3 -------------------------------------------------------
set s3 [expr {{$seed + 2}}]
puts "\\n>>> MCTS ITER 3: seed=$s3 pct=$pct iters=$iters max_depth=$max_depth"
set t3 [clock milliseconds]
resynth_mcts -percentage $pct -corner $corner -seed $s3 \\
             -iters $iters -max_depth $max_depth -ucb_constant $ucb_constant
set ms3 [expr {{[clock milliseconds] - $t3}}]
lassign [log_stage iter3] wns3 tns3 viols3

set total_ms [expr {{[clock milliseconds] - $t_start}}]

puts [format \\
  "RESULT algo=mcts3 seed=%s pct=%s iters=%s max_depth=%s \\
wns0=%.4f tns0=%.4f viols0=%d \\
wns1=%.4f tns1=%.4f viols1=%d \\
wns2=%.4f tns2=%.4f viols2=%d \\
wns3=%.4f tns3=%.4f viols3=%d \\
ms1=%d ms2=%d ms3=%d total_ms=%d" \\
  $seed $pct $iters $max_depth \\
  $wns0 $tns0 $viols0 \\
  $wns1 $tns1 $viols1 \\
  $wns2 $tns2 $viols2 \\
  $wns3 $tns3 $viols3 \\
  $ms1 $ms2 $ms3 $total_ms]
"""

# ── SA 3-iter TCL ────────────────────────────────────────────────────────────
def make_sa_tcl(design, def_path):
    pre = preamble(design, def_path)
    return f"""# SA 3-iteration chained resynthesis — {design} (ASAP7 post-CTS)
# Calls resynth_annealing 3 times; each call starts from the netlist the previous left.
# 1000 iters per call, seeds 2345/2346/2347.

set seed      [expr {{[info exists ::env(SEED)]       ? $::env(SEED)       : 2345}}]
set iters     [expr {{[info exists ::env(ITERS)]      ? $::env(ITERS)      : 1000}}]
set corner    [expr {{[info exists ::env(CORNER)]     ? $::env(CORNER)     : "slow"}}]
set pct       [expr {{[info exists ::env(PERCENTAGE)] ? $::env(PERCENTAGE) : 20}}]
set debug_lvl [expr {{[info exists ::env(DEBUG_LEVEL)]? $::env(DEBUG_LEVEL): 1}}]
{pre}
remove_fillers
set_debug_level RMP sa_eval $debug_lvl

lassign [log_stage baseline] wns0 tns0 viols0
set t_start [clock milliseconds]

# ---- SA iteration 1 ---------------------------------------------------------
set s1 $seed
puts "\\n>>> SA ITER 1: seed=$s1 pct=$pct iters=$iters"
set t1 [clock milliseconds]
resynth_annealing -percentage $pct -corner $corner -seed $s1 -iters $iters
set ms1 [expr {{[clock milliseconds] - $t1}}]
lassign [log_stage iter1] wns1 tns1 viols1

# ---- SA iteration 2 ---------------------------------------------------------
set s2 [expr {{$seed + 1}}]
puts "\\n>>> SA ITER 2: seed=$s2 pct=$pct iters=$iters"
set t2 [clock milliseconds]
resynth_annealing -percentage $pct -corner $corner -seed $s2 -iters $iters
set ms2 [expr {{[clock milliseconds] - $t2}}]
lassign [log_stage iter2] wns2 tns2 viols2

# ---- SA iteration 3 ---------------------------------------------------------
set s3 [expr {{$seed + 2}}]
puts "\\n>>> SA ITER 3: seed=$s3 pct=$pct iters=$iters"
set t3 [clock milliseconds]
resynth_annealing -percentage $pct -corner $corner -seed $s3 -iters $iters
set ms3 [expr {{[clock milliseconds] - $t3}}]
lassign [log_stage iter3] wns3 tns3 viols3

set total_ms [expr {{[clock milliseconds] - $t_start}}]

puts [format \\
  "RESULT algo=sa3 seed=%s pct=%s iters=%s \\
wns0=%.4f tns0=%.4f viols0=%d \\
wns1=%.4f tns1=%.4f viols1=%d \\
wns2=%.4f tns2=%.4f viols2=%d \\
wns3=%.4f tns3=%.4f viols3=%d \\
ms1=%d ms2=%d ms3=%d total_ms=%d" \\
  $seed $pct $iters \\
  $wns0 $tns0 $viols0 \\
  $wns1 $tns1 $viols1 \\
  $wns2 $tns2 $viols2 \\
  $wns3 $tns3 $viols3 \\
  $ms1 $ms2 $ms3 $total_ms]
"""

# ── Shell sweep ───────────────────────────────────────────────────────────────
def make_sh(design, algo):
    tag   = "mcts3" if algo == "mcts" else "sa3"
    tcl   = f"{design}_routed_qor_{tag}.tcl"
    extra = 'MAX_DEPTH="${MAX_DEPTH:-10}" UCB_CONSTANT="${UCB_CONSTANT:-0.3}"' \
            if algo == "mcts" else ""
    extra_export = 'MAX_DEPTH="$MAX_DEPTH" UCB_CONSTANT="$UCB_CONSTANT"' \
                   if algo == "mcts" else ""
    return f"""#!/usr/bin/env bash
# 3-iteration {'MCTS' if algo=='mcts' else 'SA'} sweep on {design} (ASAP7 post-CTS).
set -u
OPENROAD="${{OPENROAD:-openroad}}"
SEEDS="${{SEEDS:-2345}}"
PERCENTAGES="${{PERCENTAGES:-10 15 20}}"
ITERS="${{ITERS:-1000}}"
{extra}
DESIGN="{design}"
SCRIPT_DIR="$(cd "$(dirname "${{BASH_SOURCE[0]}}")" && pwd)"
LOG_DIR="${{SCRIPT_DIR}}/../../qor_{tag}/${{DESIGN}}"
TCL="{tcl}"
mkdir -p "${{LOG_DIR}}"
[[ -f "${{TCL}}" ]] || {{ echo "ERROR: ${{TCL}} not found"; exit 1; }}

total=0
for _s in ${{SEEDS}}; do for _p in ${{PERCENTAGES}}; do for _i in ${{ITERS}}; do
  total=$((total+1)); done; done; done

run=0
for seed in ${{SEEDS}}; do
  for pct in ${{PERCENTAGES}}; do
    for iter in ${{ITERS}}; do
      run=$((run+1))
      log="${{LOG_DIR}}/{tag}_seed${{seed}}_pct${{pct}}_iters${{iter}}.log"
      printf '[%d/%d] %s seed=%s pct=%s%% iters=%s\\n' "$run" "$total" "$DESIGN" "$seed" "$pct" "$iter"
      SEED="$seed" PERCENTAGE="$pct" ITERS="$iter" {extra_export} \\
        CORNER="slow" DEBUG_LEVEL="1" NO_COLOR=1 \\
        "${{OPENROAD}}" -no_init -exit "${{TCL}}" > "${{log}}" 2>&1
      rc=$?
      if [[ $rc -ne 0 ]]; then
        echo "  FAILED (exit $rc) — ${{log}}"
      else
        r=$(grep "^RESULT " "${{log}}" | tail -1)
        w0=$(echo "$r" | grep -o 'wns0=[^ ]*' | cut -d= -f2)
        w3=$(echo "$r" | grep -o 'wns3=[^ ]*' | cut -d= -f2)
        ms=$(echo "$r" | grep -o 'total_ms=[^ ]*' | cut -d= -f2)
        echo "  OK: baseline=${{w0}}ps → iter3=${{w3}}ps  total=${{ms}}ms"
      fi
    done; done; done
echo "Logs: ${{LOG_DIR}}/"
"""

# ── Master parallel runner ─────────────────────────────────────────────────────
MASTER = """#!/usr/bin/env bash
# Master parallel runner: 3-iter MCTS and 3-iter SA on all 7 designs.
# MCTS and SA run SEPARATELY (not chained together).
# Max 4 jobs in parallel.
set -u
OPENROAD="${OPENROAD:-openroad}"
SEEDS="${SEEDS:-2345}"
PERCENTAGES="${PERCENTAGES:-10 15 20}"
ITERS="${ITERS:-1000}"
MAX_DEPTH="${MAX_DEPTH:-10}"
ALGO="${ALGO:-both}"
MAX_JOBS="${MAX_JOBS:-4}"
DESIGNS="${DESIGNS:-gcd spi uart usb_phy sasc_top b13 i2c_master_top}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MASTER_LOG="${SCRIPT_DIR}/master_mcts3_run.log"
: > "${MASTER_LOG}"

log() { printf '[%s] %s\\n' "$(date +%H:%M:%S)" "$*" | tee -a "${MASTER_LOG}"; }

declare -a JOB_PIDS=(); declare -a JOB_LABELS=()

wait_for_slot() {
  while [[ ${#JOB_PIDS[@]} -ge ${MAX_JOBS} ]]; do
    local np=() nl=()
    for i in "${!JOB_PIDS[@]}"; do
      if kill -0 "${JOB_PIDS[$i]}" 2>/dev/null; then
        np+=("${JOB_PIDS[$i]}"); nl+=("${JOB_LABELS[$i]}")
      else
        wait "${JOB_PIDS[$i]}" 2>/dev/null; rc=$?
        [[ $rc -ne 0 ]] && log "DONE [FAILED rc=$rc] ${JOB_LABELS[$i]}" || log "DONE [OK] ${JOB_LABELS[$i]}"
      fi
    done
    JOB_PIDS=("${np[@]+"${np[@]}"}"); JOB_LABELS=("${nl[@]+"${nl[@]}"}")
    [[ ${#JOB_PIDS[@]} -ge ${MAX_JOBS} ]] && sleep 5
  done
}
wait_all() {
  for i in "${!JOB_PIDS[@]}"; do
    wait "${JOB_PIDS[$i]}" 2>/dev/null; rc=$?
    [[ $rc -ne 0 ]] && log "DONE [FAILED rc=$rc] ${JOB_LABELS[$i]}" || log "DONE [OK] ${JOB_LABELS[$i]}"
  done; JOB_PIDS=(); JOB_LABELS=()
}
launch() {
  local label="$1"; shift; wait_for_slot; log "START $label"
  "$@" >> "${MASTER_LOG}" 2>&1 & JOB_PIDS+=($!); JOB_LABELS+=("$label")
}

run_design() {
  local tag="$1" design="$2"
  local sdir="${SCRIPT_DIR}/mcts3_script/${design}"
  local sh="${design}_bench_routed_qor_${tag}.sh"
  [[ -f "${sdir}/${sh}" ]] || { log "ERROR: ${sdir}/${sh} not found"; return 1; }
  ( cd "${sdir}"
    OPENROAD="${OPENROAD}" SEEDS="${SEEDS}" PERCENTAGES="${PERCENTAGES}" \\
      ITERS="${ITERS}" MAX_DEPTH="${MAX_DEPTH}" NO_COLOR=1 bash "./${sh}" )
}

ts=$(date +%s)
log "=== 3-iter MCTS + 3-iter SA Sweep START ==="
log "Designs: ${DESIGNS}  |  Pcts: ${PERCENTAGES}  |  Iters/call: ${ITERS}"
log "MCTS max_depth: ${MAX_DEPTH}  |  Max parallel jobs: ${MAX_JOBS}"

for design in ${DESIGNS}; do
  [[ "$ALGO" == "both" || "$ALGO" == "mcts" ]] && launch "MCTS3/${design}" run_design mcts3 "${design}"
  [[ "$ALGO" == "both" || "$ALGO" == "sa"   ]] && launch "SA3/${design}"   run_design sa3   "${design}"
done

log "All launched, waiting..."
wait_all
elapsed=$(( $(date +%s) - ts ))
log "=== ALL DONE: ${elapsed}s ($(( elapsed/3600 ))h $(( (elapsed%3600)/60 ))m) ==="
log "MCTS logs: ${SCRIPT_DIR}/qor_mcts3/    SA logs: ${SCRIPT_DIR}/qor_sa3/"
"""

# ── write everything ──────────────────────────────────────────────────────────
def write(path, content, exe=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(content)
    if exe:
        os.chmod(path, os.stat(path).st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    print(f"  {path}")

if __name__ == "__main__":
    print("Generating 3-iter MCTS and SA scripts...")
    for design, dpath in DESIGNS.items():
        d = os.path.join(BASE, "mcts3_script", design)
        write(f"{d}/{design}_routed_qor_mcts3.tcl", make_mcts_tcl(design, dpath))
        write(f"{d}/{design}_routed_qor_sa3.tcl",   make_sa_tcl(design, dpath))
        write(f"{d}/{design}_bench_routed_qor_mcts3.sh", make_sh(design,"mcts"), exe=True)
        write(f"{d}/{design}_bench_routed_qor_sa3.sh",   make_sh(design,"sa"),   exe=True)
        os.makedirs(os.path.join(BASE, "qor_mcts3", design), exist_ok=True)
        os.makedirs(os.path.join(BASE, "qor_sa3",   design), exist_ok=True)

    write(os.path.join(BASE, "run_mcts3_parallel.sh"), MASTER, exe=True)
    print(f"\nAll done. To launch:")
    print(f"  cd {BASE}")
    print(f"  OPENROAD=/home/mani-deep-g/OpenROAD/build/bin/openroad MAX_JOBS=4 bash run_mcts3_parallel.sh")
