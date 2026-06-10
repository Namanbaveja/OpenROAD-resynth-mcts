# Physically-Aware Logic Restructuring for Post-CTS Timing Closure

An extension of the OpenROAD **`rmp`** (restructure) module that performs
**physically-aware logic restructuring on a placed, post-CTS design**. Unlike
the upstream restructuring flow — which evaluates candidate restructurings on
*unplaced* cells with zero wire delay — this work scores every candidate on the
**real, incrementally-legalized, parasitic-loaded design** it would actually
produce. A Monte Carlo Tree Search (MCTS) / Simulated Annealing search explores
the space of ABC restructuring recipes against this physically-grounded score.

> **Key idea:** putting incremental placement + parasitic estimation *inside*
> the candidate-evaluation loop is what separates real, routing-clean timing
> recovery from physically-blind regressions.

This repository is a **fork of [OpenROAD](https://github.com/The-OpenROAD-Project/OpenROAD)**;
all new work lives in [`src/rmp/`](src/rmp/).

---

## Table of Contents
1. [What's New](#whats-new)
2. [Repository Layout](#repository-layout)
3. [Prerequisites](#prerequisites)
4. [Installation (build OpenROAD)](#installation)
5. [The Resynthesis Commands](#the-resynthesis-commands)
6. [Quick Start](#quick-start)
7. [Running the Benchmark Sweeps](#running-the-benchmark-sweeps)
8. [Interpreting the Output](#interpreting-the-output)
9. [Results](#results)
10. [Credits](#credits)

---

## What's New

On top of the upstream `rmp` cone-extraction and ABC-mapping primitives, this
fork adds:

* **In-loop physical evaluation** — each candidate is inserted, its new cells
  are legalized with incremental OpenDP, wire RC is refreshed on the dirty nets,
  and the candidate is scored on the resulting **TNS**, then rolled back via the
  OpenDB ECO journal.
* **Real per-cone delay budget** — ABC is mapped against
  `Δ_cone = min(required) − max(arrival)` instead of a unit-delay default.
* **Three search strategies** — `resynth_mcts` (MCTS over recipe prefixes),
  `resynth_sa` (simulated annealing with a tiered cheap/full evaluation and
  top-K finalists), and `resynth_annealing` (the upstream blind-oracle baseline,
  for comparison).
* **Tiered evaluation** — a fast incremental oracle (`kCheap`) scores every
  candidate during the search; a full `repair_setup` pass (`kFull`) is reserved
  for the top-5 finalists.
* **Benchmark harness** — sweep scripts that run the post-CTS flow across
  designs and `percentage` settings and report per-stage WNS/TNS/VEP.

---

## Repository Layout

```
src/rmp/
├── src/                         # C++ implementation
│   ├── cone_mcts_strategy.cpp   # MCTS search + UCB1 selection
│   ├── slack_tuning_strategy.cpp# tiered SA + top-K, baseline anchoring
│   ├── cone_manager.cpp         # cone extraction + per-cone delay budget
│   ├── annealing_strategy.cpp   # blind-oracle SA (upstream baseline)
│   └── rmp.tcl                  # Tcl command definitions
├── include/rmp/                 # public headers
└── test/
    ├── benchmark_cts/final_cts/ # post-CTS DEF + SDC per design (small subset)
    ├── sa_script/<design>/      # tiered-SA bench (resynth_sa)  -> qor_sa_tiered/
    ├── sa_orig_script/<design>/ # blind-oracle bench (resynth_annealing) -> qor_sa_orig/
    └── mcts_script/<design>/    # MCTS bench (resynth_mcts)
```

> **Note:** generated outputs (`test/qor_*`) and large benchmark DEFs
> (jpeg, ethmac, vga_led, ibex, …) are **git-ignored** to keep the repo small.
> The small paper designs (gcd, uart, usb_phy, b13, i2c, wb_dma, mem_ctrl) are
> included. See [Running the Benchmark Sweeps](#running-the-benchmark-sweeps)
> for how to add your own.

---

## Prerequisites

* A Linux machine (Ubuntu 20.04/22.04 recommended)
* Build tools and OpenROAD dependencies (CMake ≥ 3.16, a C++17 compiler, etc.)
* ~16 GB RAM (more for the large designs)

The exact dependency list is handled by OpenROAD's own installer.

---

## Installation

Clone your fork and build OpenROAD as usual:

```bash
git clone https://github.com/<your-username>/OpenROAD.git
cd OpenROAD
git submodule update --init --recursive

# install dependencies (run once)
sudo ./etc/DependencyInstaller.sh

# build
mkdir build && cd build
cmake ..
make -j$(nproc)
```

This produces the `openroad` binary at `build/bin/openroad`, with the modified
`rmp` module compiled in.

To rebuild only after editing `rmp`:

```bash
cd build && make -j$(nproc) rmp openroad
```

---

## The Resynthesis Commands

All three commands operate on a **placed, post-CTS** design that is already
loaded (`read_def` + `read_sdc` + `set_wire_rc`). They restructure the
worst-slack logic cones in place and leave the surrounding layout untouched.

### `resynth_mcts` — Monte Carlo Tree Search (recommended)

```tcl
resynth_mcts \
  -corner       slow \   # timing corner
  -seed         3500 \   # RNG seed (reproducibility)
  -iters        1500 \   # search iterations
  -max_depth    6 \      # max recipe length
  -ucb_constant 0.3 \    # UCB1 exploration constant (C0)
  -percentage   30 \     # keep worst N% of violating endpoints
  -wns_pct      10 \     # WNS-regression guard tolerance
  -eval_mode    tiered   # tiered (kCheap loop + kFull top-5) | full
```

### `resynth_sa` — Simulated Annealing with tiered eval + top-K

```tcl
resynth_sa \
  -corner     slow \
  -seed       3500 \
  -iters      1500 \
  -percentage 30 \
  -eval_mode  tiered \
  -final_topk 5          # # of distinct finalists re-scored with the full oracle
```

### `resynth_annealing` — upstream blind-oracle baseline (for comparison)

```tcl
resynth_annealing \
  -corner     slow \
  -seed       3500 \
  -iters      1500 \
  -percentage 30
```

| Option | Meaning |
|---|---|
| `-percentage` | retain only the worst *N%* of violating endpoints (bounds cone size) |
| `-seed`       | RNG seed for reproducible runs |
| `-iters`      | number of search iterations |
| `-max_depth`  | maximum number of ABC operators in a recipe (MCTS) |
| `-ucb_constant` | UCB1 exploration constant; anneals from this value over the run |
| `-eval_mode`  | `tiered` (fast) or `full` (every candidate gets a full repair) |
| `-final_topk` | (`resynth_sa`) number of finalists re-evaluated with the full oracle |

---

## Quick Start

Minimal script to restructure one design (`gcd`):

```tcl
# ---- libraries (ASAP7) ----
read_liberty <path>/asap7sc7p5t_*_SS_*.lib(.gz)
# (fast corner liberties too, if using define_corners)

read_lef <path>/asap7_tech_1x_*.lef
read_lef <path>/asap7sc7p5t_28_R_1x_*.lef

# ---- post-CTS design ----
read_def src/rmp/test/benchmark_cts/final_cts/gcd/4_cts.def
read_sdc src/rmp/test/benchmark_cts/final_cts/gcd/4_cts.sdc

# ---- estimated wire RC (no SPEF at this stage) ----
set_wire_rc -signal -resistance 1.3340e-1 -capacitance 8.6000e-2

# ---- baseline timing ----
report_worst_slack -max

# ---- restructure the critical cones ----
resynth_mcts -corner slow -seed 3500 -iters 1500 -percentage 30 -eval_mode tiered

# ---- final timing ----
report_worst_slack -max
report_tns
```

Run it with:

```bash
build/bin/openroad -no_init -exit quick_start.tcl
```

---

## Running the Benchmark Sweeps

The harness sweeps a design over `percentage = {10,20,30,40}` and reports
per-stage QoR (Baseline → After-Repair → Restructure-Only → Final).

```bash
cd src/rmp/test/sa_script/gcd        # tiered-SA bench for gcd

SEEDS=3500 PERCENTAGES="10 20 30 40" ITERS=1500 EVAL_MODE=tiered \
  OPENROAD=../../../../build/bin/openroad \
  ./gcd_bench_routed_qor.sh
```

* Tiered (our method) → `sa_script/<design>/` → outputs to `qor_sa_tiered/<design>/`
* Blind baseline      → `sa_orig_script/<design>/` → outputs to `qor_sa_orig/<design>/`
* MCTS                → `mcts_script/<design>/`

**Adding a new design:** drop its post-CTS `4_cts.def` and `4_cts.sdc` into
`test/benchmark_cts/final_cts/<design>/`, then copy a `<design>` script folder
(adjust the design name inside the `.tcl`). Post-CTS DEFs can be generated with
[OpenROAD-flow-scripts](https://github.com/The-OpenROAD-Project/OpenROAD-flow-scripts)
(run through the CTS stage).

> **Large designs:** use `EVAL_MODE=tiered`, a low `PERCENTAGE` (10), and run
> with `set_thread_count` low + few parallel jobs — they are memory-heavy.

---

## Interpreting the Output

Each run prints a per-stage QoR block and a machine-parseable `RESULT` line:

```
RESULT seed=3500 pct=30 iters=1500 ... wns_before_ps=-361.7 wns_after_ps=-279.2
       tns_before_ps=-15436 tns_after_ps=-11569 ... wns_delta_ps=82.5
       tns_delta_ps=3867 ... elapsed_ms=292000
```

Stages logged via `RMP-028x QOR_STAGE`:
`baseline` (S1) → `repair_only` (S2) → `restructure_only` (S3) →
`restructure_repair` (S4, final). `Δ = S4 − S1`, positive = improvement.

---

## Results

On 7nm (ASAP7) post-CTS designs, the physically-aware search improves WNS and
TNS on every benchmark and never falls below OpenROAD's standalone repair flow.
Representative TNS recovery (Final vs Baseline):

| Design | best ΔTNS | best ΔWNS |
|---|---|---|
| Mem-Ctrl | +12,565 ps (+83%) | +92 ps |
| WB-DMA   | +3,520 ps (+58%)  | +56 ps |
| GCD      | +4,796 ps (+31%)  | +101 ps |
| B13      | +6,925 ps (+22%)  | +86 ps |
| JPEG (57k) | +4,202 ps      | +90 ps |

The blind-oracle baseline (`resynth_annealing`) regresses timing and/or fails
global routing on several large designs, where the physically-aware oracle does
not — demonstrating that evaluation fidelity is the decisive factor.

---

## Credits

This work builds on the [OpenROAD Project](https://theopenroadproject.org/) and
its `rmp` module. ABC is used for AIG restructuring and technology mapping.
OpenROAD is distributed under the BSD-3-Clause license; see [`LICENSE`](LICENSE).
