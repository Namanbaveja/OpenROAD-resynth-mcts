#!/usr/bin/env bash
# Group 2: IWLS designs — tiered eval (kCheap + kFull top-5)
# wb_dma, tv80, mem_ctrl, ac97 — MCTS + SA
#
# Usage:
#   ./run_group2_parallel.sh                                  # defaults
#   ITERS=500 PERCENTAGES="2 4 6" ./run_group2_parallel.sh   # custom
set -u

OPENROAD="${OPENROAD:-/home/mani-deep-g/OpenROAD/build/bin/openroad}"
SEEDS="${SEEDS:-2345}"
PERCENTAGES="${PERCENTAGES:-2 4 6}"
ITERS="${ITERS:-500}"
EVAL_MODE="${EVAL_MODE:-tiered}"
MAX_JOBS="${MAX_JOBS:-4}"
DESIGNS="${DESIGNS:-wb_dma tv80 mem_ctrl ac97}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_ROOT="${SCRIPT_DIR}"
MASTER_LOG="${LOG_ROOT}/master_group2.log"
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
  local design="$1"
  local d="${SCRIPT_DIR}/mcts_script/${design}"
  (cd "$d" && OPENROAD="${OPENROAD}" SEEDS="${SEEDS}" PERCENTAGES="${PERCENTAGES}" \
     ITERS="${ITERS}" EVAL_MODE="${EVAL_MODE}" NO_COLOR=1 \
     bash "./${design}_bench_routed_qor_mcts.sh")
}

run_sa() {
  local design="$1"
  local d="${SCRIPT_DIR}/sa_script/${design}"
  (cd "$d" && OPENROAD="${OPENROAD}" SEEDS="${SEEDS}" PERCENTAGES="${PERCENTAGES}" \
     ITERS="${ITERS}" EVAL_MODE="${EVAL_MODE}" NO_COLOR=1 \
     bash "./${design}_bench_routed_qor.sh")
}

total_start=$(date +%s)
log "=== Group 2 sweep START ==="
log "Designs    : ${DESIGNS}"
log "Seed       : ${SEEDS}"
log "Pcts       : ${PERCENTAGES}"
log "Iters      : ${ITERS}"
log "Eval mode  : ${EVAL_MODE}"
log "Max jobs   : ${MAX_JOBS}"
log "OpenROAD   : ${OPENROAD}"

for design in ${DESIGNS}; do
  launch "MCTS-T/${design}" run_mcts "${design}"
  launch "SA-T/${design}"   run_sa   "${design}"
done

log "All jobs launched — waiting..."
wait_all

total_elapsed=$(( $(date +%s) - total_start ))
log "=== ALL DONE. Total: ${total_elapsed}s ($(( total_elapsed/3600 ))h $(( (total_elapsed%3600)/60 ))m) ==="
