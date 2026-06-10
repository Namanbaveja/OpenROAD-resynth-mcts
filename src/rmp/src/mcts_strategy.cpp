// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#include "mcts_strategy.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "absl/hash/hash.h"
#include "absl/random/distributions.h"
#include "cut/abc_library_factory.h"
#include "db_sta/dbSta.hh"
#include "gia.h"
#include "slack_tuning_strategy.h"
#include "sta/Delay.hh"
#include "sta/Graph.hh"
#include "utils.h"
#include "utl/Logger.h"
#include "utl/unique_name.h"

namespace rmp {
using utl::RMP;

namespace {

// ---------------------------------------------------------------------------
// MCTS tree node.
//
// Each node represents a unique prefix of GIA ops (the path from the root).
// The root node has an empty prefix (the design as-is, before any op).
//
// untried_op_ids: indices into all_ops that have NOT yet been expanded as
//   children of this node.  When empty, the node is fully expanded and UCB
//   selects among existing children.  We reset to {0..N-1} per node because
//   sequences allow repetition and order matters — every op is a valid next
//   step regardless of which op was taken to reach this node.
//
// best_score: maximum composite score seen across all Evaluate calls that
//   descended through this node.  MAX backprop (not mean) mirrors how SA
//   tracks best_composite — we want the best achievable outcome, not the
//   average over early exploratory visits.
// ---------------------------------------------------------------------------
struct MctsNode
{
  std::vector<GiaOp>                     prefix;
  MctsNode*                              parent        = nullptr;
  int                                    action_op_idx = -1;
  std::vector<std::unique_ptr<MctsNode>> children;
  std::vector<size_t>                    untried_op_ids;
  int                                    visits        = 0;
  double best_score  = -std::numeric_limits<double>::max();
  double total_score = 0.0;  // sum of all composite scores through this node
};

// UCB1 child selection with score normalization.
//
// Composite scores live in seconds (~-4e-10), making the raw exploit term
// 10 orders of magnitude smaller than the explore term (C×√... ≈ 1–3).
// Without normalization UCB degenerates to pure BFS — every child gets the
// same UCB regardless of its timing quality.
//
// Fix: normalize the exploit term relative to the baseline composite so it
// lives in [-1, +1], comparable to the explore term with C=√2.
//
//   exploit = (mean_score - baseline) / |baseline|
//   explore = C × √(ln N_parent / N_child)
//
// Mean score (total_score / visits) is used instead of best_score so that
// subtrees which got one lucky result early are not permanently favoured
// over subtrees that are consistently good.
MctsNode* UCBSelect(MctsNode* node, float C, double baseline)
{
  MctsNode* best_child = nullptr;
  double    best_ucb   = -std::numeric_limits<double>::max();

  // Avoid division by zero if baseline is exactly 0.
  const double norm = std::abs(baseline) + 1e-30;

  for (auto& child : node->children) {
    if (child->visits == 0) {
      return child.get();
    }
    const double mean_score = child->total_score / child->visits;
    const double exploit    = (mean_score - baseline) / norm;
    const double explore
        = static_cast<double>(C)
          * std::sqrt(std::log(static_cast<double>(node->visits))
                      / static_cast<double>(child->visits));
    const double ucb = exploit + explore;
    if (ucb > best_ucb) {
      best_ucb   = ucb;
      best_child = child.get();
    }
  }
  return best_child;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// MctsStrategy::RunStrategy
//
// Overall algorithm (AlphaZero-style, no rollout):
//
//   for each iteration (= one Evaluate call = one tree path):
//     1. Selection   — descend via UCB until we reach a node with untried ops
//                      or a terminal (prefix.size() == max_depth).
//     2. Expansion   — pick one untried op, append it, create a new child.
//     3. Simulation  — call SolutionSlack::Evaluate on the child's prefix.
//                      This IS the value; there is no separate rollout.
//     4. Backprop    — walk root→leaf, update visits and best_score.
//
// Hard WNS guard, ECO journal, DPL, repair_setup, and baseline measurement
// are all delegated to the same infrastructure SA uses — no new mechanics.
// ---------------------------------------------------------------------------
std::vector<GiaOp> MctsStrategy::RunStrategy(
    const std::vector<GiaOp>&        all_ops,
    const std::vector<sta::Vertex*>& candidate_vertices,
    cut::AbcLibrary&                 abc_library,
    sta::dbSta*                      sta,
    utl::UniqueName&                 name_generator,
    rsz::Resizer*                    resizer,
    utl::Logger*                     logger)
{
  // -------------------------------------------------------------------------
  // Step 1: baseline — read ORIGINAL design state without RunGia.
  //
  // Matches AnnealingStrategy: EvaluateCurrentState avoids the destructive
  // Abc_NtkMap re-mapping that Evaluate(empty_ops) would trigger.
  // -------------------------------------------------------------------------
  debugPrint(logger,
             RMP,
             "mcts",
             1,
             "Evaluating baseline from current (original) design state");

  SolutionSlack baseline_slack;
  baseline_slack.EvaluateCurrentState(sta, corner_);
  const double     baseline_composite = baseline_slack.composite_score_;
  const sta::Slack baseline_wns_value = baseline_slack.timing_metrics_.wns;

  logger->info(RMP,
               70,
               "MCTS baseline: composite={:.6e}, {}",
               baseline_composite,
               baseline_slack.timing_metrics_.toString());

  // -------------------------------------------------------------------------
  // Step 2: parameters
  // -------------------------------------------------------------------------
  constexpr float kDefaultUcbConstant = 1.41421356f;  // √2 — canonical UCT
  const float     ucb_constant = ucb_constant_.value_or(kDefaultUcbConstant);

  // C annealing: start at ucb_constant (exploration), decay to ucb_constant/6
  // (exploitation) over the full run.  When all depth-max leaves have been
  // seen, the exploit term needs to win over explore so UCB concentrates
  // budget on the best subtrees rather than round-robining them.
  // Schedule: C(t) = C_start * (1 - 5/6 * t/T)  → C_start at t=0, C_start/6 at t=T.
  const float ucb_c_start = ucb_constant;
  const float ucb_c_end   = ucb_constant / 6.0f;

  // max_depth: maximum GIA op sequence length per tree path.
  // Deeper trees allow more complex restructurings; UCB with C-annealing
  // concentrates budget on the most promising subtrees so depth-4+ nodes
  // do get evaluated when their shallower parents score well.
  const unsigned max_depth = max_depth_.value_or(6u);

  // Hard WNS guard — identical to AnnealingStrategy.
  // spef_wns_baseline_ is set by SlackTuningStrategy::OptimizeDesign before
  // RunStrategy is called, so it always reflects the pre-SA SPEF WNS.
  const sta::Slack wns_guard_ref
      = std::min(spef_wns_baseline_, baseline_wns_value);
  const sta::Slack max_wns_regression_abs = std::abs(wns_guard_ref) * 10.00f;

  logger->info(RMP,
               71,
               "MCTS: iterations={}, max_depth={}, ucb_constant={:.4f}, "
               "all_ops={}, wns_guard_ref={}",
               iterations_,
               max_depth,
               ucb_constant,
               all_ops.size(),
               wns_guard_ref);

  // -------------------------------------------------------------------------
  // Step 3: initialise root (empty prefix = no GIA ops applied)
  // -------------------------------------------------------------------------
  auto root = std::make_unique<MctsNode>();
  root->untried_op_ids.resize(all_ops.size());
  std::iota(root->untried_op_ids.begin(), root->untried_op_ids.end(), 0);

  double             best_composite    = baseline_composite;
  sta::Slack         best_worst_slack  = baseline_wns_value;
  TimingMetrics      best_metrics      = baseline_slack.timing_metrics_;
  std::vector<GiaOp> best_ops;  // empty = no improvement over baseline

  // Evaluation cache: prefix → {composite, worst_slack}.
  // MCTS re-visits leaf nodes after the tree is fully expanded; without a
  // cache every re-visit burns a full cheap eval.  Cache lets us reuse
  // the result and spend the budget on backprop only.
  using PrefixKey = std::vector<size_t>;
  struct CacheVal { double composite; sta::Slack worst_slack; TimingMetrics metrics; };
  std::unordered_map<PrefixKey, CacheVal, absl::Hash<PrefixKey>> eval_cache;
  unsigned cache_hits = 0;

  // "full"   → kFull every iteration (repair_setup each call).
  // "tiered" → kCheap all iters (no repair_setup) + kFull top-5 tier-up.
  const bool tiered = (eval_mode_ == "tiered");
  constexpr size_t kTierUpK = 5;
  struct CandidateEntry { double cheap_composite; std::vector<GiaOp> ops; };
  std::vector<CandidateEntry> unique_candidates;

  // -------------------------------------------------------------------------
  // Restart mechanism: when no improvement for kMctsStallLimit consecutive
  // iterations, reset the MCTS tree to a fresh root and continue searching.
  // The global best solution is preserved across restarts.
  //
  // Rationale: benchmarking showed MCTS finds its best solution within the
  // first 10% of iterations then stalls (GCD: last improvement at iter 77/1000,
  // UART: iter 63/1000, I2C: iter 39/1000).  Without restarts, 90%+ of the
  // iteration budget is wasted on pure exploitation of one exhausted subtree.
  // With stall_limit=100, 1000 iters yields ~10 independent searches.
  //
  // OLD behaviour (no restarts) — to revert: set kMctsStallLimit = iterations_
  // -------------------------------------------------------------------------
  // Adaptive stall limit: ~3 restarts regardless of iteration budget.
  // 300 iters → stall at 100; 100 iters → stall at 33; 50 iters → stall at 16.
  const unsigned kMctsStallLimit = std::max(10u, iterations_ / 3);
  unsigned no_improve_streak = 0;
  unsigned restart_count     = 0;

  // -------------------------------------------------------------------------
  // Main MCTS loop — one Evaluate call per iteration
  // -------------------------------------------------------------------------
  for (unsigned iter = 0; iter < iterations_; ++iter) {

    // C annealing: linear decay from ucb_c_start to ucb_c_end.
    const float t_frac = static_cast<float>(iter)
                         / static_cast<float>(std::max(1u, iterations_ - 1));
    const float c_now  = ucb_c_start + (ucb_c_end - ucb_c_start) * t_frac;

    // ----- Selection: descend via UCB until we can expand -----
    MctsNode* node = root.get();
    while (node->untried_op_ids.empty()
           && !node->children.empty()
           && node->prefix.size() < max_depth) {
      node = UCBSelect(node, c_now, baseline_composite);
      assert(node != nullptr);
    }

    // ----- Expansion: create one new child from an untried op -----
    if (!node->untried_op_ids.empty()
        && node->prefix.size() < max_depth) {
      // Random selection among untried ops avoids systematic bias toward
      // lower-indexed ops on the first pass through the tree.
      const size_t pick_pos
          = absl::Uniform<size_t>(random_, 0, node->untried_op_ids.size());
      const size_t op_idx = node->untried_op_ids[pick_pos];
      node->untried_op_ids.erase(node->untried_op_ids.begin() + pick_pos);

      auto child            = std::make_unique<MctsNode>();
      child->parent         = node;
      child->action_op_idx  = static_cast<int>(op_idx);
      child->prefix         = node->prefix;
      child->prefix.push_back(all_ops[op_idx]);
      // Populate untried ops only if the child is not yet at max depth.
      // Leaf nodes (prefix.size() == max_depth) can never be expanded;
      // leaving untried_op_ids empty prevents the expansion block from
      // firing on them in subsequent iterations and wasting eval budget
      // by re-evaluating the same prefix repeatedly.
      if (child->prefix.size() < max_depth) {
        child->untried_op_ids.resize(all_ops.size());
        std::iota(child->untried_op_ids.begin(),
                  child->untried_op_ids.end(),
                  0);
      }
      node = child.get();
      node->parent->children.push_back(std::move(child));
    }

    // Guard: if we are back at root with nothing to expand (only reachable
    // when max_depth==0, which is prevented above, or a degenerate budget).
    if (node == root.get()
        && node->untried_op_ids.empty()
        && node->children.empty()) {
      debugPrint(logger,
                 RMP,
                 "mcts",
                 1,
                 "Iter {}: tree fully explored — stopping",
                 iter + 1);
      break;
    }

    // ----- Simulation = Evaluate (with cache) -----
    // Build the prefix key (op IDs only — deterministic, design-independent).
    PrefixKey prefix_key;
    prefix_key.reserve(node->prefix.size());
    for (const auto& op : node->prefix) {
      prefix_key.push_back(op.id);
    }

    double        composite_new;
    sta::Slack    worst_slack_new;
    TimingMetrics metrics_new;

    auto cache_it = eval_cache.find(prefix_key);
    if (cache_it != eval_cache.end()) {
      composite_new   = cache_it->second.composite;
      worst_slack_new = cache_it->second.worst_slack;
      metrics_new     = cache_it->second.metrics;
      ++cache_hits;
      debugPrint(logger, RMP, "mcts", 1,
                 "Iter {}: cache hit prefix len={} composite={:.6e}",
                 iter + 1, prefix_key.size(), composite_new);
    } else {
      // tiered: kCheap (no repair_setup, fast proxy score).
      // full:   kFull  (repair_setup every iter, accurate score).
      const EvalTier tier = tiered ? EvalTier::kCheap : EvalTier::kFull;
      SolutionSlack sol{node->prefix};
      auto [ws, req_ignore]
          = sol.Evaluate(candidate_vertices, abc_library, corner_, sta,
                         name_generator, resizer, logger, tier);
      worst_slack_new = ws;
      composite_new   = sol.composite_score_;
      metrics_new     = sol.timing_metrics_;
      eval_cache[prefix_key] = {composite_new, worst_slack_new, metrics_new};
      if (tiered) {
        unique_candidates.push_back({composite_new, node->prefix});
      }
    }

    // Hard WNS guard — same logic as AnnealingStrategy.
    // Backprop visit count without updating best_score so UCB naturally
    // de-prioritises this subtree (exploit term stays at -max).
    if (worst_slack_new < wns_guard_ref - max_wns_regression_abs) {
      debugPrint(logger,
                 RMP,
                 "mcts",
                 1,
                 "Iter {}: rejecting catastrophic WNS regression: "
                 "WNS_new={} (guard_ref={}, limit=-{})",
                 iter + 1,
                 worst_slack_new,
                 wns_guard_ref,
                 max_wns_regression_abs);
      MctsNode* bp = node;
      while (bp != nullptr) {
        ++bp->visits;
        bp->total_score += baseline_composite;  // treat as baseline (no improvement)
        bp = bp->parent;
      }
      continue;
    }

    // ----- Backpropagation: visits++, total_score sum, and MAX update -----
    {
      MctsNode* bp = node;
      while (bp != nullptr) {
        ++bp->visits;
        bp->total_score += composite_new;
        if (composite_new > bp->best_score) {
          bp->best_score = composite_new;
        }
        bp = bp->parent;
      }
    }

    // Periodic progress log (every 10 iters).
    if ((iter + 1) % 10 == 0) {
      logger->info(RMP,
                   72,
                   "MCTS iter {}/{}: "
                   "WNS={:.2f} ps  TNS={:.2f} ps  VEP={}  "
                   "composite={:.6e}  depth={} cache={}/{} C={:.3f}",
                   iter + 1,
                   iterations_,
                   static_cast<double>(best_metrics.wns) * 1e12,
                   static_cast<double>(best_metrics.tns) * 1e12,
                   best_metrics.violating_count,
                   best_composite,
                   node->prefix.size(),
                   cache_hits,
                   iter + 1,
                   c_now);

      // Every 50 iters: dump root-child visit distribution to verify UCB.
      // If visits are roughly equal across children, UCB is degenerate (BFS).
      // Healthy UCB concentrates visits on 1-3 promising subtrees.
      if ((iter + 1) % 50 == 0 && !root->children.empty()) {
        std::ostringstream dist;
        for (const auto& ch : root->children) {
          const double mean = ch->visits > 0
                                  ? ch->total_score / ch->visits
                                  : baseline_composite;
          dist << " op" << ch->action_op_idx << "=v" << ch->visits
               << "(m=" << mean << ")";
        }
        debugPrint(logger,
                   RMP,
                   "mcts",
                   1,
                   "iter {}: root visit dist:{}",
                   iter + 1,
                   dist.str());
      }
    }

    debugPrint(logger,
               RMP,
               "mcts",
               1,
               "Iter {}: composite={:.6e} WNS={} ops_len={} C={:.3f}",
               iter + 1,
               composite_new,
               worst_slack_new,
               node->prefix.size(),
               c_now);

    // ----- Update global best -----
    if (composite_new > best_composite) {
      best_composite    = composite_new;
      best_worst_slack  = worst_slack_new;
      best_metrics      = metrics_new;
      best_ops          = node->prefix;
      no_improve_streak = 0;
      debugPrint(logger,
                 RMP,
                 "mcts",
                 1,
                 "New best: composite={:.6e} WNS={} prefix_len={} "
                 "(restart#{})",
                 best_composite,
                 best_worst_slack,
                 node->prefix.size(),
                 restart_count);
    } else {
      ++no_improve_streak;
    }

    // ----- Restart: reset tree when stalled -----
    // When no improvement for kMctsStallLimit consecutive iterations, discard
    // the current tree and start fresh.  The eval cache and global best are
    // preserved — new searches benefit from cached evaluations and compare
    // against the same baseline so the best solution is never lost.
    //
    // OLD behaviour (no restarts):
    // — this block did not exist; no_improve_streak was never checked.
    if (no_improve_streak >= kMctsStallLimit) {
      ++restart_count;
      logger->info(RMP,
                   84,
                   "MCTS restart #{}: no improvement for {} iters "
                   "(iter {}/{}).  Resetting tree, keeping best "
                   "composite={:.6e}.",
                   restart_count,
                   kMctsStallLimit,
                   iter + 1,
                   iterations_,
                   best_composite);
      root = std::make_unique<MctsNode>();
      root->untried_op_ids.resize(all_ops.size());
      std::iota(root->untried_op_ids.begin(), root->untried_op_ids.end(), 0);
      no_improve_streak = 0;
    }
  }

  // -------------------------------------------------------------------------
  // ---- Step 4: tier-up (tiered mode only) ---------------------------------
  if (tiered && !unique_candidates.empty()) {
    const size_t tier_k = std::min(kTierUpK, unique_candidates.size());
    std::partial_sort(
        unique_candidates.begin(),
        unique_candidates.begin() + static_cast<std::ptrdiff_t>(tier_k),
        unique_candidates.end(),
        [](const CandidateEntry& a, const CandidateEntry& b) {
          return a.cheap_composite > b.cheap_composite;
        });
    logger->info(RMP, 86,
                 "Tier-up: re-evaluating top-{} candidates with kFull "
                 "from {} unique cheap evals",
                 tier_k, unique_candidates.size());
    for (size_t i = 0; i < tier_k; ++i) {
      SolutionSlack sol{unique_candidates[i].ops};
      auto [ws_full, req_ig]
          = sol.Evaluate(candidate_vertices, abc_library, corner_, sta,
                         name_generator, resizer, logger, EvalTier::kFull);
      if (ws_full < wns_guard_ref - max_wns_regression_abs) continue;
      const double fc = sol.composite_score_;
      debugPrint(logger, RMP, "mcts", 1,
                 "Tier-up {}/{}: cheap={:.6e} full={:.6e} WNS={}",
                 i+1, tier_k, unique_candidates[i].cheap_composite, fc, ws_full);
      if (fc > best_composite) {
        best_composite   = fc;
        best_worst_slack = ws_full;
        best_metrics     = sol.timing_metrics_;
        best_ops         = unique_candidates[i].ops;
        logger->info(RMP, 87, "Tier-up new best: composite={:.6e} WNS={} (candidate {}/{})",
                     best_composite, best_worst_slack, i+1, tier_k);
      }
    }
  }

  // Step 5: final guard — identical to AnnealingStrategy.
  // If MCTS never beat the baseline, return empty_ops so OptimizeDesign
  // leaves the design untouched (pre-SA repair_setup changes are kept).
  // -------------------------------------------------------------------------
  logger->info(RMP,
               85,
               "MCTS cache: {} hits / {} iterations ({:.0f}%) — unique "
               "prefixes evaluated: {}  restarts: {}",
               cache_hits,
               iterations_,
               100.0 * cache_hits / std::max(1u, iterations_),
               eval_cache.size(),
               restart_count);

  if (best_composite <= baseline_composite) {
    logger->info(
        RMP,
        73,
        "MCTS complete. No improvement over baseline "
        "(baseline composite={:.6e}, best={:.6e}); returning no-op.",
        baseline_composite,
        best_composite);
    last_best_composite_ = std::numeric_limits<double>::quiet_NaN();
    return {};
  }

  logger->info(RMP,
               74,
               "MCTS complete. Best composite={:.6e}, best WNS={} "
               "(baseline composite={:.6e}, baseline WNS={})",
               best_composite,
               best_worst_slack,
               baseline_composite,
               baseline_wns_value);

  last_best_composite_ = best_composite;
  return best_ops;
}

}  // namespace rmp
