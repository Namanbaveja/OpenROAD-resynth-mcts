// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#include "improved_annealing_strategy.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

#include "absl/random/distributions.h"
#include "cut/abc_library_factory.h"
#include "db_sta/dbSta.hh"
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
#include "utils.h"
#include "utl/Logger.h"
#include "utl/deleter.h"
#include "utl/unique_name.h"

namespace rmp {
using utl::RMP;

std::vector<GiaOp> ImprovedAnnealingStrategy::RunStrategy(
    const std::vector<GiaOp>& all_ops,
    const std::vector<sta::Vertex*>& candidate_vertices,
    cut::AbcLibrary& abc_library,
    sta::dbSta* sta,
    utl::UniqueName& name_generator,
    rsz::Resizer* resizer,
    utl::Logger* logger)
{
  // SA-4: Non-adjacent swap — any two distinct positions instead of only (i, i+1)
  // RNG is passed in so calibration can use a dedicated generator independent
  // of the main-loop RNG.
  auto randomNeighbor = [&](const std::vector<GiaOp>& sol,
                            std::mt19937& rng) -> std::vector<GiaOp> {
    auto result = sol;
    enum class Move : uint8_t { kAdd, kRemove, kSwap, kCount };
    Move move = Move::kAdd;
    if (result.size() > 1) {
      move = Move(absl::Uniform<int>(rng, 0, static_cast<int>(Move::kCount)));
    }
    switch (move) {
      case Move::kAdd: {
        size_t i = absl::Uniform<size_t>(rng, 0, result.size() + 1);
        size_t j = absl::Uniform<size_t>(rng, 0, all_ops.size());
        result.insert(result.begin() + i, all_ops[j]);
      } break;
      case Move::kRemove: {
        size_t i = absl::Uniform<size_t>(rng, 0, result.size());
        result.erase(result.begin() + i);
      } break;
      case Move::kSwap: {
        // SA-4: pick any two distinct positions — C(n,2) pairs vs n-1 for adjacent-only
        size_t i = absl::Uniform<size_t>(rng, 0, result.size());
        size_t j;
        do {
          j = absl::Uniform<size_t>(rng, 0, result.size());
        } while (j == i);
        std::swap(result[i], result[j]);
      } break;
      case Move::kCount:
        break;
    }
    return result;
  };

  auto makeInitOps = [&](std::mt19937& rng) -> std::vector<GiaOp> {
    std::vector<GiaOp> ops;
    ops.reserve(initial_ops_);
    for (size_t i = 0; i < initial_ops_; i++) {
      ops.push_back(all_ops[absl::Uniform<size_t>(rng, 0, all_ops.size())]);
    }
    return ops;
  };

  // V3 calibration runs in a dedicated RNG so the main-loop RNG state is
  // independent of whether calibration ran — the explicit -temp path and
  // the auto path explore the same neighbor sequence for a given seed.
  std::mt19937 calib_rng(random_());
  if (!temperature_) {
    // V3 calibration — median uphill |Δslack| with regime-adaptive scaling.
    // V2 used mean-of-all-deltas / ln(0.8) which (a) inflated by improvement
    // spikes and (b) ignored that deeply-violated designs (GCD: |WNS|≈430ps,
    // typical step≈50ps) want near-greedy descent while near-closure designs
    // (AES: |WNS|≈30ps) want moderate exploration. Empirical signature: GCD
    // with manual T₀=5ps reached 89% improvement, V2 auto T₀≈200ps only 75%.
    //
    // V3 uses two robust signals:
    //   median_delta   = median |Δslack| of *uphill* (worsening) neighbors
    //                    (fallback: median over all if <5 uphill samples)
    //   ratio          = |ref_slack| / median_delta — violation depth in
    //                    units of typical step. ratio≥5 → near-greedy regime;
    //                    ratio≤2 → exploratory regime; linear in between.
    //   T₀ = median_delta × factor(ratio)
    //   At T₀=median_delta×1.0, P(accept median worsening move) = exp(-1)=37%
    //   At T₀=median_delta×0.2, P(accept median worsening move) = exp(-5)=0.7%
    debugPrint(logger,
               RMP,
               "improved_annealing",
               1,
               "Calibrating initial temperature from circuit response");
    auto ref_ops = makeInitOps(calib_rng);
    auto ref_eval = SolutionSlack{ref_ops}.Evaluate(
        candidate_vertices, abc_library, corner_, sta, name_generator, resizer, logger);
    sta::Slack ref_slack = ref_eval.first;
    sta::Delay required = ref_eval.second;

    constexpr int kCalibSamples = 20;
    std::vector<float> uphill_deltas;
    std::vector<float> all_deltas;
    uphill_deltas.reserve(kCalibSamples);
    all_deltas.reserve(kCalibSamples);
    for (int c = 0; c < kCalibSamples; c++) {
      auto nb_ops = randomNeighbor(ref_ops, calib_rng);
      auto nb_eval = SolutionSlack{nb_ops}.Evaluate(
          candidate_vertices, abc_library, corner_, sta, name_generator, resizer, logger);
      float delta = static_cast<float>(nb_eval.first)
                    - static_cast<float>(ref_slack);
      all_deltas.push_back(std::fabs(delta));
      if (delta < 0.0f) {  // uphill = worsening slack
        uphill_deltas.push_back(-delta);
      }
    }
    std::vector<float>* deltas
        = (uphill_deltas.size() >= 5) ? &uphill_deltas : &all_deltas;
    std::sort(deltas->begin(), deltas->end());
    float median_delta = deltas->empty()
                             ? static_cast<float>(required)
                             : (*deltas)[deltas->size() / 2];

    float ratio = (median_delta > 0.0f)
                      ? std::fabs(static_cast<float>(ref_slack)) / median_delta
                      : 0.0f;
    float t0_factor;
    if (ratio >= 5.0f) {
      t0_factor = 0.2f;
    } else if (ratio <= 2.0f) {
      t0_factor = 1.0f;
    } else {
      t0_factor = 1.0f - 0.8f * (ratio - 2.0f) / 3.0f;
    }
    temperature_ = std::max(median_delta * t0_factor,
                            std::numeric_limits<float>::min());
  }

  logger->info(RMP, 75, "Resynthesis (improved): starting simulated annealing");
  logger->info(RMP, 76, "Initial temperature: {}", *temperature_);

  // V3: single run. V2's multi-start with ILS-perturbed restarts caused
  // E4 (revert_after) to drop from 75% → 0% on GCD: for seed=42, all four
  // revert_after configurations converged to the same -105ps local optimum
  // because perturbing the global best by initial_ops_ random mutations
  // rarely escaped the basin. With V3's regime-adaptive T₀ the single shot
  // is more reliable, and removing forced restarts eliminates the lock-in.
  constexpr unsigned num_restarts = 1;
  const unsigned iters_per_restart = iterations_;
  // V3: auto-revert defaults to iters/4. Without an automatic floor,
  // geometric cooling can drift into deeper-violation regions (observed:
  // GCD slow corner -649ps vs -430ps baseline) and never recover. Explicit
  // -revert_after still overrides.
  const unsigned revert_threshold
      = revert_after_.value_or(std::max(5u, iters_per_restart / 4));

  std::vector<GiaOp> global_best_ops;
  sta::Slack global_best_slack = std::numeric_limits<sta::Slack>::lowest();

  for (unsigned restart = 0; restart < num_restarts; restart++) {
    // SA-7+: Iterated Local Search — restart >0 begins from a perturbation of
    // the best solution found so far instead of a fresh random draw, so that
    // useful structural information from earlier restarts is preserved while
    // injecting enough randomness to escape local minima.  Pure random
    // restarts discard everything learned and can fail repeatedly on
    // unlucky seeds (e.g., GCD seed=1234).
    std::vector<GiaOp> ops;
    if (restart > 0 && !global_best_ops.empty()) {
      ops = global_best_ops;
      const size_t kPerturb = std::max(initial_ops_, static_cast<unsigned>(1));
      for (size_t p = 0; p < kPerturb; p++) {
        ops = randomNeighbor(ops, random_);
      }
    } else {
      ops = makeInitOps(random_);
    }
    sta::Slack worst_slack = SolutionSlack{ops}
                                 .Evaluate(candidate_vertices,
                                           abc_library,
                                           corner_,
                                           sta,
                                           name_generator,
                                           resizer,
                                           logger)
                                 .first;

    sta::Slack best_worst_slack = worst_slack;
    auto best_ops = ops;
    size_t worse_iters = 0;

    // V3: fixed 2-decade cooling — α = 0.01^(1/(N-1)).
    // N=30 → α=0.853; N=90 → α=0.949; N=150 → α=0.969.
    // The midpoint ratio T(N/2)/T₀ ≈ 0.094 is invariant across budgets.
    // V2's adaptive scaling produced inconsistent schedules: at N=30 it
    // stayed at 0.5-decade decay (T_final ≈ 0.32×T₀, still warm — wasted
    // late iters on random walk) which fought against the regime-adaptive
    // T₀ logic. A fixed 2-decade decay gives consistent warm/cold proportions.
    const float decay_alpha
        = std::pow(0.01f,
                   1.0f / static_cast<float>(std::max(1u, iters_per_restart - 1)));

    for (unsigned i = 0; i < iters_per_restart; i++) {
      float current_temp
          = *temperature_ * std::pow(decay_alpha, static_cast<float>(i));

      if (worse_iters >= revert_threshold) {
        logger->info(RMP, 77, "Reverting to the best found solution");
        ops = best_ops;
        worst_slack = best_worst_slack;
        worse_iters = 0;
      }

      if ((i + 1) % 10 == 0) {
        logger->info(RMP,
                     78,
                     "Restart {}/{}, iteration: {}, temperature: {}, "
                     "best worst slack: {}",
                     restart + 1,
                     num_restarts,
                     i + 1,
                     current_temp,
                     best_worst_slack);
      } else {
        debugPrint(logger,
                   RMP,
                   "improved_annealing",
                   1,
                   "Restart {}/{}, iteration: {}, temperature: {}, "
                   "best worst slack: {}",
                   restart + 1,
                   num_restarts,
                   i + 1,
                   current_temp,
                   best_worst_slack);
      }

      auto new_ops = randomNeighbor(ops, random_);
      sta::Slack worst_slack_new
          = SolutionSlack{new_ops}
                .Evaluate(candidate_vertices,
                           abc_library,
                           corner_,
                           sta,
                           name_generator,
                           resizer,
                           logger)
                .first;

      if (worst_slack_new < best_worst_slack) {
        worse_iters++;
      } else {
        worse_iters = 0;
      }

      if (worst_slack_new < worst_slack) {
        float accept_prob
            = current_temp == 0
                  ? 0.0f
                  : std::exp((worst_slack_new - worst_slack) / current_temp);
        debugPrint(logger,
                   RMP,
                   "improved_annealing",
                   1,
                   "Current worst slack: {}, new: {}, accepting new ABC script "
                   "with probability {}",
                   worst_slack,
                   worst_slack_new,
                   accept_prob);
        if (absl::Uniform<float>(random_, 0, 1) < accept_prob) {
          debugPrint(logger,
                     RMP,
                     "improved_annealing",
                     1,
                     "Accepting new ABC script with worse slack");
        } else {
          debugPrint(logger,
                     RMP,
                     "improved_annealing",
                     1,
                     "Rejecting new ABC script with worse slack");
          continue;
        }
      } else {
        debugPrint(logger,
                   RMP,
                   "improved_annealing",
                   1,
                   "Current worst slack: {}, new: {}, accepting new ABC script",
                   worst_slack,
                   worst_slack_new);
      }

      ops = std::move(new_ops);
      worst_slack = worst_slack_new;

      if (worst_slack > best_worst_slack) {
        best_worst_slack = worst_slack;
        best_ops = ops;
      }
    }

    // SA-7: track global best across restarts
    if (best_worst_slack > global_best_slack) {
      global_best_slack = best_worst_slack;
      global_best_ops = best_ops;
    }
  }

  return global_best_ops;
}

}  // namespace rmp
