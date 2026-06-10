// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#pragma once

#include <optional>
#include <random>
#include <vector>

#include "aig/gia/gia.h"
#include "cut/abc_library_factory.h"
#include "db_sta/dbSta.hh"
#include "gia.h"
#include "resynthesis_strategy.h"
#include "rsz/Resizer.hh"
#include "slack_tuning_strategy.h"
#include "sta/Delay.hh"
#include "sta/Graph.hh"
#include "utl/Logger.h"
#include "utl/unique_name.h"

namespace sta {
class Scene;
}  // namespace sta

namespace rmp {

// Monte Carlo Tree Search strategy for GIA op-sequence exploration.
//
// Replaces SA's flat random-walk with a tree-structured search where each
// node represents a unique prefix of GIA ops and UCB1 concentrates the
// evaluation budget on prefixes that have historically produced good timing.
//
// Evaluation model (AlphaZero-style): there is NO cheap rollout.
// SolutionSlack::Evaluate (RunGia + DPL + repair_setup) is the value
// function — one full pipeline call per MCTS iteration, same budget as SA.
// This is correct because timing can only be measured after mapping,
// placement, and repair; there is no cheap intermediate-depth proxy.
//
// UCB1 node value uses MAX backpropagation (not mean): a node's value is
// the best outcome reachable through it, matching how SA tracks best_composite.
class MctsStrategy final : public SlackTuningStrategy
{
 public:
  // eval_mode: "full"   → kFull every iteration (accurate, current default)
  //            "tiered" → kCheap all iters + kFull top-5 tier-up (~5-7x faster)
  explicit MctsStrategy(sta::Scene*                     corner,
                        sta::Slack                      slack_threshold,
                        std::mt19937::result_type       seed,
                        std::optional<float>            ucb_constant,
                        unsigned                        iterations,
                        std::optional<unsigned>         max_depth,
                        unsigned                        initial_ops,
                        float                           percentage = 100.0f,
                        float                           wns_pct    = 10.0f,
                        std::string                     eval_mode  = "full")
      : SlackTuningStrategy(corner,
                            slack_threshold,
                            seed,
                            iterations,
                            initial_ops,
                            percentage,
                            wns_pct),
        ucb_constant_(ucb_constant),
        max_depth_(max_depth),
        eval_mode_(std::move(eval_mode))
  {
  }

  std::vector<GiaOp> RunStrategy(
      const std::vector<GiaOp>&        all_ops,
      const std::vector<sta::Vertex*>& candidate_vertices,
      cut::AbcLibrary&                 abc_library,
      sta::dbSta*                      sta,
      utl::UniqueName&                 name_generator,
      rsz::Resizer*                    resizer,
      utl::Logger*                     logger) override;

 private:
  std::optional<float>    ucb_constant_;
  std::optional<unsigned> max_depth_;
  std::string             eval_mode_ = "full";  // "full" or "tiered"
};

}  // namespace rmp
