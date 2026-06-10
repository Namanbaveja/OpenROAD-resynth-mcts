#!/usr/bin/env bash
# Combined parallel sweep:
#   Group 1 — Standard designs: full eval, 1500 iters, seed=3500, pct=10/20/30/40 (MCTS+SA)
#   Group 2 — IWLS designs:    tiered eval, 20000 iters, seed=2345, pct=2/4/6 (MCTS only)
set -u

OPENROAD="${OPENROAD:-/home/mani-deep-g/OpenROAD/build/bin/openroad}"
MAX_JOBS="${MAX_JOBS:-6}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MASTER_LOG="${SCRIPT_DIR}/master_run.log"
: > "${MASTER_LOG}"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "${MASTER_LOG}"; }

declare -a JOB_PIDS=()
declare -a JOB_LABELS=()

wait_for_slot() {
  while [[ ${#JOB_PIDS[@]} -ge ${MAX_JOBS} ]]; do
    local new_pids=() new_labels=()
    for i in "${!JOB_PIDS[@]}"; do
      if kill -0 "${JOB_PIDS[$i]}" 2>/dev/null; then
        new_pids+=("${JOB_PIDS[$i]}"); new_labels+=("${JOB_LABELS[$i]}")
      else
        wait "${JOB_PIDS[$i]}" 2>/dev/null; rc=$?
        log "DONE [$([ $rc -eq 0 ] && echo OK || echo FAILED rc=$rc)] ${JOB_LABELS[$i]}"
      fi
    done
    JOB_PIDS=("${new_pids[@]+"${new_pids[@]}"}")
    JOB_LABELS=("${new_labels[@]+"${new_labels[@]}"}")
    [[ ${#JOB_PIDS[@]} -ge ${MAX_JOBS} ]] && sleep 5
  done
}

wait_all() {
  for i in "${!JOB_PIDS[@]}"; do
    wait "${JOB_PIDS[$i]}" 2>/dev/null; rc=$?
    log "DONE [$([ $rc -eq 0 ] && echo OK || echo FAILED rc=$rc)] ${JOB_LABELS[$i]}"
  done
  JOB_PIDS=(); JOB_LABELS=()
}

launch() {
  local label="$1"; shift
  wait_for_slot
  log "START $label"
  "$@" >> "${MASTER_LOG}" 2>&1 &
  JOB_PIDS+=($!); JOB_LABELS+=("$label")
}

run_mcts() {
  local design="$1" seeds="$2" pcts="$3" iters="$4" eval_mode="$5"
  local script_dir="${SCRIPT_DIR}/mcts_script/${design}"
  local sh="${design}_bench_routed_qor_mcts.sh"
  [[ ! -f "${script_dir}/${sh}" ]] && log "ERROR: ${script_dir}/${sh} not found" && return 1
  (cd "${script_dir}" && OPENROAD="${OPENROAD}" SEEDS="${seeds}" PERCENTAGES="${pcts}" \
     ITERS="${iters}" EVAL_MODE="${eval_mode}" NO_COLOR=1 bash "./${sh}")
}

run_sa() {
  local design="$1" seeds="$2" pcts="$3" iters="$4"
  local script_dir="${SCRIPT_DIR}/sa_script/${design}"
  local sh="${design}_bench_routed_qor.sh"
  [[ ! -f "${script_dir}/${sh}" ]] && log "ERROR: ${script_dir}/${sh} not found" && return 1
  (cd "${script_dir}" && OPENROAD="${OPENROAD}" SEEDS="${seeds}" PERCENTAGES="${pcts}" \
     ITERS="${iters}" NO_COLOR=1 bash "./${sh}")
}

total_start=$(date +%s)
log "=== Combined benchmark sweep START ==="
log "Group 1: gcd uart usb_phy b13 i2c — full eval, 1500 iters, seed=3500, pct=10/20/30/40 (MCTS+SA)"
log "Group 2: wb_dma tv80 mem_ctrl ac97 — tiered eval, 20000 iters, seed=2345, pct=2/4/6 (MCTS+SA)"
log "Max jobs: ${MAX_JOBS}  |  OpenROAD: ${OPENROAD}"

# ── Group 1: standard designs (MCTS + SA) ─────────────────────────────────────
for design in gcd uart usb_phy b13 i2c_master_top; do
  launch "MCTS/${design}"   run_mcts "${design}" "3500" "10 20 30 40" "1500" "full"
  launch "SA/${design}"     run_sa   "${design}" "3500" "10 20 30 40" "1500"
done

# ── Group 2: IWLS designs (MCTS + SA, tiered) ────────────────────────────────
for design in wb_dma tv80 mem_ctrl ac97; do
  launch "MCTS-T/${design}" run_mcts "${design}" "2345" "2 4 6" "20000" "tiered"
  launch "SA-T/${design}"   run_sa   "${design}" "2345" "2 4 6" "20000"
done

log "All jobs launched — waiting for completion..."
wait_all

total_elapsed=$(( $(date +%s) - total_start ))
log "=== ALL DONE. Total: ${total_elapsed}s ($(( total_elapsed/3600 ))h $(( (total_elapsed%3600)/60 ))m) ==="
