#!/usr/bin/env bash
# Master runner: MCTS + SA sweep on all 8 benchmark designs.
# Settings: seed=2345, pct=10/15/20, iters=1000
#
# Usage:
#   ./run_all_benchmarks.sh                         # run everything
#   ALGO=mcts ./run_all_benchmarks.sh               # MCTS only
#   ALGO=sa   ./run_all_benchmarks.sh               # SA only
#   DESIGNS="b12 b13" ./run_all_benchmarks.sh       # specific designs
#   OPENROAD=/path/to/openroad ./run_all_benchmarks.sh
set -u

OPENROAD="${OPENROAD:-openroad}"
SEEDS="${SEEDS:-2345}"
PERCENTAGES="${PERCENTAGES:-10 15 20}"
ITERS="${ITERS:-1000}"
ALGO="${ALGO:-both}"       # mcts | sa | both
# 8 designs: OpenCores (gcd, spi, uart, usb_phy, sasc_top, i2c_master_top)
#           + ITC99     (b13, b12)
DESIGNS="${DESIGNS:-gcd spi uart usb_phy sasc_top b13 i2c_master_top b12}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  C_RESET=$'\e[0m'; C_BOLD=$'\e[1m'; C_CYAN=$'\e[36m'; C_GREEN=$'\e[32m'; C_DIM=$'\e[2m'
else
  C_RESET=''; C_BOLD=''; C_CYAN=''; C_GREEN=''; C_DIM=''
fi

run_algo() {
  local algo="$1" design="$2"
  local script_dir log_tag

  if [[ "$algo" == "mcts" ]]; then
    script_dir="${SCRIPT_DIR}/mcts_script/${design}"
    local sh="${design}_bench_routed_qor_mcts.sh"
  else
    script_dir="${SCRIPT_DIR}/sa_script/${design}"
    local sh="${design}_bench_routed_qor.sh"
  fi

  if [[ ! -f "${script_dir}/${sh}" ]]; then
    echo "ERROR: ${script_dir}/${sh} not found" >&2
    return 1
  fi

  printf '\n%s%s>>> [%s] %s%s\n' "${C_BOLD}" "${C_CYAN}" "${algo^^}" "${design}" "${C_RESET}"

  (
    cd "${script_dir}"
    OPENROAD="${OPENROAD}" SEEDS="${SEEDS}" PERCENTAGES="${PERCENTAGES}" \
      ITERS="${ITERS}" NO_COLOR="${NO_COLOR:-}" \
      bash "./${sh}"
  )
}

total_start=$(date +%s)

for design in ${DESIGNS}; do
  [[ "$ALGO" == "both" || "$ALGO" == "mcts" ]] && run_algo mcts "${design}"
  [[ "$ALGO" == "both" || "$ALGO" == "sa"   ]] && run_algo sa   "${design}"
done

total_elapsed=$(( $(date +%s) - total_start ))
printf '\n%sAll benchmarks complete. Total wall time: %ds%s\n' "${C_DIM}" "${total_elapsed}" "${C_RESET}"
