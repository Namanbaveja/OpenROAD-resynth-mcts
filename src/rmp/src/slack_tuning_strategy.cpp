// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors
//
// =============================================================================
// IMPROVED VERSION — Changes from upstream:
//   1. Composite SA score: WNS + TNS + violation count + DPL failure penalty
//      (instead of WNS-only) — drives holistic timing optimization
//   2. Timing-weighted centroid hint (slack-criticality biased) instead of
//      simple geometric average
//   3. Grid-snapped placement hints — reduces wasted DPL search steps
//   4. Iterative multi-cell placement — handles inter-cell dependencies
//   5. Non-adjacent SA swap — explores permutation space more aggressively
//   6. Spatial filter for coord_snapshot — only snapshots cells near new
//      cells rather than entire design
//   7. DEF snapshots gated behind a debug flag (no static counter abuse)
//   8. DPL failure penalty integrated into SA score so failed placements
//      are correctly avoided
//
// HEADER CHANGES NEEDED (slack_tuning_strategy.h):
//   - Add `TimingMetrics` struct (definition below in anonymous namespace —
//     move to header if needed publicly)
//   - Add `ScoreWeights` struct to SolutionSlack
//   - Add `timing_metrics_` and `composite_score_` members to SolutionSlack
//   - SA comparison code (in RunStrategy) must compare composite_score_
//     instead of worst_slack_
// =============================================================================

#include "slack_tuning_strategy.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/random/distributions.h"
#include "cut/abc_library_factory.h"
#include "dpl/Opendp.h"
#include "est/EstimateParasitics.h"
#include "gia.h"
#include "odb/db.h"
#include "odb/defout.h"
#include "rsz/Resizer.hh"
#include "sta/Delay.hh"
#include "sta/Graph.hh"
#include "sta/MinMax.hh"
#include "sta/TimingRole.hh"
#include "sta/Transition.hh"
#include "utils.h"
#include "utl/Logger.h"
#include "utl/unique_name.h"

namespace rmp {

// -----------------------------------------------------------------------------
// Tunable constants
// -----------------------------------------------------------------------------

// IMPORTANT: kSearchResizeIters and kFinalResizeIters MUST be equal so the SA
// evaluates solutions under the same resize budget as the final apply.
// Different values create an eval-apply mismatch (different cell counts/sizes
// → different timing → unreliable SA selection).
constexpr size_t kSearchResizeIters = 100;
constexpr size_t kFinalResizeIters  = 100;

// Max displacement (in sites/rows) for incremental DPL during SA evaluation.
// Kept small so diamond search stays fast.
constexpr int kDplMaxDispX = 20;  // sites
constexpr int kDplMaxDispY = 5;   // rows

// Number of placement passes for multi-cell batches. After the first pass,
// previously-placed cells from the same batch can be used as anchors for
// remaining cells, so a second pass usually improves placement quality.
constexpr int kPlacementIterations = 2;

// rsz::repair_setup budget when run inside an SA evaluation.
// ABC produces a load-blind netlist (no buffer trees, min-area cells on
// fanout-heavy nets); raw STA on that output looks 5-15× worse than baseline,
// so SA rejects every candidate even when the underlying structure is
// improvable.  Running a bounded repair_setup pass inside Evaluate scores
// each candidate under "what repair_timing CAN achieve given this structure"
// rather than "what raw ABC output looks like".
//
// 0  → disabled (revert to pre-repair behaviour)
// 1-2 → fast, useful for SA where each iter must be cheap
// 5   → moderate; per-iter cost up but candidate scoring much more faithful
// 10+ → closer to standalone repair_setup behaviour, much slower
constexpr int kSaRepairSetupMaxPasses = 10000;


// ---- COST FUNCTION v3: TNS-centric (2026-05-28) ------------------------------
//
// Motivation: with coverage-weighted endpoint selection the cone now covers
// hub cells shared across many violating paths.  A TNS-dominant objective
// rewards candidates that improve many paths simultaneously (which is exactly
// what the coverage-weighted cone makes possible), rather than optimising the
// single worst path at the expense of the rest.
//
// New formula:
//   score = TNS  (raw total negative slack, in seconds)
//
//   Pure TNS objective: directly maximise the sum of all endpoint slacks.
//   Simpler than TnsPerViolator — no division by violating_count.
//   Higher (less negative) TNS = better candidate.
//
// DPL penalty is retained (unchanged) to reject unplaced-cell solutions.
constexpr float kWeightWns         = 0.00f; // disabled — pure TNS objective
constexpr float kWeightTns         = 1.00f; // weight on raw TNS (see score below)

// kKwnsK: still used in GetTimingMetrics to size the KWNS heap (structural
// computation kept for logging/diagnostics even though KWNS is no longer
// part of the composite score).
constexpr int kKwnsK = 5;

// ---- OLD COST FUNCTION (RICHER FORMULA 2026-05-18) — kept for reference ----
// constexpr int   kKwnsK             = 5;     // number of worst endpoints in KWNS
// constexpr float kWeightKwns        = 0.50f; // primary signal; replaces raw WNS
// constexpr float kWeightTns_old     = 0.30f; // weighted by per-violator depth
// constexpr float kWeightViolations  = 0.15f; // re-enabled; rewards fixed endpoints
// constexpr float kWeightRmsViol     = 0.05f; // distribution shape, small voice
// constexpr float kWeightWns_old     = 0.00f; // WNS was disabled; KWNS subsumed it
//
// Old score:
//   score = kWeightKwns * KwnsMean()
//         + kWeightTns_old * TnsPerViolator()
//         - kWeightViolations * violating_count * kPerViolationCostSec
//         - kWeightRmsViol * RmsViolatorSlack()
//
// Why replaced:
//   1. WNS weight was 0 — direct WNS not in the objective at all.
//   2. KWNS_mean only approximated WNS through K=5 worst endpoints.
//   3. Violation penalty caused TNS regression on SASC_TOP (restructuring
//      traded TNS for WNS improvement and score went up anyway).
//   4. RMS term added noise without measurable QoR benefit.
// Per-failure DPL penalty (in seconds-equivalent). One unplaced cell is worth
// 2ns of timing degradation in the composite score — large enough to
// strongly discourage solutions that overflow the placement bins.
constexpr double kDplPenaltyPerFailure = 2.0e-9;

// kPerViolationCostSec retained for reference (used in old formula).
// constexpr double kPerViolationCostSec = 2.0e-11;

using utl::RMP;

// -----------------------------------------------------------------------------
// Internal helpers (anonymous namespace)
// -----------------------------------------------------------------------------
namespace {

// Compute WNS, TNS, and violation count in a single endpoint traversal,
// ALL scoped to the specified corner.
//
// The bug this fixes: the old code used sta->slack(v, MinMax::max()) for the
// violation count — that overload has NO corner parameter and returns the worst
// slack across ALL corners. On multi-corner designs (e.g. fast + slow) this
// silently counted slow-corner violations even when SA was asked to evaluate
// the fast corner. Result: WNS showed +37 ps (fast corner — timing clean) but
// violations showed 93 (slow corner), producing an inconsistent composite score
// that SA could never improve because it only optimised the fast corner.
//
// Fix: build a single-element SceneSeq from the specified corner and pass it to
// the per-corner slack(vertex, rf, scenes, minmax) overload so that violations,
// WNS, and TNS all measure the same corner.
std::pair<TimingMetrics, sta::Vertex*> GetTimingMetrics(sta::dbSta* sta,
                                                        sta::Scene* corner)
{
  TimingMetrics      metrics;
  sta::Vertex*       worst_vertex = nullptr;
  const sta::SceneSeq corner_seq{corner};

  // WNS — also produces the worst vertex for downstream required-time query.
  sta->worstSlack(corner, sta::MinMax::max(), metrics.wns, worst_vertex);

  // TNS — sum of all negative endpoint slacks for THIS corner.
  metrics.tns = sta->totalNegativeSlack(corner, sta::MinMax::max());

  // Single traversal accumulates: violator count, total endpoints, squared
  // violator-slack sum, and the top-K worst endpoint slacks (KWNS sum).
  //
  // The "top-K" tracking uses a fixed-size max-heap of size kKwnsK, holding
  // the K most-negative slacks seen so far.  We push every slack and pop
  // the LEAST-negative (the heap root) when size exceeds K.  At the end the
  // heap contains the K most-negative slacks; we sum them.
  std::priority_queue<sta::Slack> least_negative_first;  // max-heap on slack
  for (sta::Vertex* v : sta->endpoints()) {
    ++metrics.total_endpoints;
    const sta::Slack v_slack = sta->slack(v,
                                          sta::RiseFallBoth::riseFall(),
                                          corner_seq,
                                          sta::MinMax::max());
    if (v_slack < 0.0f) {
      ++metrics.violating_count;
      const double s = static_cast<double>(v_slack);
      metrics.squared_violator_sum += s * s;
    }
    // KWNS: track K most-negative slacks across ALL endpoints (not just
    // violators, in case there are <K violators on a clean design).
    least_negative_first.push(v_slack);
    if (static_cast<int>(least_negative_first.size()) > kKwnsK) {
      least_negative_first.pop();
    }
  }

  // Drain the heap into kwns_sum / kwns_endpoints_counted.
  metrics.kwns_endpoints_counted
      = static_cast<int>(least_negative_first.size());
  sta::Slack kwns_accum = 0.0;
  while (!least_negative_first.empty()) {
    kwns_accum = kwns_accum + least_negative_first.top();
    least_negative_first.pop();
  }
  metrics.kwns_sum = kwns_accum;

  return {metrics, worst_vertex};
}

// Composite SA score: combines five signals into a single scalar where
// HIGHER is BETTER (less negative).  All terms are in seconds-equivalent
// so Metropolis acceptance can compare deltas against a temperature in
// the same units.
//
//   1. KWNS_mean   — average of worst K endpoint slacks (decouples score
//                    from a single hostage endpoint).
//   2. TNS / N_viol — TNS averaged over violators (real signal weight,
//                    unlike the previous TNS / N_total dilution).
//   3. -viol·c_viol — direct credit for fixing endpoints.
//   4. -RMS_viol   — penalises spread-out violation distributions.
double CompositeScore(const TimingMetrics& m,
                      int                  dpl_failures,
                      int                  total_new_cells)
{
  // Pure TNS: raw total negative slack in seconds (≤ 0).
  // Higher (less negative) = better candidate.
  double score = static_cast<double>(m.tns);

  // OLD formula (kept for reference — commented out 2026-05-24):
  // const double rms_term = m.RmsViolatorSlack();
  // double score = kWeightWns_old        * static_cast<double>(m.wns)
  //              + kWeightKwns           * m.KwnsMean()
  //              + kWeightTns_old        * m.TnsPerViolator()
  //              - kWeightViolations     * static_cast<double>(m.violating_count)
  //                                      * kPerViolationCostSec
  //              - kWeightRmsViol        * rms_term;

  // DPL failure penalty: each unplaced cell is timing-equivalent harm.
  // Without this, SA can pick solutions that "look good in timing" but
  // failed to actually place — those numbers are unreliable.
  if (dpl_failures > 0 && total_new_cells > 0) {
    const double failure_ratio
        = static_cast<double>(dpl_failures) / total_new_cells;
    score -= failure_ratio * kDplPenaltyPerFailure;
  }

  return score;
}

// Backward BFS from one endpoint collecting every odb::dbInst* in its
// combinational fanin cone.  Stops at register/latch clock-to-Q edges and
// at top-level ports (no owning instance).  Timing-check edges (setup/hold
// arcs into FF data pins) are skipped so we don't accidentally cross into
// the FF clock network.
std::unordered_set<odb::dbInst*> GetFaninConeInsts(sta::Vertex*  endpoint,
                                                    sta::dbSta*   sta)
{
  sta::dbNetwork* network = sta->getDbNetwork();
  sta::Graph*     graph   = sta->graph();

  std::unordered_set<odb::dbInst*>  cone_insts;
  std::unordered_set<sta::Vertex*>  visited;
  std::queue<sta::Vertex*>          bfs_q;

  bfs_q.push(endpoint);
  visited.insert(endpoint);

  while (!bfs_q.empty()) {
    sta::Vertex* v = bfs_q.front();
    bfs_q.pop();

    // Map vertex pin → dbInst (nullptr for top-level ports).
    sta::Pin*      pin  = v->pin();
    sta::Instance* inst = network->instance(pin);
    if (inst != nullptr) {
      odb::dbInst* db_inst = network->staToDb(inst);
      if (db_inst != nullptr) {
        cone_insts.insert(db_inst);
      }
    }

    // Walk backward through all fanin edges.
    sta::VertexInEdgeIterator edge_iter(v, graph);
    while (edge_iter.hasNext()) {
      sta::Edge*              edge = edge_iter.next();
      const sta::TimingRole*  role = edge->role();

      // Stop at sequential cell boundaries (FF/latch clock-to-Q).
      if (role->genericRole() == sta::TimingRole::regClkToQ()
          || role->genericRole() == sta::TimingRole::latchDtoQ()) {
        continue;
      }
      // Skip setup/hold check arcs — these cross to the clock network.
      if (role->isTimingCheck()) {
        continue;
      }

      sta::Vertex* fanin_v = edge->from(graph);
      // Only enqueue if not yet visited.
      if (visited.insert(fanin_v).second) {
        // Stop at top-level ports (no owning instance).
        if (network->instance(fanin_v->pin()) == nullptr) {
          continue;
        }
        bfs_q.push(fanin_v);
      }
    }
  }

  return cone_insts;
}

// Snap a coordinate to the placement grid (site width × row height).
// Off-grid hints force DPL diamond search to walk extra steps; snapping
// gives DPL a valid starting cell of the grid immediately.
void SnapToPlacementGrid(odb::dbBlock* block, int& x, int& y)
{
  const auto& rows = block->getRows();
  if (rows.empty()) {
    return;
  }
  odb::dbRow* row     = *rows.begin();
  odb::dbSite* site    = row->getSite();
  if (!site) {
    return;
  }
  const int site_w = site->getWidth();
  const int row_h  = site->getHeight();

  if (site_w > 0) {
    x = (x / site_w) * site_w;
  }
  if (row_h > 0) {
    y = (y / row_h) * row_h;
  }
}

// Set inst's location to a slack-weighted centroid of its connected pins.
// Critical (low-slack) nets pull the cell harder than non-critical ones,
// biasing placement toward timing-important neighbors.
//
// `consider_unplaced_batch` controls whether new cells from the same batch
// that have already been placed in this iteration count as anchors. Set
// to true on second placement pass for inter-cell convergence.
void SetWeightedCentroidHint(odb::dbInst*                   inst,
                             odb::dbBlock*                  block,
                             sta::dbSta*                    sta,
                             sta::Scene*                    corner,
                             const std::set<odb::dbInst*>&  placed_in_batch)
{
  double sum_x = 0.0, sum_y = 0.0, total_weight = 0.0;
  auto*  network = sta->getDbNetwork();

  for (odb::dbITerm* iterm : inst->getITerms()) {
    odb::dbNet* net = iterm->getNet();
    if (!net) {
      continue;
    }

    // Slack-based weight: critical nets pull harder.
    // Use absolute slack so that a +0.1ns net contributes less than a -1ns net.
    double weight = 1.0;
    if (sta::Net* sta_net = network->dbToSta(net)) {
      const sta::Slack net_slack = sta->slack(sta_net, sta::MinMax::max());
      // Map slack → weight: w = 1 / (|slack| + epsilon)
      // The +1ps epsilon prevents division by zero on perfectly-met paths.
      weight = 1.0 / (std::abs(static_cast<double>(net_slack)) + 1e-12);
    }

    // Sum positions of placed neighbors (existing cells + already-placed
    // cells from the same batch when consider_unplaced_batch=true).
    for (odb::dbITerm* other : net->getITerms()) {
      odb::dbInst* other_inst = other->getInst();
      if (other_inst == inst) {
        continue;
      }
      const bool is_placed_existing
          = other_inst->getPlacementStatus().isPlaced();
      const bool is_placed_in_batch
          = placed_in_batch.count(other_inst) > 0;
      if (!is_placed_existing && !is_placed_in_batch) {
        continue;
      }
      int x = 0, y = 0;
      if (other->getAvgXY(&x, &y)) {
        sum_x += weight * x;
        sum_y += weight * y;
        total_weight += weight;
      }
    }

    // Block terminals (chip I/O) are always "placed".
    for (odb::dbBTerm* bterm : net->getBTerms()) {
      int x = 0, y = 0;
      if (bterm->getFirstPinLocation(x, y)) {
        sum_x += weight * x;
        sum_y += weight * y;
        total_weight += weight;
      }
    }
  }

  if (total_weight > 0.0) {
    int hx = static_cast<int>(std::round(sum_x / total_weight));
    int hy = static_cast<int>(std::round(sum_y / total_weight));
    SnapToPlacementGrid(block, hx, hy);
    inst->setLocation(hx, hy);
  }
}


// Helper for writing snapshot DEFs with consistent naming.
void WriteSnapshotDef(odb::dbBlock*       block,
                      const std::string& tag,
                      utl::Logger*        logger,
                      int                 msg_id,
                      const std::string& description)
{
  const std::string fname
      = std::string(block->getName()) + "_" + tag + ".def";
  odb::DefOut writer(logger);
  writer.writeBlock(block, fname.c_str());
  logger->info(RMP, msg_id, "{}: {}", description, fname);
}

}  // anonymous namespace

// -----------------------------------------------------------------------------
// SolutionSlack — solution representation + SA neighbor + evaluation
// -----------------------------------------------------------------------------

std::string SolutionSlack::toString() const
{
  if (solution_.empty()) {
    return "[]";
  }

  std::ostringstream os;
  os << '[' << std::to_string(solution_.front().id);
  for (int i = 1; i < solution_.size(); i++) {
    os << ", " << std::to_string(solution_[i].id);
  }
  os << "], composite=" << composite_score_;
  if (worst_slack_) {
    os << " WNS=" << *worst_slack_;
  } else {
    os << " WNS=not computed";
  }
  return os.str();
}

SolutionSlack::ResultOps SolutionSlack::RandomNeighbor(
    const SolutionSlack::ResultOps& all_ops,
    utl::Logger*                    logger,
    std::mt19937&                   random) const
{
  SolutionSlack::ResultOps sol = solution_;

  enum class Move : uint8_t
  {
    kAdd,
    kRemove,
    kSwap,
    kCount
  };

  Move move = Move::kAdd;
  if (sol.size() > 1) {
    const auto count = static_cast<std::underlying_type_t<Move>>(Move::kCount);
    move             = Move(absl::Uniform<int>(random, 0, count));
  }

  switch (move) {
    case Move::kAdd: {
      debugPrint(logger, RMP, "slack_tunning", 2, "Adding a new GIA operation");
      const size_t i = absl::Uniform<size_t>(random, 0, sol.size() + 1);
      const size_t j = absl::Uniform<size_t>(random, 0, all_ops.size());
      sol.insert(sol.begin() + i, all_ops[j]);
      break;
    }
    case Move::kRemove: {
      debugPrint(logger, RMP, "slack_tunning", 2, "Removing a GIA operation");
      const size_t i = absl::Uniform<size_t>(random, 0, sol.size());
      sol.erase(sol.begin() + i);
      break;
    }
    case Move::kSwap: {
      // IMPROVEMENT: arbitrary swap (was adjacent-only) — explores the
      // permutation space far more aggressively, which matters because op
      // ordering significantly affects ABC's logic optimization quality.
      debugPrint(logger, RMP, "slack_tunning", 2, "Swapping GIA operations");
      assert(sol.size() > 1);
      const size_t i = absl::Uniform<size_t>(random, 0, sol.size());
      size_t       j = absl::Uniform<size_t>(random, 0, sol.size());
      while (j == i) {
        j = absl::Uniform<size_t>(random, 0, sol.size());
      }
      std::swap(sol[i], sol[j]);
      break;
    }
    case Move::kCount:
      break;  // unreachable
  }

  return sol;
}

void SolutionSlack::EvaluateCurrentState(sta::dbSta* sta, sta::Scene* corner)
{
  // Read timing from the current (untouched) design state. No ECO journal,
  // no RunGia, no DPL — this is what SA must compare candidate recipes
  // against to decide whether applying any of them is worthwhile.
  auto [metrics, _] = GetTimingMetrics(sta, corner);
  timing_metrics_   = metrics;
  composite_score_  = CompositeScore(metrics,
                                     /*dpl_failures=*/0,
                                     /*total_new_cells=*/0);
  worst_slack_      = metrics.wns;
}

std::pair<sta::Slack, sta::Delay> SolutionSlack::Evaluate(
    const std::vector<sta::Vertex*>& candidate_vertices,
    cut::AbcLibrary&                 abc_library,
    sta::Scene*                      corner,
    sta::dbSta*                      sta,
    utl::UniqueName&                 name_generator,
    rsz::Resizer*                    resizer,
    utl::Logger*                     logger,
    EvalTier                         tier)
{
  auto* block = sta->db()->getChip()->getBlock();

  // ----- parasitics guard (live during RunGia callbacks) -----
  auto* ep = resizer->getEstimateParasitics();
  const bool use_incremental
      = (ep != nullptr && ep->wireSignalCapacitance(corner) > 0.0);
  std::unique_ptr<est::IncrementalParasiticsGuard> guard;
  if (use_incremental) {
    guard = std::make_unique<est::IncrementalParasiticsGuard>(ep);
    debugPrint(
        logger, RMP, "sa_eval", 1, "parasitics: incremental guard active");
  } else {
    debugPrint(logger,
               RMP,
               "sa_eval",
               1,
               ep ? "parasitics: guard SKIPPED — wire RC not set"
                  : "parasitics: guard SKIPPED — ep unavailable");
  }

  odb::dbDatabase::beginEco(block);

  // Snapshot DEFs are gated behind the sa_snap debug flag rather than a
  // static counter — cleaner and doesn't leak state across calls.
  const bool do_snap
      = logger->debugCheck(RMP, "sa_snap", /*level=*/1);

  if (do_snap) {
    WriteSnapshotDef(block,
                     "eval_1_begin_eco",
                     logger,
                     233,
                     "SA snapshot [1/3 begin_eco — original ODB]");
  }

  RunGia(sta,
         candidate_vertices,
         abc_library,
         solution_,
         kSearchResizeIters,
         name_generator,
         logger);

  if (do_snap) {
    WriteSnapshotDef(
        block,
        "eval_2_after_RunGia",
        logger,
        234,
        "SA snapshot [2/3 after_RunGia — cone removed, new cells UNPLACED]");
  }

  odb::dbDatabase::endEco(block);

  // ----- step 1: legalize newly inserted cells -----
  std::vector<odb::dbInst*>                        new_cells;
  std::vector<std::pair<odb::dbInst*, odb::Point>> coord_snapshot;
  int                                              eval_unplaced = 0;
  int                                              dpl_failures  = 0;

  if (auto* opendp = resizer->getOpendp()) {
    // Phase A: collect new cells (UNPLACED status after RunGia).
    for (odb::dbInst* inst : block->getInsts()) {
      if (!inst->getPlacementStatus().isPlaced()) {
        ++eval_unplaced;
        new_cells.push_back(inst);
      }
    }

    if (eval_unplaced > 0) {
      // IMPROVEMENT: iterative placement.
      // First pass: anchor only on existing cells (placed_in_batch is empty).
      // Second pass: re-hint using cells placed in pass 1 too — this lets
      // mutually-connected new cells converge toward consistent positions
      // instead of being sequence-dependent.
      std::set<odb::dbInst*> placed_in_batch;

      for (int pass = 0; pass < kPlacementIterations; ++pass) {
        for (odb::dbInst* inst : new_cells) {
          SetWeightedCentroidHint(inst, block, sta, corner, placed_in_batch);
        }

        // After the first pass, mark all new cells as anchors for pass 2's
        // re-hint computation. Status is set in the proper place below.
        if (pass == 0) {
          for (odb::dbInst* inst : new_cells) {
            placed_in_batch.insert(inst);
          }
        }
      }

      // Snapshot ALL placed cells before DPL. The spatial-filter optimisation
      // (BBox of new cells + margin) is incorrect: ODB ECO undo restores
      // deleted cone cells at their original positions, and DPL may have
      // moved OTHER existing cells to those same vacated positions (cascade
      // ripUpAndReplace). Cells outside the BBox+margin are not captured, so
      // they remain at the DPL-moved positions after undo, overlapping the
      // ECO-restored originals (DPL-0033 placement overlap bug).
      // For designs of this scale (~1000 cells) the O(N) snapshot is trivial.
      for (odb::dbInst* inst : block->getInsts()) {
        if (!inst->getPlacementStatus().isPlaced()) {
          continue;
        }
        int x = 0, y = 0;
        inst->getLocation(x, y);
        coord_snapshot.emplace_back(inst, odb::Point(x, y));
      }
      debugPrint(logger,
                 RMP,
                 "sa_eval",
                 2,
                 "snapshot: {} placed cells saved",
                 coord_snapshot.size());

      // Phase B: incremental diamond-search DPL — only new cells move
      // (existing cells fixed unless ripUpAndReplace evicts a neighbor).
      debugPrint(logger,
                 RMP,
                 "sa_eval",
                 1,
                 "legalize: incremental DPL for {} new cell(s)",
                 eval_unplaced);
      dpl_failures
          = opendp->legalizeNewCellsIncremental(kDplMaxDispX, kDplMaxDispY);

      if (dpl_failures > 0) {
        debugPrint(logger,
                   RMP,
                   "sa_eval",
                   1,
                   "legalize: {}/{} cell(s) FAILED — applying score penalty",
                   dpl_failures,
                   eval_unplaced);
      }

      // Mark new cells as PLACED so timing analysis treats their locations
      // as valid.
      for (odb::dbInst* inst : new_cells) {
        inst->setPlacementStatus(odb::dbPlacementStatus::PLACED);
      }

      if (do_snap) {
        WriteSnapshotDef(
            block,
            "eval_3_legalized",
            logger,
            235,
            "SA snapshot [3/3 legalized — new cells PLACED]");
      }
    } else {
      debugPrint(logger, RMP, "sa_eval", 1, "legalize: no unplaced cells");
    }
  } else {
    debugPrint(logger, RMP, "sa_eval", 1, "legalize: opendp unavailable");
  }

  // ----- step 2: degenerate-net filter + parasitic update -----
  // Defensive: scan every net attached to a new cell for duplicate pin
  // coordinates, which would crash Flute. Site-level dedup in DPL should
  // prevent this, but the guard keeps SA robust if it ever does happen.
  if (guard) {
    std::set<odb::dbNet*> checked_nets;
    int                   skipped = 0;

    for (odb::dbInst* inst : new_cells) {
      for (odb::dbITerm* iterm : inst->getITerms()) {
        odb::dbNet* net = iterm->getNet();
        if (!net || !checked_nets.insert(net).second) {
          continue;
        }

        std::vector<std::pair<int, int>> coords;
        for (odb::dbITerm* t : net->getITerms()) {
          int x = 0, y = 0;
          if (t->getAvgXY(&x, &y)) {
            coords.emplace_back(x, y);
          }
        }
        for (odb::dbBTerm* bt : net->getBTerms()) {
          int x = 0, y = 0;
          if (bt->getFirstPinLocation(x, y)) {
            coords.emplace_back(x, y);
          }
        }

        std::set<std::pair<int, int>> seen;
        std::pair<int, int>           bad{0, 0};
        bool                          degenerate = false;
        for (const auto& c : coords) {
          if (!seen.insert(c).second) {
            degenerate = true;
            bad        = c;
            break;
          }
        }

        if (degenerate) {
          if (sta::Net* sta_net = ep->getDbNetwork()->dbToSta(net)) {
            ep->removeNetFromParasiticsInvalid(sta_net);
            ++skipped;
            logger->warn(RMP,
                         227,
                         "skipping Steiner estimation for net '{}': "
                         "duplicate pin coordinate ({},{})",
                         net->getName(),
                         bad.first,
                         bad.second);
          }
        }
      }
    }
    debugPrint(logger,
               RMP,
               "sa_eval",
               1,
               "parasitics: updating ({} degenerate net(s) skipped)",
               skipped);
    guard->update();
  }

  // ----- step 2b: in-situ repair_setup on the resynthesized design -----
  // Run a bounded rsz::repairSetup so SA evaluates each candidate under
  // post-repair timing rather than the raw ABC mapping output.  ABC is
  // load-blind: it omits the buffer trees rsz inserts pre-SA and leaves
  // min-area cells on fanout-heavy nets.  Without this step the candidate
  // is graded on a structurally good but unbuffered netlist and rejected
  // every iter even when repair_timing could have closed the slack.
  //
  // Cost: significant.  Each iter adds the time of up to
  // kSaRepairSetupMaxPasses bounded passes over the violating endpoints
  // (typically seconds on UART-class designs).  Set kSaRepairSetupMaxPasses
  // to 0 to disable and recover original SA-eval speed.
  //
  // ECO safety: rsz buffer-insertions and cell upsizing go through ODB
  // (createInst / swapMaster / connect / disconnect), all of which are
  // journalled by the active beginEco transaction and rolled back by
  // undoEco at the end of Evaluate.  rsz's internal IncrementalParasitics
  // guard is reentrant (EstimateParasitics.cpp:1322) — when our outer
  // guard is already active, the inner one is a no-op.
  //
  // Skipped for EvalTier::kCheap: the repair step is the bottleneck
  // (~1.5 s/iter on UART).  Cheap evals produce noisier scores but
  // preserve relative candidate ranking, which is all MCTS needs.
  // repair_setup runs only in the final kFull re-evaluation pass over
  // the top-K candidates so the winner is scored accurately.
  if (tier == EvalTier::kFull && kSaRepairSetupMaxPasses > 0 && resizer != nullptr) {
    try {
      resizer->repairSetup(/*setup_margin=*/0.0,
                           /*repair_tns_end_percent=*/1.0,
                           /*max_passes=*/10000,
                           /*max_iterations=*/-1,
                           /*max_repairs_per_pass=*/1,
                           /*match_cell_footprint=*/false,
                           /*verbose=*/false,
                           /*sequence=*/{},
                           /*phases=*/"",
                           /*skip_pin_swap=*/false,
                           /*skip_gate_cloning=*/false,
                           /*skip_size_down=*/false,
                           /*skip_buffering=*/false,
                           /*skip_buffer_removal=*/false,
                           /*skip_last_gasp=*/false,
                           /*skip_vt_swap=*/true,
                           /*skip_crit_vt_swap=*/true);
      debugPrint(logger,
                 RMP,
                 "sa_eval",
                 1,
                 "repair_setup: completed (max_passes={})",
                 kSaRepairSetupMaxPasses);
    } catch (const std::exception& e) {
      logger->warn(RMP,
                   250,
                   "repair_setup failed in SA eval: {} - candidate "
                   "evaluated without repair benefit",
                   e.what());
    } catch (...) {
      logger->warn(RMP,
                   256,
                   "repair_setup failed in SA eval (unknown exception) - "
                   "candidate evaluated without repair benefit");
    }
  }

  // ----- step 3: composite timing evaluation -----
  // Get all three timing dimensions in a single endpoint traversal.
  auto [metrics, worst_vertex] = GetTimingMetrics(sta, corner);

  // Composite SA score = weighted(WNS + TNS + viol_ratio) − DPL penalty.
  // Higher is better. Pushes SA to optimize all three dimensions while
  // avoiding solutions that overflow the placement bins.
  const double composite = CompositeScore(metrics, dpl_failures, eval_unplaced);

  worst_slack_     = metrics.wns;       // backward compatibility
  timing_metrics_  = metrics;           // full picture
  composite_score_ = composite;         // SA comparison key

  debugPrint(logger,
             RMP,
             "sa_eval",
             1,
             "eval result: composite={:.6e}  {}  ops={}  new_cells={}  "
             "dpl_failures={}",
             composite,
             metrics.toString(),
             solution_.size(),
             eval_unplaced,
             dpl_failures);

  sta::Delay required = sta->required(worst_vertex,
                                      sta::RiseFallBoth::riseFall(),
                                      sta->scenes(),
                                      sta::MinMax::max());

  odb::dbDatabase::undoEco(block);

  // Restore positions of any existing cells displaced by ripUpAndReplace
  // during DPL. undoEco handles netlist changes only — DPL displacement
  // is outside the ECO journal scope, so we restore manually.
  for (const auto& [inst, orig_pt] : coord_snapshot) {
    int cx = 0, cy = 0;
    inst->getLocation(cx, cy);
    if (cx != orig_pt.x() || cy != orig_pt.y()) {
      inst->setLocation(orig_pt.x(), orig_pt.y());
    }
  }

  return {metrics.wns, required};
}

// -----------------------------------------------------------------------------
// SlackTuningStrategy — top-level orchestration
// -----------------------------------------------------------------------------

void SlackTuningStrategy::OptimizeDesign(sta::dbSta*       sta,
                                         utl::UniqueName& name_generator,
                                         rsz::Resizer*    resizer,
                                         utl::Logger*     logger)
{
  sta->ensureGraph();
  sta->ensureLevelized();
  sta->searchPreamble();
  for (auto mode : sta->modes()) {
    sta->ensureClkNetwork(mode);
  }

  // ----- collect violating endpoints -----
  auto candidate_vertices = GetEndpoints(sta, resizer, slack_threshold_);
  if (candidate_vertices.empty()) {
    logger->info(RMP,
                 58,
                 "All endpoints have slack above threshold, nothing to do.");
    return;
  }

  // ----- Greedy cone selection: top k% worst-slack endpoints -----------------
  // Sort violating endpoints by slack ascending (most negative first) and keep
  // the top percentage_%.  Simple greedy selection — no coverage computation.
  if (percentage_ > 0.0f && percentage_ < 100.0f) {
    const size_t total = candidate_vertices.size();
    const size_t keep  = std::min(
        total,
        std::max(size_t(1),
                 static_cast<size_t>(total * static_cast<double>(percentage_) / 100.0)));

    std::partial_sort(
        candidate_vertices.begin(),
        candidate_vertices.begin() + static_cast<std::ptrdiff_t>(keep),
        candidate_vertices.end(),
        [sta](sta::Vertex* a, sta::Vertex* b) {
          return sta->slack(a, sta::MinMax::max())
               < sta->slack(b, sta::MinMax::max());  // most negative first
        });

    candidate_vertices.resize(keep);

    const sta::Slack worst = sta->slack(candidate_vertices.front(), sta::MinMax::max());
    const sta::Slack shal  = sta->slack(candidate_vertices.back(),  sta::MinMax::max());
    logger->info(RMP, 236,
                 "Greedy top-{}% selection: {}/{} endpoints "
                 "(slack range [{:.2f}, {:.2f}] ps)",
                 static_cast<int>(percentage_), keep, total,
                 static_cast<double>(worst) * 1e12,
                 static_cast<double>(shal)  * 1e12);
  }

  // ----- detect link_design-after-read_def mistake -----
  {
    auto* b               = sta->db()->getChip()->getBlock();
    int   total_insts     = 0;
    int   placed_insts    = 0;
    for (odb::dbInst* inst : b->getInsts()) {
      ++total_insts;
      if (inst->getPlacementStatus().isPlaced()) {
        ++placed_insts;
      }
    }
    if (total_insts > 0 && placed_insts == 0) {
      logger->warn(
          RMP,
          237,
          "No cells have placement info at startup ({} instances, 0 placed). "
          "This typically means link_design was called after read_def, "
          "destroying placement coordinates. Non-cone cells will be "
          "legalized from scratch and SA QoR will be unreliable. "
          "Fix: use read_def without link_design for post-PnR flows, or "
          "call read_verilog + link_design BEFORE read_def.",
          total_insts);
    }
  }

  // Initial timing snapshot with SPEF (or pre-estimation) parasitics.
  // MUST be done before estimateWireParasitics() so that spef_wns_baseline_
  // records the actual routed-wire timing, not the estimated model timing.
  // This baseline is used by RunStrategy for the WNS regression guard.
  auto [init_metrics, init_worst] = GetTimingMetrics(sta, corner_);
  spef_wns_baseline_ = init_metrics.wns;

  // QOR_STAGE 1 — baseline: post-CTS/post-route design before any repair or restructuring.
  logger->info(RMP, 280,
               "QOR_STAGE stage=baseline wns_ps={:.4f} tns_ps={:.4f} viols={}",
               static_cast<double>(init_metrics.wns) * 1e12,
               static_cast<double>(init_metrics.tns) * 1e12,
               init_metrics.violating_count);

  // ----- Pre-SA repair_setup: lift baseline to the standalone-repair floor -----
  // The earlier diagnostic confirmed (RMP-0254 in 20260514_203340 log) that
  // running rsz::repairSetup on the untouched design produces exactly the
  // same WNS as a manual `repair_timing` Tcl call (-85.84 ps on UART vs
  // baseline -101.75 ps).  ABC's cone resynthesis then destroys timing by
  // 250-500 ps every candidate, so SA cannot beat the original -101.75 ps
  // even with rsz repair inside Evaluate.
  //
  // The fix: run repair_setup ONCE before SA starts and KEEP the changes.
  // SA's baseline becomes the post-repair state (-85.84 ps).  Even if SA
  // returns no-op (every candidate worse than the new baseline), the design
  // we hand back is the repair_setup output → guaranteed +15.9 ps WNS
  // improvement over running rmp::resynth_annealing with no pre-repair.
  //
  // If repair_setup throws, we undoEco any partial changes and fall back
  // to the original (un-repaired) baseline.
  {
    auto* block = sta->db()->getChip()->getBlock();
    odb::dbDatabase::beginEco(block);

    std::unique_ptr<est::IncrementalParasiticsGuard> pre_guard;
    if (auto* ep = resizer->getEstimateParasitics()) {
      if (ep->wireSignalCapacitance(corner_) > 0.0) {
        pre_guard = std::make_unique<est::IncrementalParasiticsGuard>(ep);
      }
    }

    bool ran = false;
    try {
      resizer->repairSetup(/*setup_margin=*/0.0,
                           /*repair_tns_end_percent=*/1.0,
                           /*max_passes=*/10000,
                           /*max_iterations=*/-1,
                           /*max_repairs_per_pass=*/1,
                           /*match_cell_footprint=*/false,
                           /*verbose=*/false,
                           /*sequence=*/{},
                           /*phases=*/"",
                           /*skip_pin_swap=*/false,
                           /*skip_gate_cloning=*/false,
                           /*skip_size_down=*/false,
                           /*skip_buffering=*/false,
                           /*skip_buffer_removal=*/false,
                           /*skip_last_gasp=*/false,
                           /*skip_vt_swap=*/true,
                           /*skip_crit_vt_swap=*/true);
      ran = true;
    } catch (const std::exception& e) {
      logger->warn(RMP, 253,
                   "Pre-SA repair_setup failed: {} - SA will run with the "
                   "original (un-repaired) baseline as reference.",
                   e.what());
    } catch (...) {
      logger->warn(RMP, 257,
                   "Pre-SA repair_setup failed (unknown exception) - SA "
                   "will run with the original baseline as reference.");
    }

    if (ran) {
      // Legalize rsz-inserted cells (clones, buffers).  rsz places them
      // near their drivers but they are typically not site-aligned — a
      // post-repair `detailed_placement` is what users normally run in
      // Tcl.  Without this step, the final check_placement -verbose at
      // the end of the script fires DPL-0006 (site-aligned check failed)
      // for every clone/buffer rsz inserted, and DPL-0033 aborts.
      if (auto* opendp = resizer->getOpendp()) {
        opendp->detailedPlacement(/*max_displacement_x=*/0,
                                  /*max_displacement_y=*/0,
                                  /*report_file_name=*/"",
                                  /*incremental=*/true);
      }

      if (pre_guard) {
        pre_guard->update();
      }
      auto [post_repair_metrics, post_repair_worst]
          = GetTimingMetrics(sta, corner_);
      logger->info(RMP, 254,
                   "Pre-SA repair_setup persisted: baseline {} -> {} "
                   "(this is now SA's reference; SA will only accept "
                   "candidates that improve on this).",
                   init_metrics.toString(),
                   post_repair_metrics.toString());
      // QOR_STAGE 2 — after repair_timing only (no restructuring yet).
      logger->info(RMP, 281,
                   "QOR_STAGE stage=repair_only wns_ps={:.4f} tns_ps={:.4f} viols={}",
                   static_cast<double>(post_repair_metrics.wns) * 1e12,
                   static_cast<double>(post_repair_metrics.tns) * 1e12,
                   post_repair_metrics.violating_count);
      // Persist the changes so SA's RunGia + repair_setup loop operates
      // on top of the repaired design, not the original.
      odb::dbDatabase::endEco(block);
      // Update SA's reference baseline.
      init_metrics       = post_repair_metrics;
      init_worst         = post_repair_worst;
      spef_wns_baseline_ = init_metrics.wns;
    } else {
      // Roll back any partial changes from a failed repairSetup.
      odb::dbDatabase::undoEco(block);
    }
    // pre_guard is destroyed at scope exit — its dtor calls updateParasitics
    // one last time, refreshing any nets touched by the final state change.
  }
  // ----- end pre-SA repair_setup -----
  // DIAGNOSTIC: composite of the ORIGINAL (untouched) design state.
  // Compare to RMP-0060 (null-remap baseline) to see whether running
  // RunGia(empty_ops) is helpful or harmful for this design.
  const double original_composite = CompositeScore(init_metrics, 0, 0);
  logger->info(RMP,
               59,
               "Resynthesis start: {} original_composite={:.6e}",
               init_metrics.toString(),
               original_composite);

  // ----- capability report -----
  {
    const bool   has_opendp = (resizer->getOpendp() != nullptr);
    auto*        ep         = resizer->getEstimateParasitics();
    const double wire_cap_fF_per_um
        = ep ? ep->wireSignalCapacitance(corner_) * 1e9 : 0.0;
    logger->info(RMP,
                 221,
                 "Post-PnR capabilities: placement-legalizer={}, "
                 "parasitic-estimator={}, wire-cap={:.4f} fF/um",
                 has_opendp ? "available" : "UNAVAILABLE",
                 ep ? "available" : "UNAVAILABLE",
                 wire_cap_fF_per_um);
    if (wire_cap_fF_per_um == 0.0) {
      logger->warn(RMP,
                   222,
                   "Wire RC not set (call set_wire_rc before "
                   "resynth_annealing). New cells will be timed with zero "
                   "wire delay — SA ranking is gate-delay only.");
    } else {
      // SA evaluates each candidate incrementally: SPEF parasitics are kept
      // for existing nets (real routed-wire delays), and only new/changed nets
      // from ABC resynthesis get Steiner-tree estimated parasitics via the
      // IncrementalParasiticsGuard inside Evaluate().
      //
      // Rationale for NOT calling estimateWireParasitics() globally here:
      //   Steiner-tree estimation gives shorter (more optimistic) wire delays
      //   than SPEF for a dense, well-routed design (Steiner tree is a lower
      //   bound on actual route length). If we replace all SPEF with estimated
      //   before SA, the baseline improves artificially (e.g. 93→82 violations
      //   for UART). SA then compares ABC candidates against this optimistic
      //   baseline — but ABC+DPL candidates always produce estimated delays
      //   similar to the (already estimated) baseline, so SA never finds
      //   improvement and always returns empty_ops.
      //
      //   With SPEF as the SA baseline:
      //     • Original nets keep their real routed-wire delays (conservative).
      //     • New cells from ABC get estimated parasitics (Steiner tree, likely
      //       shorter than the original route-congestion-stretched wires).
      //     • Candidates that restructure critical-path logic AND land in good
      //       DPL positions genuinely beat the SPEF baseline, giving SA a
      //       meaningful signal.
      logger->info(RMP,
                   224,
                   "SA evaluation: existing parasitics kept for unchanged nets "
                   "(SPEF if loaded, otherwise set_wire_rc estimates); "
                   "new-cell nets estimated incrementally per candidate "
                   "(wire cap={:.4f} fF/um).",
                   wire_cap_fF_per_um);
    }
  }

  // ----- build ABC library + run SA -----
  cut::AbcLibraryFactory factory(logger);
  factory.AddDbSta(sta);
  factory.AddResizer(resizer);
  factory.SetCorner(corner_);
  cut::AbcLibrary abc_library = factory.Build();

  std::vector<GiaOp> all_ops = GiaOps(logger);

  // ---- Single-pass MCTS: coverage-weighted TNS cone -----------------------
  std::vector<GiaOp> best_ops = RunStrategy(all_ops,
                                            candidate_vertices,
                                            abc_library, sta,
                                            name_generator,
                                            resizer, logger);
  logger->info(RMP, 62,
               "MCTS returned: ops={} empty={}",
               best_ops.size(), best_ops.empty() ? "YES" : "NO");

  // ----- final top-K repair-and-select -------------------------------------
  // RunStrategy ranked candidates by the CHEAP (pre-repair) score.  Log
  // analysis (Group 1 + wb_dma, 34k candidates) shows that ranking is biased:
  // the best-kCheap recipes have the least repair headroom (pre-repair-TNS vs
  // repair-gain correlation = -0.67), so the true post-repair winner is only
  // sporadically the kCheap #1.  In TIERED mode the bias is total — every
  // kCheap candidate is unrepaired while the baseline is post-repair, so SA
  // reports "no cheap-pass improvement" and best_ops comes back empty, yet
  // some shortlisted recipes still beat the baseline AFTER repair.
  //
  // So: re-score each DISTINCT shortlisted recipe under REAL repair (kFull
  // Evaluate = RunGia + DPL + repair_setup + measure + undoEco) and apply the
  // one with the best measured post-repair TNS — but only if it actually beats
  // the post-repair baseline (original_composite).  Each Evaluate restores the
  // design via undoEco, so the persistent apply below still starts from the
  // untouched post-repair baseline.  This is authoritative when active: it
  // overrides RunStrategy's cheap-pass best_ops (which it includes in the
  // shortlist anyway).
  if (final_topk_ > 1 && !top_k_recipes_.empty()) {
    logger->info(RMP, 290,
                 "Final top-K: re-scoring {} distinct recipe(s) under real "
                 "repair (post-repair baseline composite={:.6e}).",
                 top_k_recipes_.size(), original_composite);
    double             best_measured = -std::numeric_limits<double>::infinity();
    std::vector<GiaOp> winner;
    for (size_t i = 0; i < top_k_recipes_.size(); ++i) {
      SolutionSlack trial{top_k_recipes_[i]};
      trial.Evaluate(candidate_vertices, abc_library, corner_, sta,
                     name_generator, resizer, logger, EvalTier::kFull);
      logger->info(RMP, 291,
                   "  recipe {}/{}: ops={} post-repair composite={:.6e} {}",
                   i + 1, top_k_recipes_.size(), top_k_recipes_[i].size(),
                   trial.composite_score_,
                   trial.timing_metrics_.toString());
      if (trial.composite_score_ > best_measured) {
        best_measured = trial.composite_score_;
        winner        = top_k_recipes_[i];
      }
    }
    if (!winner.empty() && best_measured > original_composite) {
      best_ops             = winner;
      last_best_composite_ = best_measured;
      logger->info(RMP, 292,
                   "Final top-K winner beats post-repair baseline: ops={} "
                   "measured composite={:.6e} > baseline={:.6e}.",
                   best_ops.size(), best_measured, original_composite);
    } else {
      best_ops.clear();
      logger->info(RMP, 293,
                   "Final top-K: no recipe beat the post-repair baseline "
                   "(best measured={:.6e} <= baseline={:.6e}); leaving the "
                   "repaired design unchanged.",
                   best_measured, original_composite);
    }
  }

  if (best_ops.empty()) {
    logger->info(RMP, 63,
                 "Resynthesis: MCTS found no improvement; design left unchanged.");
    auto [fm, fu] = GetTimingMetrics(sta, corner_);
    logger->info(RMP, 68, "Resynthesis end: {}", fm.toString());
    logger->info(RMP, 282,
                 "QOR_STAGE stage=restructure_only wns_ps={:.4f} tns_ps={:.4f} viols={}",
                 static_cast<double>(fm.wns) * 1e12,
                 static_cast<double>(fm.tns) * 1e12, fm.violating_count);
    logger->info(RMP, 283,
                 "QOR_STAGE stage=restructure_repair wns_ps={:.4f} tns_ps={:.4f} viols={}",
                 static_cast<double>(fm.wns) * 1e12,
                 static_cast<double>(fm.tns) * 1e12, fm.violating_count);
    return;
  }

  logger->info(RMP, 67, "Resynthesis: applying best ABC operations");

  // ----- timestamp for best-solution snapshot files -----
  char ts_buf[32];
  {
    std::time_t now = std::time(nullptr);
    std::tm     tm_local;
    localtime_r(&now, &tm_local);
    std::strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm_local);
  }
  auto*             block    = sta->db()->getChip()->getBlock();
  const std::string best_pfx = std::string(block->getName()) + "_best_";
  const std::string best_ts(ts_buf);

  // ----- final apply -----
  std::unique_ptr<est::IncrementalParasiticsGuard> apply_guard;
  if (auto* ep = resizer->getEstimateParasitics()) {
    if (ep->wireSignalCapacitance(corner_) > 0.0) {
      apply_guard = std::make_unique<est::IncrementalParasiticsGuard>(ep);
    }
  }

  RunGia(sta,
         candidate_vertices,
         abc_library,
         best_ops,
         kFinalResizeIters,
         name_generator,
         logger);

  // Post-PnR: place + mark all newly inserted cells.
  if (auto* opendp = resizer->getOpendp()) {
    std::vector<odb::dbInst*> new_cells;
    std::set<odb::dbInst*>    placed_in_batch;

    // Collect new cells.
    for (odb::dbInst* inst : block->getInsts()) {
      if (!inst->getPlacementStatus().isPlaced()) {
        new_cells.push_back(inst);
      }
    }

    if (!new_cells.empty()) {
      // IMPROVEMENT: same iterative placement strategy as SA evaluation,
      // for consistency between eval-time and apply-time placement quality.
      for (int pass = 0; pass < kPlacementIterations; ++pass) {
        for (odb::dbInst* inst : new_cells) {
          SetWeightedCentroidHint(
              inst, block, sta, corner_, placed_in_batch);
        }
        if (pass == 0) {
          for (odb::dbInst* inst : new_cells) {
            placed_in_batch.insert(inst);
          }
        }
      }

      // Snapshot 1/2 — before legalization (centroid hints visible).
      const std::string snap_after_rungia
          = best_pfx + "after_rungia_" + best_ts + ".def";
      {
        odb::DefOut writer(logger);
        writer.writeBlock(block, snap_after_rungia.c_str());
      }
      logger->info(RMP,
                   231,
                   "Best-solution snapshot [1/2 after_rungia — {} unplaced "
                   "new cell(s)]: {}",
                   static_cast<int>(new_cells.size()),
                   snap_after_rungia);

      const int dpl_failures
          = opendp->legalizeNewCellsIncremental(kDplMaxDispX, kDplMaxDispY);

      for (odb::dbInst* inst : new_cells) {
        inst->setPlacementStatus(odb::dbPlacementStatus::PLACED);
      }

      // Snapshot 2/2 — final state (everything placed and legal).
      const std::string snap_legalized
          = best_pfx + "legalized_" + best_ts + ".def";
      {
        odb::DefOut writer(logger);
        writer.writeBlock(block, snap_legalized.c_str());
      }
      logger->info(RMP,
                   235,
                   "Best-solution snapshot [2/2 legalized — {} new cell(s) "
                   "placed]: {}",
                   static_cast<int>(new_cells.size()),
                   snap_legalized);

      if (dpl_failures > 0) {
        logger->warn(RMP,
                     230,
                     "Post-PnR final apply: {}/{} new cells could not find "
                     "a legal slot — design may have DRC violations",
                     dpl_failures,
                     static_cast<int>(new_cells.size()));
      }
    }
    logger->info(RMP,
                 220,
                 "Post-PnR final apply: {} new cells legalized",
                 static_cast<int>(new_cells.size()));
  }

  // Parasitics refresh — INCREMENTAL only, matching SA's Evaluate.
  // We intentionally do NOT call estimateWireParasitics() here.  The earlier
  // version of this code did, which forced a global Steiner re-estimation
  // of every net in the design.  That changed RC on nets SA never touched
  // and shifted the composite enough to disagree with SA's recorded best
  // (RMP-0261 drift ranged from -2e-11 to +4e-12).  Using guard->update()
  // refreshes only the dirty nets the apply_guard tracked since RunGia,
  // identical to what guard->update() does inside Evaluate.
  if (apply_guard) {
    apply_guard->update();
  }

  // QOR_STAGE 3 — after restructuring + stitch-back (GIA + DPL), BEFORE repair_timing.
  // Captures the raw ABC restructuring gain independent of the repair pass.
  {
    auto [restructure_metrics, restructure_worst] = GetTimingMetrics(sta, corner_);
    logger->info(RMP, 284,
                 "QOR_STAGE stage=restructure_only wns_ps={:.4f} tns_ps={:.4f} viols={}",
                 static_cast<double>(restructure_metrics.wns) * 1e12,
                 static_cast<double>(restructure_metrics.tns) * 1e12,
                 restructure_metrics.violating_count);
  }

  // Final-apply repair_setup — mirrors the pre-SA repair_setup block, so
  // the final design timing matches what SA used to rank candidates.
  //
  // WHY THIS IS REQUIRED
  // ====================
  // Every SA Evaluate call internally ran repair_setup (step 2b) on the
  // candidate before measuring its composite.  best_ops therefore points to
  // a candidate whose recorded WNS is the POST-REPAIR value.  Without this
  // step the final-apply path leaves the new cone in its RAW post-ABC
  // state — typically 100-300 ps worse on UART/GCD because rsz's buffer
  // trees and per-cell sizing have not been applied to the cone.
  //
  // STRUCTURE MUST MATCH THE PRE-SA PATTERN
  // =======================================
  // - beginEco / endEco wrap: rsz's journalBegin/journalEnd nest inside
  //   our outer transaction.  Without the outer transaction, rsz's
  //   journalEnd -> commitEco walked uninitialised ODB state and Sig-11ed.
  // - IncrementalParasiticsGuard: rsz registers callbacks via setDbCbkOwner
  //   when an outer guard is active; without it, ODB changes are not
  //   reflected back to the parasitics estimator and rsz makes decisions
  //   on stale RC.  This is also what the rsz single-endpoint repairSetup
  //   variant does explicitly (RepairSetup.cc:575).
  // - max_repairs_per_pass = 1: matches the pre-SA call and the Tcl
  //   default.  Larger values are documented to break binding-WNS
  //   endpoint handling.
  if (kSaRepairSetupMaxPasses > 0 && resizer != nullptr) {
    auto* final_block = sta->db()->getChip()->getBlock();
    odb::dbDatabase::beginEco(final_block);

    std::unique_ptr<est::IncrementalParasiticsGuard> final_guard;
    if (auto* ep = resizer->getEstimateParasitics()) {
      if (ep->wireSignalCapacitance(corner_) > 0.0) {
        final_guard = std::make_unique<est::IncrementalParasiticsGuard>(ep);
      }
    }

    bool repair_ran = false;
    try {
      resizer->repairSetup(/*setup_margin=*/0.0,
                           /*repair_tns_end_percent=*/1.0,
                           /*max_passes=*/kSaRepairSetupMaxPasses,
                           /*max_iterations=*/-1,
                           /*max_repairs_per_pass=*/1,
                           /*match_cell_footprint=*/false,
                           /*verbose=*/false,
                           /*sequence=*/{},
                           /*phases=*/"",
                           /*skip_pin_swap=*/false,
                           /*skip_gate_cloning=*/false,
                           /*skip_size_down=*/false,
                           /*skip_buffering=*/false,
                           /*skip_buffer_removal=*/true,
                           /*skip_last_gasp=*/true,
                           /*skip_vt_swap=*/true,
                           /*skip_crit_vt_swap=*/true);
      repair_ran = true;
    } catch (const std::exception& e) {
      logger->warn(RMP, 259,
                   "Final-apply repair_setup failed: {} - reported WNS may "
                   "be worse than SA's evaluated best.",
                   e.what());
    } catch (...) {
      logger->warn(RMP, 260,
                   "Final-apply repair_setup failed (unknown exception) - "
                   "reported WNS may be worse than SA's evaluated best.");
    }

    if (repair_ran) {
      if (final_guard) {
        final_guard->update();
      }
      // Legalize any clones/buffers rsz inserted.  Without this,
      // check_placement -verbose may fire DPL-0006/DPL-0033 on
      // rsz-inserted but not-site-aligned cells.
      if (auto* opendp = resizer->getOpendp()) {
        opendp->detailedPlacement(/*max_displacement_x=*/0,
                                  /*max_displacement_y=*/0,
                                  /*report_file_name=*/"",
                                  /*incremental=*/true);
      }
      logger->info(RMP, 258,
                   "Final-apply repair_setup completed (max_passes={}).",
                   kSaRepairSetupMaxPasses);
      odb::dbDatabase::endEco(final_block);
    } else {
      odb::dbDatabase::undoEco(final_block);
    }
    // final_guard is destroyed at scope exit, AFTER endEco/undoEco — same
    // ordering as Evaluate and the pre-SA repair block.
  }

  // Final timing report — composite metrics so the user sees overall health,
  // not just WNS.
  auto [final_metrics, final_worst] = GetTimingMetrics(sta, corner_);
  logger->info(RMP, 69, "Resynthesis end: {}", final_metrics.toString());
  // QOR_STAGE 4 — after restructuring + repair_timing (final design state).
  logger->info(RMP, 285,
               "QOR_STAGE stage=restructure_repair wns_ps={:.4f} tns_ps={:.4f} viols={}",
               static_cast<double>(final_metrics.wns) * 1e12,
               static_cast<double>(final_metrics.tns) * 1e12,
               final_metrics.violating_count);

  // ----- post-apply composite drift report -----
  // SA evaluates each candidate under INCREMENTAL Steiner parasitics:
  // existing nets keep their pre-SA SPEF/wire-RC values; only nets touched
  // by the candidate's cone get fresh Steiner estimation.  The final-apply
  // path then calls estimateWireParasitics() GLOBALLY, replacing RC on
  // every net — including untouched ones — and reruns repair_setup on the
  // re-estimated design.  These two parasitics regimes are not identical
  // and can shift the composite by enough to confuse anyone reading the
  // SA log.  Worse, the violation count is the most sensitive term and
  // tends to drift the most (marginal endpoints can flip under global
  // re-estimation).
  //
  // Rather than hide the drift, log SA's recorded best composite next to
  // the post-apply re-measured composite so the gap is visible.
  //
  // dpl_failures and total_new_cells are passed as 0 here: the DplPenalty
  // would already have produced a warn earlier in the apply path; for the
  // post-apply composite snapshot we treat the placement as legal.
  const double post_apply_composite
      = CompositeScore(final_metrics, /*dpl_failures=*/0,
                       /*total_new_cells=*/0);
  if (std::isnan(last_best_composite_)) {
    // SA returned empty_ops (no-op path); not reachable from this branch
    // since we only get here when SA returned a non-empty best_ops, but
    // logged for completeness.
    logger->info(RMP, 261,
                 "Post-apply composite: {:.6e} "
                 "(SA returned no-op, no in-SA best to compare).",
                 post_apply_composite);
  } else {
    const double drift = post_apply_composite - last_best_composite_;
    logger->info(RMP, 262,
                 "Post-apply composite: {:.6e}  (SA recorded best: {:.6e}, "
                 "drift: {:+.4e}).  Drift is normally caused by the global "
                 "estimateWireParasitics() call and the final-apply "
                 "repair_setup making different moves than the per-iter "
                 "Evaluate did under incremental parasitics.",
                 post_apply_composite,
                 last_best_composite_,
                 drift);
  }
}

}  // namespace rmp