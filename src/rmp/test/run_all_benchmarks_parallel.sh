#!/usr/bin/env bash
# Parallel master runner: MCTS + SA sweep on all 8 benchmark designs.
# Runs up to MAX_JOBS design+algo pairs simultaneously.
#
# Usage:
#   ./run_all_benchmarks_parallel.sh
#   MAX_JOBS=6 ./run_all_benchmarks_parallel.sh
#   ALGO=mcts ./run_all_benchmarks_parallel.sh
#   OPENROAD=/path/to/openroad ./run_all_benchmarks_parallel.sh
set -u

OPENROAD="${OPENROAD:-openroad}"
SEEDS="${SEEDS:-2345}"
PERCENTAGES="${PERCENTAGES:-10 15 20}"
ITERS="${ITERS:-1000}"
ALGO="${ALGO:-both}"        # mcts | sa | both
MAX_JOBS="${MAX_JOBS:-4}"   # parallel job slots (safe for 16-core/15GB)
DESIGNS="${DESIGNS:-gcd spi uart usb_phy sasc_top b13 i2c_master_top b12}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MASTER_LOG="${SCRIPT_DIR}/master_run.log"
: > "${MASTER_LOG}"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "${MASTER_LOG}"; }

# ---- job pool ----------------------------------------------------------------
# We track child PIDs + their labels in parallel arrays.
declare -a JOB_PIDS=()
declare -a JOB_LABELS=()

wait_for_slot() {
  # Block until fewer than MAX_JOBS children are running.
  while [[ ${#JOB_PIDS[@]} -ge ${MAX_JOBS} ]]; do
    local new_pids=() new_labels=()
    for i in "${!JOB_PIDS[@]}"; do
      if kill -0 "${JOB_PIDS[$i]}" 2>/dev/null; then
        new_pids+=("${JOB_PIDS[$i]}")
        new_labels+=("${JOB_LABELS[$i]}")
      else
        wait "${JOB_PIDS[$i]}" 2>/dev/null
        rc=$?
        if [[ $rc -ne 0 ]]; then
          log "DONE [FAILED rc=$rc] ${JOB_LABELS[$i]}"
        else
          log "DONE [OK] ${JOB_LABELS[$i]}"
        fi
      fi
    done
    JOB_PIDS=("${new_pids[@]+"${new_pids[@]}"}")
    JOB_LABELS=("${new_labels[@]+"${new_labels[@]}"}")
    [[ ${#JOB_PIDS[@]} -ge ${MAX_JOBS} ]] && sleep 5
  done
}

wait_all() {
  for i in "${!JOB_PIDS[@]}"; do
    wait "${JOB_PIDS[$i]}" 2>/dev/null
    rc=$?
    if [[ $rc -ne 0 ]]; then
      log "DONE [FAILED rc=$rc] ${JOB_LABELS[$i]}"
    else
      log "DONE [OK] ${JOB_LABELS[$i]}"
    fi
  done
  JOB_PIDS=(); JOB_LABELS=()
}

launch() {
  local label="$1"; shift
  wait_for_slot
  log "START $label"
  "$@" >> "${MASTER_LOG}" 2>&1 &
  JOB_PIDS+=($!)
  JOB_LABELS+=("$label")
}

# ---- per-design runner -------------------------------------------------------
run_design_algo() {
  local algo="$1" design="$2"
  if [[ "$algo" == "mcts" ]]; then
    local script_dir="${SCRIPT_DIR}/mcts_script/${design}"
    local sh="${design}_bench_routed_qor_mcts.sh"
  else
    local script_dir="${SCRIPT_DIR}/sa_script/${design}"
    local sh="${design}_bench_routed_qor.sh"
  fi

  if [[ ! -f "${script_dir}/${sh}" ]]; then
    log "ERROR: ${script_dir}/${sh} not found"
    return 1
  fi

  (
    cd "${script_dir}"
    OPENROAD="${OPENROAD}" SEEDS="${SEEDS}" PERCENTAGES="${PERCENTAGES}" \
      ITERS="${ITERS}" NO_COLOR=1 \
      bash "./${sh}"
  )
}

# ---- main sweep --------------------------------------------------------------
total_start=$(date +%s)
log "=== Parallel benchmark sweep START ==="
log "Designs   : ${DESIGNS}"
log "Algorithm : ${ALGO}"
log "Pcts      : ${PERCENTAGES}"
log "Iters     : ${ITERS}"
log "Seed      : ${SEEDS}"
log "Max jobs  : ${MAX_JOBS}"
log "OpenROAD  : ${OPENROAD}"

for design in ${DESIGNS}; do
  [[ "$ALGO" == "both" || "$ALGO" == "mcts" ]] && \
    launch "MCTS/${design}" run_design_algo mcts "${design}"
  [[ "$ALGO" == "both" || "$ALGO" == "sa" ]] && \
    launch "SA/${design}"   run_design_algo sa   "${design}"
done

log "All jobs launched, waiting for completion..."
wait_all

total_elapsed=$(( $(date +%s) - total_start ))
log "=== ALL DONE. Total wall time: ${total_elapsed}s ($(( total_elapsed/3600 ))h $(( (total_elapsed%3600)/60 ))m) ==="
log "Results in: ${SCRIPT_DIR}/qor_mcts/  and  ${SCRIPT_DIR}/qor_sa/"
