// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors
//
// =============================================================================
// IMPROVED VERSION — Changes from upstream:
//   1. SA decisions now driven by SolutionSlack::composite_score_ instead of
//      WNS-only. The composite folds in WNS + TNS + violation count + DPL
//      failure penalty, giving SA a holistic timing target.
//   2. WNS is preserved in logs for human-readable progress (engineers think
//      in WNS), but acceptance / best-tracking compares composite scores.
//   3. Acceptance probability uses the same composite delta — units are
//      seconds-equivalent so the existing temperature scale (set from the
//      required time of the worst path) still works without retuning.
//
// HEADER-LEVEL ASSUMPTION:
//   SolutionSlack must expose:
//     - composite_score_ : double      (set by Evaluate)
//     - timing_metrics_  : TimingMetrics (set by Evaluate)
//   See slack_tuning_strategy.cpp for the implementation that populates
//   these fields.
// =============================================================================

#include "annealing_strategy.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "absl/random/distributions.h"
#include "cut/abc_library_factory.h"
#include "cut/logic_extractor.h"
#include "db_sta/dbSta.hh"
#include "db_sta/dbNetwork.hh"
#include "gia.h"
#include "map/if/if.h"
#include "map/mio/mio.h"
#include "map/scl/sclLib.h"
#include "map/scl/sclSize.h"
#include "misc/extra/extra.h"
#include "misc/nm/nm.h"
#include "misc/util/abc_global.h"
#include "misc/vec/vecPtr.h"
#include "odb/db.h"
#include "rsz/Resizer.hh"
#include "slack_tuning_strategy.h"
#include "sta/Delay.hh"
#include "sta/Graph.hh"
#include "sta/Units.hh"
#include "utils.h"
#include "utl/Logger.h"
#include "utl/deleter.h"
#include "utl/unique_name.h"

namespace rmp {
using utl::RMP;

namespace {

// Bounded collector of the K best DISTINCT recipes seen during SA, keyed by
// the exact ordered sequence of GiaOp ids.  SA revisits the same op-sequence
// many times (revert-on-stagnation, repeated neighbors), so deduping by recipe
// is essential — without it a raw "top-5 by score" frequently collapses to
// 1-2 distinct recipes.  K is tiny (≈5) so linear scans are fine.
class TopKRecipes
{
 public:
  explicit TopKRecipes(size_t k) : k_(k) {}

  // Offer a candidate.  Keeps the best composite per distinct recipe and,
  // once full, evicts the lowest-composite entry when a better one arrives.
  void Consider(double composite, const std::vector<GiaOp>& ops)
  {
    if (ops.empty()) {
      return;  // empty recipe == no-op; never a shortlist candidate
    }
    for (auto& e : entries_) {
      if (SameRecipe(e.ops, ops)) {
        if (composite > e.composite) {
          e.composite = composite;
        }
        return;
      }
    }
    if (entries_.size() < k_) {
      entries_.push_back({composite, ops});
      return;
    }
    size_t worst = 0;
    for (size_t i = 1; i < entries_.size(); ++i) {
      if (entries_[i].composite < entries_[worst].composite) {
        worst = i;
      }
    }
    if (composite > entries_[worst].composite) {
      entries_[worst] = {composite, ops};
    }
  }

  // Recipes ranked composite-descending (best first).
  std::vector<std::vector<GiaOp>> Ranked() const
  {
    std::vector<Entry> sorted = entries_;
    std::sort(sorted.begin(), sorted.end(),
              [](const Entry& a, const Entry& b) {
                return a.composite > b.composite;
              });
    std::vector<std::vector<GiaOp>> out;
    out.reserve(sorted.size());
    for (auto& e : sorted) {
      out.push_back(e.ops);
    }
    return out;
  }

 private:
  struct Entry
  {
    double             composite;
    std::vector<GiaOp> ops;
  };

  static bool SameRecipe(const std::vector<GiaOp>& a,
                         const std::vector<GiaOp>& b)
  {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
      if (a[i].id != b[i].id) {
        return false;
      }
    }
    return true;
  }

  size_t             k_;
  std::vector<Entry> entries_;
};

}  // namespace

std::vector<GiaOp> AnnealingStrategy::RunStrategy(
    const std::vector<GiaOp>&        all_ops,
    const std::vector<sta::Vertex*>& candidate_vertices,
    cut::AbcLibrary&                 abc_library,
    sta::dbSta*                      sta,
    utl::UniqueName&                 name_generator,
    rsz::Resizer*                    resizer,
    utl::Logger*                     logger)
{
  // -------------------------------------------------------------------------
  // Step 1: read the ORIGINAL design's timing as SA's baseline.
  //
  // We deliberately do NOT call Evaluate(empty_ops) here — that path runs
  // RunGia(empty_ops), which still extracts the cone and re-maps it through
  // Abc_NtkMap. For some designs (e.g. tightly P&R-tuned cones) that re-map
  // is destructive, so SA would compare candidates against a degraded
  // reference and apply recipes that beat the degraded reference but are
  // still worse than the original. EvaluateCurrentState reads STA on the
  // untouched design, giving SA the correct comparison point.
  // -------------------------------------------------------------------------
  debugPrint(logger,
             RMP,
             "annealing",
             1,
             "Evaluating baseline from current (original) design state");

  std::vector<GiaOp> empty_ops;
  SolutionSlack      baseline_slack;
  baseline_slack.EvaluateCurrentState(sta, corner_);
  const double     baseline_composite = baseline_slack.composite_score_;
  const sta::Slack baseline_wns_value = baseline_slack.timing_metrics_.wns;

  logger->info(RMP,
               60,
               "Baseline (original design): composite={:.6e}, {}",
               baseline_composite,
               baseline_slack.timing_metrics_.toString());

  // -------------------------------------------------------------------------
  // Step 2: seed SA with `initial_ops_` random operations (existing behaviour)
  // -------------------------------------------------------------------------
  debugPrint(logger,
             RMP,
             "annealing",
             1,
             "Generating and evaluating the initial random solution");

  std::vector<GiaOp> ops;
  ops.reserve(initial_ops_);
  for (size_t i = 0; i < initial_ops_; i++) {
    const auto idx = absl::Uniform<size_t>(random_, 0, all_ops.size());
    ops.push_back(all_ops[idx]);
  }

  // eval_mode controls the per-iteration scoring strategy:
  //   "full"   — kFull every iteration (repair_setup in loop, accurate).
  //   "tiered" — kCheap every iteration (no repair), kFull for top-K final.
  //   "cone"   — RunGiaConeSlack every iteration: pure ABC cone delay (To-Tr),
  //              no ODB writes, no DPL, no parasitic update, no undoEco.
  //              ~100× faster than kFull; score is physically unbiased (no
  //              Steiner optimism — measures gate delay on the cone only).
  //              top-K final re-scoring with kFull unchanged.
  const bool tiered_sa = (eval_mode_ == "tiered");
  const bool cone_sta  = (eval_mode_ == "cone");
  const EvalTier sa_tier = tiered_sa ? EvalTier::kCheap : EvalTier::kFull;

  // ---- Pre-extract cone data for cone_sta mode (done once, reused per iter) ----
  // Mirrors BuildConeGroups in cone_manager.cpp: extract LogicCut from all
  // candidate_vertices merged into one group, then read per-PI arrival times
  // and per-PO required times from the post-repair STA graph.
  std::optional<cut::LogicCut>            sa_cut;
  std::unordered_map<std::string, float>  sa_pi_arrivals;
  std::unordered_map<std::string, float>  sa_po_requireds;
  float                                   sa_lib_scale    = 1.0f;
  double                                  sa_delay_budget = 0.0;
  bool                                    cone_ready      = false;

  if (cone_sta) {
    cut::LogicExtractorFactory extractor(sta, logger);
    for (sta::Vertex* v : candidate_vertices) {
      extractor.AppendEndpoint(v);
    }
    sa_cut.emplace(extractor.BuildLogicCut(abc_library));

    if (sa_cut->IsEmpty()) {
      logger->warn(RMP, 294,
                   "SA cone-STA: empty cone after extraction — "
                   "falling back to kCheap (tiered) evaluation.");
    } else {
      sa_lib_scale = static_cast<float>(
          sta->units()->timeUnit()->scale());
      sta::dbNetwork* network = sta->getDbNetwork();

      float min_required_f = std::numeric_limits<float>::max();
      float max_arrival_f  = std::numeric_limits<float>::lowest();

      // per-PO required times (tightest per net across all endpoints)
      for (sta::Vertex* ep : candidate_vertices) {
        const sta::Delay req = sta->required(ep,
                                             sta::RiseFallBoth::riseFall(),
                                             sta->scenes(),
                                             sta::MinMax::max());
        const float req_f = static_cast<float>(req);
        if (req_f < min_required_f) {
          min_required_f = req_f;
        }
        sta::Pin* pin    = ep->pin();
        sta::Net* po_net = network->net(pin);
        if (!po_net) {
          if (sta::Term* term = network->term(pin)) {
            po_net = network->net(term);
          }
        }
        if (po_net && sa_lib_scale > 0.0f) {
          const std::string net_path = network->pathName(po_net);
          if (!net_path.empty()) {
            const float val = req_f / sa_lib_scale;
            auto [it, ok]   = sa_po_requireds.emplace(net_path, val);
            if (!ok && val < it->second) {
              it->second = val;
            }
          }
        }
      }

      // per-PI arrival times (max arrival at each primary-input net)
      for (const sta::Net* net : sa_cut->primary_inputs()) {
        sta::PinSet* drivers = network->drivers(net);
        if (!drivers) {
          continue;
        }
        float net_arrival_f = std::numeric_limits<float>::lowest();
        for (const sta::Pin* drv_pin : *drivers) {
          const sta::VertexId vid   = network->vertexId(drv_pin);
          sta::Vertex*        drv_v = sta->graph()->vertex(vid);
          if (!drv_v) {
            continue;
          }
          const sta::Arrival arr = sta->arrival(drv_v,
                                                sta::RiseFallBoth::riseFall(),
                                                sta->scenes(),
                                                sta::MinMax::max());
          const float arr_f = static_cast<float>(arr);
          if (arr_f > net_arrival_f) {
            net_arrival_f = arr_f;
          }
          if (arr_f > max_arrival_f) {
            max_arrival_f = arr_f;
          }
        }
        if (net_arrival_f > std::numeric_limits<float>::lowest()
            && sa_lib_scale > 0.0f) {
          const std::string net_path = network->pathName(net);
          if (!net_path.empty()) {
            sa_pi_arrivals[net_path] = net_arrival_f / sa_lib_scale;
          }
        }
      }

      // scalar delay budget: min_required - max_arrival (Liberty units)
      if (min_required_f < std::numeric_limits<float>::max()
          && sa_lib_scale > 0.0f) {
        const float max_arr
            = (max_arrival_f > std::numeric_limits<float>::lowest())
                  ? max_arrival_f
                  : 0.0f;
        sa_delay_budget
            = static_cast<double>(min_required_f - max_arr) / sa_lib_scale;
      }

      cone_ready = true;
      logger->info(RMP, 295,
                   "SA cone-STA: cone extracted — {} PIs  {} POs  {} insts  "
                   "delay_budget={:.4f} (Liberty units)",
                   sa_pi_arrivals.size(),
                   sa_po_requireds.size(),
                   static_cast<int>(sa_cut->cut_instances().size()),
                   sa_delay_budget);
    }
  }

  // Helper lambda: evaluate one op-sequence.
  // In cone_sta mode: RunGiaConeSlack — no ODB writes, score = To-Tr.
  // Otherwise: Evaluate() with the configured tier.
  // Returns {score, wns}.  wns is 0 in cone_sta mode (not available).
  auto eval_ops = [&](const std::vector<GiaOp>& ev_ops,
                      SolutionSlack&             ss) -> std::pair<double, sta::Slack> {
    if (cone_sta && cone_ready) {
      const double improvement = RunGiaConeSlack(sta,
                                                 *sa_cut,
                                                 abc_library,
                                                 ev_ops,
                                                 sa_pi_arrivals,
                                                 sa_po_requireds,
                                                 sa_lib_scale,
                                                 sa_delay_budget,
                                                 logger);
      return {improvement, sta::Slack(0.0)};
    }
    auto [ws, req] = ss.Evaluate(candidate_vertices,
                                 abc_library,
                                 corner_,
                                 sta,
                                 name_generator,
                                 resizer,
                                 logger,
                                 sa_tier);
    return {ss.composite_score_, ws};
  };

  SolutionSlack sol_slack{ops};
  auto [composite, worst_slack] = eval_ops(ops, sol_slack);
  // In cone_sta mode the score is To-Tr (Liberty units, > 0 = improvement).
  // The effective baseline is 0 (no improvement), not the full-design TNS.
  const double effective_baseline = cone_sta ? 0.0 : baseline_composite;

  // Shortlist of the K best DISTINCT recipes by cheap score.  Seeded with
  // the initial random solution; every evaluated neighbor is offered below.
  TopKRecipes topk(final_topk_);
  topk.Consider(composite, ops);

  // -------------------------------------------------------------------------
  // Temperature, cooling schedule, and stagnation defaults.
  // T₀ = |initial_score - effective_baseline|.
  // In cone_sta mode: effective_baseline=0, so T₀ = |initial To-Tr|.
  // In full/tiered:   effective_baseline=baseline_composite (design TNS).
  // -------------------------------------------------------------------------
  if (!temperature_) {
    constexpr float kMinT0 = 1.0e-6f;  // 1 ns Liberty-unit floor for cone mode
    const double delta_abs
        = std::abs(static_cast<double>(composite)
                   - static_cast<double>(effective_baseline));
    temperature_ = std::max(static_cast<float>(delta_abs), kMinT0);
  }

  // Geometric cooling: T(i) = T0 × alpha^i, alpha chosen so the last
  // iteration is ~0.5% of T0 (200× reduction).
  const float cooling_alpha
      = (iterations_ > 1)
            ? std::pow(0.005f,
                       1.0f / static_cast<float>(iterations_ - 1))
            : 1.0f;

  // If revert-on-stagnation isn't configured, default to N/5.
  const unsigned effective_revert_after
      = revert_after_.value_or(std::max(10u, iterations_ / 5));

  // Hard guard against catastrophic WNS regression.
  // Reference: use spef_wns_baseline_ (WNS measured with SPEF before
  // estimateWireParasitics replaced them) so the guard is calibrated on
  // real routed-wire delays.  When estimated parasitics give positive WNS,
  // using baseline_wns_value would set the threshold near 0 ps and reject
  // every candidate — spef_wns_baseline_ is always the more negative
  // (conservative) reference.
  const sta::Slack wns_guard_ref
      = std::min(spef_wns_baseline_, baseline_wns_value);
  // 10.00× |baseline_WNS| — sanity net only.
  // With the richer composite (KWNS_mean + TNS/N_viol + viol_count +
  // RMS_viol), the composite itself weighs the trade-off between WNS
  // regression and gains on the other axes.  This hard guard exists only
  // to reject pathological candidates (DPL meltdown, ABC produced a wholly
  // un-timeable netlist, etc.) and should fire essentially never on a
  // healthy design.  10× the baseline magnitude is loose enough that
  // Metropolis decides for normal candidates and tight enough to catch
  // true blow-ups.
  const sta::Slack max_wns_regression_abs
      = std::abs(wns_guard_ref) * 10.00f;

  logger->info(RMP, 52, "Resynthesis: starting simulated annealing");
  if (cone_sta) {
    logger->info(RMP, 297,
                 "Cone-STA mode: T₀={:.4e}, α={:.4f}, revert_after={}, "
                 "initial cone_improvement={:.4f} Liberty-units",
                 *temperature_, cooling_alpha, effective_revert_after,
                 composite);
  } else {
    logger->info(RMP, 505,
                 "Initial temp: {:.4e}, cooling α: {:.4f}, "
                 "revert_after: {}, initial composite: {:.6e}, {}",
                 *temperature_, cooling_alpha, effective_revert_after,
                 composite, sol_slack.timing_metrics_.toString());
  }

  // -------------------------------------------------------------------------
  // Step 3: initialize "best" as the better of (effective_baseline, init).
  // -------------------------------------------------------------------------
  double             best_composite;
  sta::Slack         best_worst_slack;
  std::vector<GiaOp> best_ops;
  if (composite > effective_baseline) {
    best_composite   = composite;
    best_worst_slack = worst_slack;
    best_ops         = ops;
  } else {
    best_composite   = effective_baseline;
    best_worst_slack = baseline_wns_value;
    best_ops         = empty_ops;
  }

  // Iterations since the last improvement to `best`. Reset on every new best;
  // when it crosses `effective_revert_after` we jump back to `best_ops`.
  unsigned iters_since_best = 0;

  // -------------------------------------------------------------------------
  // Annealing loop
  // -------------------------------------------------------------------------
  for (unsigned i = 0; i < iterations_; i++) {
    const float current_temp
        = *temperature_ * std::pow(cooling_alpha, static_cast<float>(i));

    // Revert-on-stagnation: if we've gone too long without finding a new best,
    // jump back to the best-known solution and continue exploring from there.
    if (iters_since_best >= effective_revert_after) {
      debugPrint(logger,
                 RMP,
                 "annealing",
                 1,
                 "Reverting to best after {} stagnant iterations",
                 iters_since_best);
      ops              = best_ops;
      composite        = best_composite;
      worst_slack      = best_worst_slack;
      iters_since_best = 0;
    }

    // Periodic progress log (every 10 iters).
    if ((i + 1) % 10 == 0) {
      if (cone_sta) {
        logger->info(RMP, 296,
                     "Iter {}/{} T={:.4e} best cone_improvement={:.4f}",
                     i + 1, iterations_, current_temp, best_composite);
      } else {
        logger->info(RMP, 506,
                     "Iter {}/{} T={:.4e} best composite={:.6e} best WNS={}",
                     i + 1, iterations_, current_temp,
                     best_composite, best_worst_slack);
      }
    }

    // ----- generate + evaluate neighbor -----
    SolutionSlack s{ops};
    auto          new_ops = s.RandomNeighbor(all_ops, logger, random_);
    sol_slack             = SolutionSlack{new_ops};

    auto [composite_new, worst_slack_new] = eval_ops(new_ops, sol_slack);

    // Offer every evaluated candidate to the shortlist.
    topk.Consider(composite_new, new_ops);

    // WNS guard: skip in cone_sta mode (no design-level WNS available).
    if (!cone_sta
        && worst_slack_new < wns_guard_ref - max_wns_regression_abs) {
      debugPrint(logger, RMP, "annealing", 1,
                 "Rejecting catastrophic WNS regression: WNS_new={} "
                 "(spef_ref={}, limit -{})",
                 worst_slack_new, wns_guard_ref, max_wns_regression_abs);
      ++iters_since_best;
      continue;
    }

    // ----- Metropolis acceptance test on COMPOSITE score -----
    // Higher composite = better. composite_new < composite means the new
    // solution is worse, so we accept probabilistically based on how much
    // worse and how hot.
    if (composite_new < composite) {
      const float accept_prob
          = current_temp <= 0.0f
                ? 0.0f
                : std::exp(static_cast<float>(composite_new - composite)
                           / current_temp);

      debugPrint(logger,
                 RMP,
                 "annealing",
                 1,
                 "current composite={:.6e}, new={:.6e} (WNS {}), accept_prob={}",
                 composite,
                 composite_new,
                 worst_slack_new,
                 accept_prob);

      if (absl::Uniform<float>(random_, 0, 1) >= accept_prob) {
        debugPrint(logger,
                   RMP,
                   "annealing",
                   1,
                   "Rejecting worse solution (composite={:.6e})",
                   composite_new);
        ++iters_since_best;
        continue;
      }
      debugPrint(logger,
                 RMP,
                 "annealing",
                 1,
                 "Accepting worse solution (composite={:.6e})",
                 composite_new);
    } else {
      debugPrint(logger,
                 RMP,
                 "annealing",
                 1,
                 "current composite={:.6e}, new={:.6e} (WNS {}), accepting",
                 composite,
                 composite_new,
                 worst_slack_new);
    }

    // ----- accepted: update current state -----
    ops         = std::move(new_ops);
    composite   = composite_new;
    worst_slack = worst_slack_new;

    // ----- update best-known if strictly better in composite -----
    if (composite > best_composite) {
      best_composite   = composite;
      best_worst_slack = worst_slack;
      best_ops         = ops;
      iters_since_best = 0;
      debugPrint(logger,
                 RMP,
                 "annealing",
                 1,
                 "New best: composite={:.6e}, WNS={}, {}",
                 best_composite,
                 best_worst_slack,
                 sol_slack.timing_metrics_.toString());
    } else {
      ++iters_since_best;
    }
  }

  // Publish the distinct-recipe shortlist for OptimizeDesign's final
  // repair-and-select pass — ALWAYS, even when the cheap pass found nothing
  // better than the (already-repaired) baseline.  In tiered mode every kCheap
  // candidate is unrepaired while the baseline is post-repair, so SA routinely
  // "finds no improvement" even though some of these recipes beat the baseline
  // AFTER repair (kCheap-worst candidates carry the most repair headroom —
  // pre-repair-TNS vs repair-gain correlation = -0.67).  OptimizeDesign
  // repair-evaluates the shortlist and only applies a winner that actually
  // beats the post-repair baseline.
  top_k_recipes_ = topk.Ranked();

  // -------------------------------------------------------------------------
  // Step 4: final guard — if SA never beat the baseline in the CHEAP pass.
  //
  // Legacy (final_topk_ <= 1): return empty ops so the caller leaves the
  // design alone.  Top-K (final_topk_ > 1): defer the decision to
  // OptimizeDesign, which repair-evaluates top_k_recipes_ and applies a winner
  // only if it beats the post-repair baseline.  We still return empty best_ops
  // here (no cheap-pass winner); OptimizeDesign keys off the shortlist instead.
  // -------------------------------------------------------------------------
  if (best_composite <= effective_baseline) {
    logger->info(RMP,
                 61,
                 "Simulated annealing complete. No cheap-pass improvement over "
                 "baseline (baseline={:.6e}, best={:.6e}).{}",
                 effective_baseline,
                 best_composite,
                 final_topk_ > 1 ? " Deferring to top-K repair-and-select."
                                 : " Returning no-op.");
    last_best_composite_ = std::numeric_limits<double>::quiet_NaN();
    if (final_topk_ <= 1) {
      top_k_recipes_.clear();
    }
    return empty_ops;
  }

  logger->info(RMP, 55,
               "Simulated annealing complete. Best score={:.6e}, "
               "best WNS={} (effective_baseline={:.6e})",
               best_composite, best_worst_slack, effective_baseline);

  // Tiered/full mode: single kFull re-eval of best.
  // Skipped when top-K is active (final_topk_ > 1) — OptimizeDesign re-scores
  // every shortlisted recipe under kFull.  Also skipped in cone_sta mode
  // (top-K always active; cone score is not comparable to kFull composite).
  if (tiered_sa && !best_ops.empty() && final_topk_ <= 1 && !cone_sta) {
    SolutionSlack best_sol{best_ops};
    auto [ws_full, req_ig] = best_sol.Evaluate(candidate_vertices,
                                               abc_library, corner_, sta,
                                               name_generator, resizer, logger,
                                               EvalTier::kFull);
    logger->info(RMP, 88,
                 "SA tiered: kFull re-eval of best → composite={:.6e} WNS={}",
                 best_sol.composite_score_, ws_full);
    last_best_composite_ = best_sol.composite_score_;
  } else {
    last_best_composite_ = best_composite;
  }
  return best_ops;
}

}  // namespace rmp