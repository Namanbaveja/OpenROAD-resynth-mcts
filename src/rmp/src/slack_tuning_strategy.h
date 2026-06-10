// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#pragma once

#include <cmath>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cut/abc_library_factory.h"
#include "gia.h"
#include "resynthesis_strategy.h"
#include "sta/Delay.hh"
#include "utl/unique_name.h"

namespace rsz {
class Resizer;
}  // namespace rsz

namespace sta {
class dbSta;
class Scene;
class Vertex;
}  // namespace sta

namespace utl {
class Logger;
}  // namespace utl

namespace rmp {

// Captures the full timing distribution for composite SA scoring.
//
// Beyond the classical (WNS, TNS, viol) triple we also track:
//   • kwns_sum_     — sum of the worst K endpoint slacks (K = kKwnsK).
//                     Generalises WNS (K=1) to "cluster of worst paths".
//                     Decouples scoring from a single hostage endpoint.
//   • squared_violator_sum_ — Σ slack² over violating endpoints. Used to
//                     compute RMS violator slack; rewards tight slack
//                     distributions over spread-out ones.
struct TimingMetrics
{
  sta::Slack wns                    = 0.0;
  sta::Slack tns                    = 0.0;
  int        violating_count        = 0;
  int        total_endpoints        = 0;

  // Sum of slacks of the K most-negative endpoints (KWNS).  When fewer
  // than K endpoints exist, the sum covers all of them.  Always ≤ 0.
  sta::Slack kwns_sum               = 0.0;
  int        kwns_endpoints_counted = 0;

  // Sum over violating endpoints of slack².  Always ≥ 0.
  double     squared_violator_sum   = 0.0;

  double NormalizedTns() const
  {
    return total_endpoints > 0
               ? static_cast<double>(tns) / total_endpoints
               : 0.0;
  }

  // Average violator depth: TNS / max(violators, 1).  When there are no
  // violators this is 0 (the design is timing-clean).
  double TnsPerViolator() const
  {
    return violating_count > 0
               ? static_cast<double>(tns) / violating_count
               : 0.0;
  }

  double ViolationRatio() const
  {
    return total_endpoints > 0
               ? static_cast<double>(violating_count) / total_endpoints
               : 0.0;
  }

  // Average of the K worst-slack endpoints' slacks (K-WNS).  Same units
  // as wns/tns (seconds, ≤ 0).
  double KwnsMean() const
  {
    return kwns_endpoints_counted > 0
               ? static_cast<double>(kwns_sum) / kwns_endpoints_counted
               : 0.0;
  }

  // Root-mean-square of violator slacks: √(Σ slack² / N_viol).  Returns
  // a non-negative magnitude (in seconds); larger means worse violator
  // distribution.  When there are no violators this is 0.
  double RmsViolatorSlack() const
  {
    if (violating_count <= 0 || squared_violator_sum <= 0.0) {
      return 0.0;
    }
    return std::sqrt(squared_violator_sum / violating_count);
  }

  std::string toString() const
  {
    std::ostringstream os;
    os << "WNS=" << wns << " TNS=" << tns << " viol=" << violating_count
       << "/" << total_endpoints
       << " KWNS_mean=" << KwnsMean() << " (K=" << kwns_endpoints_counted
       << ") RMS_viol=" << RmsViolatorSlack();
    return os.str();
  }
};

// Controls how expensive the timing evaluation inside Evaluate() is.
//
//   kCheap — ABC + DPL + incremental parasitics + STA.  No repair_setup.
//            Fast (~0.3s on UART).  Used for all MCTS iterations so the
//            tree can explore 5-7× more candidates in the same wall time.
//            Scores are noisier but relative ranking is preserved.
//
//   kFull  — Adds repair_setup on top of kCheap (~2s on UART).  Used only
//            for the top-K candidates found by the cheap pass so the final
//            winner is scored accurately under post-repair timing.
enum class EvalTier { kCheap, kFull };

class SolutionSlack final
{
 public:
  using ResultOps = std::vector<GiaOp>;

 private:
  ResultOps solution_;
  std::optional<sta::Slack> worst_slack_ = std::nullopt;

 public:
  double         composite_score_ = 0.0;
  TimingMetrics  timing_metrics_;

  explicit SolutionSlack() = default;
  explicit SolutionSlack(ResultOps solution) : solution_{std::move(solution)} {}
  explicit SolutionSlack(ResultOps& solution, sta::Slack worst_slack)
      : solution_{solution}, worst_slack_{worst_slack}
  {
  }

  std::string toString() const;
  ResultOps RandomNeighbor(const ResultOps& all_ops,
                           utl::Logger* logger,
                           std::mt19937& random) const;

  std::pair<sta::Slack, sta::Delay> Evaluate(
      const std::vector<sta::Vertex*>& candidate_vertices,
      cut::AbcLibrary& abc_library,
      sta::Scene* corner,
      sta::dbSta* sta,
      utl::UniqueName& name_generator,
      rsz::Resizer* resizer,
      utl::Logger* logger,
      EvalTier tier = EvalTier::kFull);

  // Populate composite_score_ and timing_metrics_ from the CURRENT design
  // state without calling RunGia. Used as SA's true baseline (the original
  // P&R design) instead of Evaluate(empty_ops), which destructively re-maps
  // the cone via Abc_NtkMap and is not actually a no-op.
  void EvaluateCurrentState(sta::dbSta* sta, sta::Scene* corner);
  ResultOps Solution() const { return solution_; }
  ResultOps& Solution() { return solution_; }
  std::optional<sta::Slack> WorstSlack() const { return worst_slack_; }
};

class SlackTuningStrategy : public ResynthesisStrategy
{
 public:
  explicit SlackTuningStrategy(sta::Scene* corner,
                               sta::Slack slack_threshold,
                               int seed,
                               unsigned iterations,
                               unsigned initial_ops,
                               float percentage = 100.0f,
                               float wns_pct    = 10.0f)
      : corner_(corner),
        slack_threshold_(slack_threshold),
        iterations_(iterations),
        initial_ops_(initial_ops),
        percentage_(percentage),
        wns_pct_(wns_pct)
  {
    random_.seed(seed);
  }

  void OptimizeDesign(sta::dbSta* sta,
                      utl::UniqueName& name_generator,
                      rsz::Resizer* resizer,
                      utl::Logger* logger) override;

  // Number of distinct best-by-kCheap recipes RunStrategy retains so that
  // OptimizeDesign can re-score each one under real (kFull) repair and pick
  // the measured post-repair winner.  1 = legacy behaviour (apply the single
  // kCheap best directly).  See top_k_recipes_.
  void setFinalTopK(unsigned k) { final_topk_ = k; }

  virtual std::vector<GiaOp> RunStrategy(
      const std::vector<GiaOp>& all_ops,
      const std::vector<sta::Vertex*>& candidate_vertices,
      cut::AbcLibrary& abc_library,
      sta::dbSta* sta,
      utl::UniqueName& name_generator,
      rsz::Resizer* resizer,
      utl::Logger* logger)
      = 0;

 protected:
  sta::Scene* corner_;
  sta::Slack slack_threshold_;
  unsigned iterations_;
  unsigned initial_ops_;
  // 100.0f = no filter (use every violating endpoint).  Values in (0, 100)
  // truncate candidate_vertices to the worst N% by slack — see
  // SlackTuningStrategy::OptimizeDesign for the filter.
 public:
 protected:
  float percentage_;
  float wns_pct_;
  std::mt19937 random_;
  // WNS measured with SPEF parasitics before estimateWireParasitics() replaces
  // them. Used as the WNS regression guard reference so the guard is calibrated
  // on real routed-wire delays rather than the estimated model which can give
  // dramatically different (often much better) WNS values.
  sta::Slack spef_wns_baseline_ = 0.0f;
  // Set by RunStrategy at exit: the composite score of the chosen best_ops
  // as measured by SA's per-iter Evaluate (incremental Steiner parasitics
  // for cone-touched nets, pre-SA SPEF/wire-RC for untouched nets).
  // OptimizeDesign reads this after the final-apply path runs its own
  // global Steiner re-estimation + final repair_setup, and logs the two
  // composite values side by side so the user sees how much the
  // parasitics-model swap shifted the composite.  Holds quiet_NaN when
  // SA returned empty_ops (no candidate selected).
  double last_best_composite_ = std::numeric_limits<double>::quiet_NaN();

  // Final-apply shortlist size.  RunStrategy fills top_k_recipes_ with up to
  // this many DISTINCT op-sequences (deduped by exact GiaOp.id order), ranked
  // by kCheap composite.  Log analysis (Group 1 + wb_dma, 34k candidates):
  // ranking purely by the cheap pre-repair score is a biased selector — the
  // best-kCheap recipes have the least repair headroom (pre-repair-TNS vs
  // repair-gain correlation = -0.67), so the post-repair winner is in the
  // kCheap top-5 only sporadically.  Re-scoring 5 distinct recipes under real
  // repair and picking the measured best cuts mean regret-vs-exhaustive from
  // ~800 ps (top-1) to ~500 ps, and deduping by recipe is what fixes the
  // worst cases (SA revisits the same sequence, collapsing a raw top-5 to
  // 1-2 distinct recipes).
  unsigned final_topk_ = 5;
  // Distinct best-by-kCheap recipes, ranked composite-descending, filled by
  // RunStrategy when final_topk_ > 1.  Empty when SA found no improvement.
  std::vector<std::vector<GiaOp>> top_k_recipes_;
};

}  // namespace rmp
