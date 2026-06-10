#!/usr/bin/env bash
# Sweep resynth_annealing across seeds, percentages, and iterations on the
# post-PnR mem_ctrl_final design and aggregate baseline-vs-after WNS/TNS/FEPs QoR.
#
# Live output:
#   - Per-iter progress bar parsed from OpenRoad's
#       "[INFO RMP-0054] Iter N/TOTAL T=... best composite=... best WNS=..."
#     lines, with current best WNS shown next to the bar.
#   - Color-coded WNS/TNS/FEP deltas in the final table
#     (green = improved, red = worsened, dim = zero / no change).
#
# Requirements: openroad in PATH, run from sa_script/mem_ctrl/.
#   ./mem_ctrl_bench_routed_qor.sh
#   SEEDS="50 100" PERCENTAGES="10 20 30" ITERS="100 200" ./mem_ctrl_bench_routed_qor.sh
#   OPENROAD=../../../build/bin/openroad ./mem_ctrl_bench_routed_qor.sh
#   DEBUG_LEVEL=0 ./mem_ctrl_bench_routed_qor.sh   # suppress per-iter debug noise
#   EXPORT_FILES=1 ./mem_ctrl_bench_routed_qor.sh  # also write DEF/v/sdc/spef
#   NO_COLOR=1 ./mem_ctrl_bench_routed_qor.sh      # disable ANSI colors
set -u

OPENROAD="${OPENROAD:-openroad}"
SEEDS="${SEEDS:-3500}"
PERCENTAGES="${PERCENTAGES:-10 20 30 40}"
ITERS="${ITERS:-1500}"
CORNER="${CORNER:-slow}"
DEBUG_LEVEL="${DEBUG_LEVEL:-1}"
EVAL_MODE="${EVAL_MODE:-tiered}"
EXPORT_FILES="${EXPORT_FILES:-0}"
TCL="${TCL:-mem_ctrl_routed_qor_orig.tcl}"

# Derive design name from the TCL filename (mem_ctrl_routed_qor.tcl → "mem_ctrl").
DESIGN="${DESIGN:-mem_ctrl}"


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="${SCRIPT_DIR}/../.."
LOG_ROOT="${LOG_ROOT:-${TEST_DIR}/qor_sa_orig}"
LOG_DIR="${LOG_DIR:-${LOG_ROOT}/${DESIGN}}"

if [[ ! -f "${TCL}" ]]; then
  echo "ERROR: ${TCL} not found; run from sa_script/mem_ctrl/" >&2
  exit 1
fi

mkdir -p "${LOG_DIR}"

# ---- colors ------------------------------------------------------------------
# Disable if stdout isn't a TTY, or if NO_COLOR is set (https://no-color.org/).
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  C_RESET=$'\e[0m'
  C_BOLD=$'\e[1m'
  C_DIM=$'\e[2m'
  C_RED=$'\e[31m'
  C_GREEN=$'\e[32m'
  C_YELLOW=$'\e[33m'
  C_CYAN=$'\e[36m'
  CLR_LINE=$'\e[2K\r'
else
  C_RESET=''; C_BOLD=''; C_DIM=''
  C_RED=''; C_GREEN=''; C_YELLOW=''; C_CYAN=''
  CLR_LINE=$'\r'
fi

# ---- persistent results ------------------------------------------------------
timestamp="$(date +%Y%m%d_%H%M%S)"
results_tsv="${LOG_DIR}/${DESIGN}_results_${timestamp}.tsv"
results_txt="${LOG_DIR}/${DESIGN}_results_${timestamp}.txt"

results_file="$(mktemp)"
trap 'rm -f "${results_file}"' EXIT

printf 'seed\tpct\titers\twns_s1_baseline\ttns_s1_baseline\tviols_s1\twns_s2_repair\ttns_s2_repair\tviols_s2\twns_s3_restructure\ttns_s3_restructure\tviols_s3\twns_s4_final\ttns_s4_final\tviols_s4\twns_delta_ps\ttns_delta_ps\tviols_delta\tsa_changed\telapsed_ms\n' \
  > "${results_file}"

# Field extractor: get() reads key=value from the RESULT line.
get() { local key="$1" line="$2"; sed -nE "s/.* ${key}=([^ ]+).*/\1/p" <<<"${line}"; }

# ---- progress bar renderer ---------------------------------------------------
# Reads OpenRoad's stdout/stderr line-by-line, writes everything to the log,
# and updates an in-place progress bar on stderr whenever it sees an
# "[INFO RMP-0054] Iter N/TOTAL ..." line. Throttled to ~10 redraws/sec so a
# 2500-iter run doesn't spam the terminal.
render_progress() {
  local logfile="$1" total_iters="$2" header="$3"
  local bar_width=30
  local last_draw=0
  local cur=0 best_wns="" done_drawn=0

  # Use awk for line buffering + numeric throttling; bash regex would work but
  # awk's match() with capture groups is cleaner here.
  awk -v logfile="${logfile}" \
      -v total="${total_iters}" \
      -v width="${bar_width}" \
      -v header="${header}" \
      -v cyan="${C_CYAN}" -v dim="${C_DIM}" -v bold="${C_BOLD}" -v reset="${C_RESET}" \
      -v clr="${CLR_LINE}" '
    BEGIN {
      last_ms = 0
      cur = 0
      best = ""
      # Print initial empty bar so the user sees something immediately.
      draw(0, "")
    }
    function now_ms(   t) {
      # gawk has systime()*1000 + ... but for portability use date via getline.
      "date +%s%3N" | getline t
      close("date +%s%3N")
      return t + 0
    }
    function draw(n, wns,   filled, empty, pct, i, bar) {
      pct = (total > 0) ? (n * 100.0 / total) : 0
      filled = int(width * n / (total > 0 ? total : 1))
      if (filled > width) filled = width
      bar = ""
      for (i = 0; i < filled; i++) bar = bar "#"
      for (i = filled; i < width; i++) bar = bar "-"
      if (wns == "") wns = "—"
      printf "%s%s %s[%s%s%s] %d/%d (%.0f%%)  best WNS=%s%s%s",
             clr, header, dim, reset, bar, dim, n, total, pct,
             bold, wns, reset > "/dev/stderr"
      fflush("/dev/stderr")
    }
    {
      # Always tee to logfile.
      print $0 >> logfile

      # Match the per-iter progress line.
      if (match($0, /Iter[ \t]+([0-9]+)\/([0-9]+).*best WNS=([^ ]+)/, m)) {
        cur = m[1] + 0
        best = m[3]
        nowms = now_ms()
        # Throttle: redraw at most every 100ms, but always redraw the final iter.
        if (nowms - last_ms >= 100 || cur == total) {
          draw(cur, best)
          last_ms = nowms
        }
      }
    }
    END {
      # Force a final draw so the bar shows 100% before we move on.
      draw(cur, best)
      close(logfile)
    }
  '
  # Newline so the next line of output doesnt overwrite the bar.
  printf '\n' >&2
}

# ---- helper: colorize a signed delta -----------------------------------------
# Args: delta_value, mode (wns|tns|fep)
#   For WNS/TNS:  positive = improved (less negative slack) → green
#                 negative = worsened → red
#   For FEPs:     negative = improved (fewer violations) → green
#                 positive = worsened → red
#                 zero     = dim
fmt_delta() {
  local val="$1" mode="$2" color=""
  # Treat empty/missing as dim zero.
  if [[ -z "${val}" || "${val}" == "0" || "${val}" == "0.0" || "${val}" == "+0" ]]; then
    printf '%s%s%s' "${C_DIM}" "${val:-0}" "${C_RESET}"
    return
  fi
  case "${mode}" in
    wns|tns)
      # awk for portable float comparison.
      if awk -v v="${val}" 'BEGIN{exit !(v+0 > 0)}'; then color="${C_GREEN}"
      elif awk -v v="${val}" 'BEGIN{exit !(v+0 < 0)}'; then color="${C_RED}"
      else color="${C_DIM}"; fi
      ;;
    fep)
      if awk -v v="${val}" 'BEGIN{exit !(v+0 < 0)}'; then color="${C_GREEN}"
      elif awk -v v="${val}" 'BEGIN{exit !(v+0 > 0)}'; then color="${C_RED}"
      else color="${C_DIM}"; fi
      ;;
  esac
  printf '%s%s%s' "${color}" "${val}" "${C_RESET}"
}

# ---- sweep -------------------------------------------------------------------
# Pre-count total runs for a [n/N] indicator.
total_planned=0
for _s in ${SEEDS}; do for _p in ${PERCENTAGES}; do for _i in ${ITERS}; do
  total_planned=$((total_planned + 1))
done; done; done

run_idx=0
total_runs=0
changed_runs=0
sweep_start=$(date +%s)

printf '%s%sSweeping %s: %d run(s) total%s\n\n' \
  "${C_BOLD}" "${C_CYAN}" "${DESIGN}" "${total_planned}" "${C_RESET}" >&2

for seed in ${SEEDS}; do
  for pct in ${PERCENTAGES}; do
    for iter in ${ITERS}; do
      run_idx=$((run_idx + 1))
      log="${LOG_DIR}/sa_seed${seed}_pct${pct}_iters${iter}.log"
      # Truncate log up front since render_progress appends.
      : > "${log}"

      header=$(printf '%s[%d/%d]%s seed=%s pct=%s%% iters=%s' \
        "${C_BOLD}" "${run_idx}" "${total_planned}" "${C_RESET}" \
        "${seed}" "${pct}" "${iter}")
      printf '%s\n' "${header}" >&2

      run_start=$(date +%s)
      # Stream OpenRoad's combined stdout/stderr through the progress renderer,
      # which also tees to the log file.
      SEED="${seed}" PERCENTAGE="${pct}" ITERS="${iter}" CORNER="${CORNER}" \
        DEBUG_LEVEL="${DEBUG_LEVEL}" EVAL_MODE="${EVAL_MODE}" EXPORT_FILES="${EXPORT_FILES}" \
        "${OPENROAD}" -no_init -exit "${TCL}" 2>&1 \
        | render_progress "${log}" "${iter}" "  progress:"
      rc=${PIPESTATUS[0]}
      run_elapsed=$(( $(date +%s) - run_start ))

      if [[ ${rc} -ne 0 ]]; then
        printf '  %sFAILED%s (exit %d, %ds); see %s\n\n' \
          "${C_RED}${C_BOLD}" "${C_RESET}" "${rc}" "${run_elapsed}" "${log}" >&2
        continue
      fi

      result_line="$(grep -E '^RESULT ' "${log}" | tail -n1)"
      if [[ -z "${result_line}" ]]; then
        printf '  %sno RESULT line%s in %s (%ds)\n\n' \
          "${C_YELLOW}" "${C_RESET}" "${log}" "${run_elapsed}" >&2
        continue
      fi

      wns_b=$(get wns_before_ps  "${result_line}")
      wns_a=$(get wns_after_ps   "${result_line}")
      wns_d=$(get wns_delta_ps   "${result_line}")
      tns_b=$(get tns_before_ps  "${result_line}")
      tns_a=$(get tns_after_ps   "${result_line}")
      tns_d=$(get tns_delta_ps   "${result_line}")
      viols_b=$(get viols_before "${result_line}")
      viols_a=$(get viols_after  "${result_line}")
      viols_d=$(get viols_delta  "${result_line}")
      sa_chg=$(get sa_changed    "${result_line}")
      ms=$(get elapsed_ms        "${result_line}")

      # Stage 2 — after repair_timing only (from C++ QOR_STAGE log line)
      stage2_line="$(grep -E 'QOR_STAGE stage=repair_only' "${log}" | tail -1)"
      wns_r=$(get wns_ps "${stage2_line}"); wns_r="${wns_r:-${wns_b}}"
      tns_r=$(get tns_ps "${stage2_line}"); tns_r="${tns_r:-${tns_b}}"
      viols_r=$(get viols "${stage2_line}"); viols_r="${viols_r:-${viols_b}}"

      # Stage 3 — after restructuring + stitch-back, before repair_timing (from C++ QOR_STAGE)
      stage3_line="$(grep -E 'QOR_STAGE stage=restructure_only' "${log}" | tail -1)"
      wns_rs=$(get wns_ps "${stage3_line}"); wns_rs="${wns_rs:-${wns_a}}"
      tns_rs=$(get tns_ps "${stage3_line}"); tns_rs="${tns_rs:-${tns_a}}"
      viols_rs=$(get viols "${stage3_line}"); viols_rs="${viols_rs:-${viols_a}}"

      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${seed}" "${pct}" "${iter}" \
        "${wns_b}"  "${tns_b}"  "${viols_b}" \
        "${wns_r}"  "${tns_r}"  "${viols_r}" \
        "${wns_rs}" "${tns_rs}" "${viols_rs}" \
        "${wns_a}"  "${tns_a}"  "${viols_a}" \
        "${wns_d}"  "${tns_d}"  "${viols_d}" \
        "${sa_chg}" "${ms}" >> "${results_file}"

      printf '  %-24s WNS=%10s  TNS=%14s  FEPs=%s\n' \
        "Stage1 baseline:"   "${wns_b}"  "${tns_b}"  "${viols_b}"
      printf '  %-24s WNS=%10s  TNS=%14s  FEPs=%s\n' \
        "Stage2 repair_only:" "${wns_r}"  "${tns_r}"  "${viols_r}"
      printf '  %-24s WNS=%10s  TNS=%14s  FEPs=%s\n' \
        "Stage3 restructure:" "${wns_rs}" "${tns_rs}" "${viols_rs}"
      printf '  %-24s WNS=%10s  TNS=%14s  FEPs=%s\n' \
        "Stage4 final:"       "${wns_a}"  "${tns_a}"  "${viols_a}"
      printf '  Delta (S4-S1):          WNS=%s TNS=%s FEPs=%s\n' \
        "$(fmt_delta "${wns_d}" wns)" "$(fmt_delta "${tns_d}" tns)" "$(fmt_delta "${viols_d}" fep)"
      if [[ "${sa_chg}" == "YES" ]]; then
        printf '  SA changed netlist: %s%s%s\n' "${C_GREEN}" "${sa_chg}" "${C_RESET}"
      else
        printf '  SA changed netlist: %s%s%s\n' "${C_DIM}" "${sa_chg}" "${C_RESET}"
      fi
      printf '  TIME %s ms (%ds wall)\n\n' "${ms}" "${run_elapsed}"

      total_runs=$((total_runs + 1))
      [[ "${sa_chg}" == "YES" ]] && changed_runs=$((changed_runs + 1))
    done
  done
done

sweep_elapsed=$(( $(date +%s) - sweep_start ))

# ---- summary -----------------------------------------------------------------
# Build summary into a string so we can both print (with color) and save (without).
summary_plain="$(
  echo ""
  echo "============================================= SUMMARY ${DESIGN^^} RESULTS ============================================="
  column -t -s $'\t' < "${results_file}"
  awk -F'\t' '
    NR>1 {
      wd += $16; td += $17; vd += $18; n += 1
      if ($19 == "YES") ch += 1
    }
    END {
      if (n > 0)
        printf "\nMean over %d runs: WNS delta = %+.4f ps | TNS delta = %+.4f ps | FEP delta = %+.1f | SA changed = %d/%d (%.0f%%)\n",
               n, wd/n, td/n, vd/n, ch, n, 100.0*ch/n
    }' "${results_file}"
)"

# Save plain (no color) summary.
printf '%s\n' "${summary_plain}" > "${results_txt}"

# Print colored summary to terminal: re-render the table with fmt_delta on the
# delta columns. Plain `column` output doesn't tolerate ANSI escapes well
# (they confuse its width math), so we re-run column on a colored TSV.
colored_tsv="$(mktemp)"
trap 'rm -f "${results_file}" "${colored_tsv}"' EXIT

# Header row — bold.
head -n1 "${results_file}" | awk -v b="${C_BOLD}" -v r="${C_RESET}" \
  'BEGIN{FS=OFS="\t"} {for(i=1;i<=NF;i++) $i=b $i r; print}' > "${colored_tsv}"

# Data rows — colorize delta columns (6=wns_delta, 9=tns_delta, 12=viols_delta).
tail -n +2 "${results_file}" | while IFS=$'\t' read -r f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11 f12 f13 f14 f15 f16 f17 f18 f19 f20; do
  cf16=$(fmt_delta "${f16}" wns)
  cf17=$(fmt_delta "${f17}" tns)
  cf18=$(fmt_delta "${f18}" fep)
  if [[ "${f19}" == "YES" ]]; then
    cf19="${C_GREEN}${f19}${C_RESET}"
  else
    cf19="${C_DIM}${f19}${C_RESET}"
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "${f1}" "${f2}" "${f3}" \
    "${f4}" "${f5}" "${f6}" \
    "${f7}" "${f8}" "${f9}" \
    "${f10}" "${f11}" "${f12}" \
    "${f13}" "${f14}" "${f15}" \
    "${cf16}" "${cf17}" "${cf18}" \
    "${cf19}" "${f20}" >> "${colored_tsv}"
done

echo ""
printf '%s%s============================================= SUMMARY %s RESULTS =============================================%s\n' \
  "${C_BOLD}" "${C_CYAN}" "${DESIGN^^}" "${C_RESET}"
column -t -s $'\t' < "${colored_tsv}"

# Mean line (colored).
awk -F'\t' \
    -v g="${C_GREEN}" -v r="${C_RED}" -v d="${C_DIM}" -v b="${C_BOLD}" -v x="${C_RESET}" '
  NR>1 {
    wd += $16; td += $17; vd += $18; n += 1
    if ($19 == "YES") ch += 1
  }
  function col_wns(v) { return (v+0 > 0) ? g : (v+0 < 0) ? r : d }
  function col_fep(v) { return (v+0 < 0) ? g : (v+0 > 0) ? r : d }
  END {
    if (n > 0) {
      wd_avg = wd/n; td_avg = td/n; vd_avg = vd/n
      printf "\n%sMean over %d runs:%s WNS delta = %s%+.4f ps%s | TNS delta = %s%+.4f ps%s | FEP delta = %s%+.1f%s | SA changed = %d/%d (%.0f%%)\n",
             b, n, x,
             col_wns(wd_avg), wd_avg, x,
             col_wns(td_avg), td_avg, x,
             col_fep(vd_avg), vd_avg, x,
             ch, n, 100.0*ch/n
    }
  }' "${results_file}"

printf '\n%sSweep elapsed: %ds%s\n' "${C_DIM}" "${sweep_elapsed}" "${C_RESET}"

# Save raw TSV for post-processing / spreadsheet import.
cp "${results_file}" "${results_tsv}"
echo ""
echo "Raw TSV  : ${results_tsv}"
echo "Summary  : ${results_txt}"
echo "Log dir  : ${LOG_DIR}/"