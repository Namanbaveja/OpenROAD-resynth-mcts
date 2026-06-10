# resynth_mcts — Full Flow Report
## Post-CTS Logic Restructuring via Monte Carlo Tree Search

---

## 1. Overview

`resynth_mcts` is an OpenROAD command that restructures combinational logic after Clock Tree Synthesis (CTS) to improve timing — specifically Worst Negative Slack (WNS), Total Negative Slack (TNS), and Violating Endpoints (VEP).

It uses **Monte Carlo Tree Search (MCTS)** to find the best sequence of ABC logic optimization operations to apply to a selected cone of combinational logic, evaluated under real post-placement timing.

---

## 2. Complete Flow — Step by Step

```
TCL: resynth_mcts -percentage 10 -iters 1000 -corner slow
         │
         ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 1: Collect Violating Endpoints                         │
│   GetEndpoints(sta, resizer, slack_threshold)               │
│   → All flip-flop D pins / output ports with slack < 0      │
│   → e.g. UART: 79 violators, GCD: 53 violators             │
└─────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 2: Coverage-Weighted Cone Selection                    │
│   For each endpoint i:                                      │
│     cone_i = backward BFS fanin (stop at register/latch)    │
│     score_i = |slack_i| × mean_coverage(cone_i)            │
│   Keep top percentage_% by score                            │
│   → Selects hub cone shared by most failing paths           │
│   → e.g. pct=10: keeps top 7/79 UART, top 3/53 GCD         │
└─────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 3: Pre-SA repair_timing (committed)                    │
│   rsz::repairSetup(max_passes=10000, max_repairs_per_pass=1)│
│   → Buffer insertion + cell upsizing on all violators       │
│   → Sets the MCTS comparison baseline                       │
│   → e.g. GCD: VEP 53→51, TNS improved by ~1300 ps          │
└─────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 4: Build ABC Library                                   │
│   AbcLibraryFactory → Abc_SclDeriveGenlib                   │
│   → Real Liberty NLDM timing tables → GENLIB format        │
│   → Gate delays use actual rise/fall times from .lib        │
└─────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 5: MCTS Search (RunStrategy)                           │
│   1000 iterations, max_depth=6, UCB constant=0.3            │
│   → See Section 3 for MCTS details                          │
│   → Each iteration: full evaluation pipeline                │
└─────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 6: Apply Best Op Sequence (Final Apply)                │
│   RunGia(sta, cone_vertices, abc_library, best_ops)         │
│   → DPL: legalizeNewCellsIncremental                        │
│   → Parasitic update: IncrementalParasiticsGuard            │
│   → QOR snapshot: restructure_only stage                    │
└─────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 7: Final repair_timing (committed)                     │
│   rsz::repairSetup on post-restructure design               │
│   → Closes remaining timing headroom opened by ABC          │
│   → QOR snapshot: restructure_repair stage (final result)   │
└─────────────────────────────────────────────────────────────┘
         │
         ▼
   Report: WNS / TNS / VEP (4 QOR stages)
```

---

## 3. MCTS Algorithm — Detailed

### 3.1 What MCTS Optimizes

MCTS searches over **sequences of GIA (Generalized And-Inverter) operations** — ABC's logic optimization primitives. There are 8 available operations:

| Op | Description |
|----|-------------|
| rewrite | Local DAG rewriting |
| rewrite -z | Zero-cost rewriting |
| refactor | Cone refactoring |
| refactor -z | Zero-cost refactoring |
| balance | Depth-balanced re-structuring |
| resub | Resubstitution |
| resub -z | Zero-cost resubstitution |
| compress | Iterative rewrite+refactor |

A **sequence** (prefix) of up to `max_depth=6` operations defines one candidate restructuring. MCTS finds the best sequence.

### 3.2 Tree Structure

```
Root (empty sequence)
├── op0 (rewrite)
│   ├── op0,op0
│   ├── op0,op1
│   │   ├── op0,op1,op0
│   │   └── ...
│   └── op0,op7
├── op1 (rewrite -z)
│   └── ...
└── op7 (compress)
    └── ...
```

Each tree node = one unique op prefix.  
Tree size (max_depth=6, 8 ops) = 8+64+512+4096+32768+262144 ≈ **300k nodes**.

### 3.3 Per-Iteration Pipeline (kFull — Real Evaluation)

```
MCTS selects a node (UCB)
        │
        ▼
Apply prefix ops to cone AIG
        │
        ▼
Abc_NtkMap: AIG → tech-mapped netlist (GENLIB delay model)
        │
        ▼
InsertMappedAbcNetwork: replace original cells with ABC output
        │
        ▼
DPL: legalizeNewCellsIncremental (place new ABC cells)
        │
        ▼
IncrementalParasiticsGuard::update() (Steiner RC for new nets)
        │
        ▼
STA: full timing analysis on mixed parasitics
        │
        ▼
rsz::repairSetup (repair_timing on this candidate)
        │
        ▼
GetTimingMetrics → composite_score = 0.20×WNS + 0.80×TNS/VEP
        │
        ▼
undoEco: roll back all changes (ODB journal)
        │
        ▼
UCB backpropagation: update visits + scores up the tree
```

**Cost**: ~2s per iteration on UART (1000 iters = ~33 min per run).

### 3.4 UCB1 Node Selection

UCB score for child node `c` with parent `p`:

```
UCB(c) = (mean_score(c) - baseline) / |baseline|   ← exploit
        + C × √(ln(visits(p)) / visits(c))          ← explore
```

Where:
- `mean_score = total_score / visits` (mean over all evaluations through this node)
- `baseline` = composite score of current (pre-MCTS) design
- `C` = UCB constant (default 0.3), annealed from C to C/6 over the run
- C annealing: more exploration early, more exploitation late

### 3.5 Restart Mechanism

When no improvement for `kMctsStallLimit = iterations/3` consecutive iterations:
- Reset the tree to a fresh root
- Keep the global best solution
- Forces re-exploration of different op prefixes
- ~3 restarts per 1000-iteration run

### 3.6 Composite Score (Reward Function)

```
composite = 0.20 × WNS_seconds + 0.80 × (TNS / violating_count)
```

Higher is better (less negative). TNS-weighted because:
- WNS alone can be gamed by fixing one path
- TNS/VEP measures overall health of the violation distribution
- `TnsPerViolator()` = average depth per violating endpoint

---

## 4. Cone Selection — Coverage-Weighted

### 4.1 Backward BFS Fanin Traversal

For each violating endpoint `e`:

```
cone_insts(e) = {}
queue = {e}
visited = {e}

while queue not empty:
    v = dequeue()
    add v's dbInst to cone_insts

    for each incoming timing edge to v:
        if edge.role == regClkToQ → STOP (register boundary)
        if edge.role == latchDtoQ → STOP (latch boundary)
        if edge.role == timingCheck → SKIP (setup/hold arc)
        else → enqueue fanin vertex (combinational logic only)
```

This collects all **combinational logic cells** on all timing paths that feed endpoint `e`, stopping at sequential element outputs.

### 4.2 Coverage Metric

```
cell_coverage[cell] = number of endpoint cones containing this cell
```

A cell with `cell_coverage = 10` appears in the fanin cones of 10 different violating endpoints. Restructuring it via ABC can improve all 10 paths simultaneously.

### 4.3 Endpoint Scoring

```
score(endpoint_i) = |slack_i| × mean_coverage(cone_i)

where:
  |slack_i|         = magnitude of timing violation (seconds)
  mean_coverage     = average cell_coverage over all cells in cone_i
```

This scores endpoints higher when:
1. Their violation is large (deep timing failure)
2. Their cone overlaps with many other failing paths (high leverage)

### 4.4 Selection

Sort all violating endpoints by score (descending), keep top `percentage_%`:

```
pct=10, 79 UART violators → keep top 7
pct=20, 79 UART violators → keep top 15
pct=10, 53 GCD violators  → keep top 3
```

The union of these endpoints' fanin cones becomes the ABC restructuring target.

---

## 5. ABC Delay Budget Calculation

### 5.1 Problem with Original Approach

The original OpenROAD code passed `DelayTarget = 1.0` to ABC with a simple GENLIB where every gate has delay = 1.0. Any cone with 2+ gate levels has path delay > 1.0, so ABC immediately gives up on timing and falls back to **minimum-depth mapping only**. The timing target was silently ignored.

### 5.2 Our Fix: Real Delay Budget

**Step A — Tightest required time at cone outputs:**
```
min_required = min over all candidate_vertices of:
    sta->required(v, RiseFallBoth, MaxDelay)
```
This is the tightest deadline any cone output must meet.

**Step B — Latest arrival at cone inputs:**
```
max_arrival = max over all primary_inputs of cut of:
    sta->arrival(driver_vertex, RiseFallBoth, MaxDelay)
```
The cone cannot process until all inputs have arrived; the latest input is the binding constraint.

**Step C — Delay budget:**
```
delay_budget_seconds = min_required - max(max_arrival, 0)
```

This is the **actual maximum delay the cone's logic may take** while still meeting timing.

**Step D — Convert to GENLIB units:**
```
delay_budget_genlib = delay_budget_seconds / liberty_time_scale
```

For ASAP7 (`time_unit = 1ps`): `liberty_time_scale = 1e-12`

So if `delay_budget_seconds = 47.3e-12 s = 47.3 ps`:
```
delay_budget_genlib = 47.3e-12 / 1e-12 = 47.34 (GENLIB units)
```

**Step E — Real GENLIB from Liberty:**

`Abc_SclDeriveGenlib` extracts actual gate delays from Liberty NLDM tables:
```
inv_delay_max    = 18.11 ps  (typical ASAP7 inverter)
min_2input_delay = 25.14 ps  (NAND2/NOR2)
```

ABC's mapper then uses these real delays to decide whether the timing target is achievable.

### 5.3 Achievability Check

```
max_gate_levels_in_budget = floor(delay_budget_genlib / inv_delay_max)

If aig_depth > max_gate_levels_in_budget:
    → UNACHIEVABLE: ABC falls back to depth-only mapping
    → Common in post-CTS (routing delay dominates)

If aig_depth ≤ max_gate_levels_in_budget:
    → ACHIEVABLE: ABC uses timing-driven mapping
    → Rare post-CTS, more common pre-place
```

**Example from UART logs:**
```
budget_genlib   = 47.34
inv_delay_max   = 18.11
→ max levels    = floor(47.34/18.11) = 2

aig_depth = 9   →  9 >> 2  →  UNACHIEVABLE

Interpretation: routing wire delay consumes 195+ ps of the 270 ps clock,
leaving only 47 ps for logic. The 9 AIG levels need ~163 ps. ABC cannot help.
```

### 5.4 QOR Stage Logging

The code logs 4 QOR stages for every run:

| Stage | What it measures |
|-------|-----------------|
| `baseline` | Post-CTS design, original routing parasitics |
| `repair_only` | After pre-SA repair_timing only (no restructuring) |
| `restructure_only` | After ABC restructuring + DPL (before repair_timing) |
| `restructure_repair` | Final: ABC + DPL + repair_timing |

---

## 6. Key Parameters

| Parameter | Default | Effect |
|-----------|---------|--------|
| `-percentage` | 100 | % of violating endpoints selected by coverage score |
| `-iters` | varies | MCTS iterations per run |
| `-max_depth` | 6 | Max op sequence length |
| `-ucb_constant` | 0.3 | UCB exploration vs exploitation balance |
| `-seed` | 42 | Random seed for MCTS randomization |
| `-corner` | slow | Timing corner for STA analysis |

---

## 7. Known Limitations

### 7.1 Post-CTS Stage is Routing-Delay Dominated

At post-CTS, routing wire delay typically accounts for 70-85% of path delay. ABC reduces **logic gate delay** (10-20% of total). Even perfect ABC restructuring achieves only marginal improvement.

**Evidence**: In all UART/GCD runs, `UNACHIEVABLE` appears for every cone — the budget allows 1-2 gate levels but cones have 8-14 AIG depth.

### 7.2 Mixed Parasitics Reward Signal

During MCTS evaluation, the reward uses:
- **SPEF** (real routed delays) for existing nets
- **Steiner-tree estimates** (optimistic, shorter than real routes) for new ABC cells

New cells appear artificially better. The MCTS optimizes against an optimistic signal.

### 7.3 repair_timing Dominates

On well-optimized post-route designs, `repair_timing` alone provides 80-90% of the improvement. ABC restructuring adds marginal structural value.

**Measurement** (GCD relaxed clock):
- repair_timing alone: +1152 ps TNS, VEP -2
- MCTS + repair_timing: +1291 ps TNS, VEP -2
- MCTS structural contribution: +139 ps (11% of total)

### 7.4 Right Stage for ABC

ABC is most effective **pre-placement**, where:
- No routing constraints limit restructuring
- Cell placement can be optimized around new logic structure
- Logic delay dominates (not wire delay)
- Budget is achievable (few gate levels needed)

---

## 8. Results Summary (Benchmark)

Running on 8 designs × 3 percentages (10/15/20) × seed=2345 × 1000 iters.

Results stored in:
```
qor_mcts/{design}/mcts_seed2345_pct{10,15,20}_iters1000.log   ← detailed
qor_mcts/{design}/mcts_*_results_*.tsv                         ← aggregated TSV
qor_sa/{design}/sa_*_results_*.tsv                             ← SA comparison
```

Final comparison table will be generated from TSV files once the benchmark run completes.
