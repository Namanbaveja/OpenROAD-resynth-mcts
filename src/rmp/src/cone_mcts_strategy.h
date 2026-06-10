// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#pragma once

#include <limits>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include "cone_manager.h"
#include "cut/abc_library_factory.h"
#include "gia.h"
#include "sta/Delay.hh"
#include "utl/unique_name.h"

namespace rsz {
class Resizer;
}

namespace sta {
class dbSta;
class Scene;
}  // namespace sta

namespace utl {
class Logger;
}

namespace rmp {

// Multi-cone MCTS strategy for post-PnR logic restructuring.
//
// Algorithm:
//   1. Extract all violating endpoints (no top-N% filter).
//   2. One endpoint → one cone (BFS-backward LogicCut); cones exceeding
//      max_instances_per_cone are skipped.
//   3. For each cone, run MCTS independently with RunGiaConeSlack as reward
//      (pure-ABC, no ODB writes, ~50× faster than SolutionSlack::Evaluate).
//   4. Apply the best-found op sequence for every cone (sequential ODB commit).
//   5. DPL to legalize newly inserted instances.
//   6. repair_setup once on the fully stitched design.
//   7. Report final WNS / TNS.
//
// The key difference from resynth_mcts (MctsStrategy):
//   • ALL violating cones are extracted BEFORE any ODB commit → cone timing
//     context is consistent for the entire MCTS phase.
//   • Reward = ABC internal delay (RunGiaConeSlack), not full STA.
//   • repair_setup runs once at the end, not inside the eval loop.
class ConeMctsStrategy
{
 public:
  ConeMctsStrategy(sta::Scene*      corner,
                   sta::Slack       slack_threshold,
                   unsigned         n_iters,
                   int              max_depth,
                   int              bfs_hops,
                   int              max_instances_per_cone,
                   unsigned         mcts_seed      = 0,
                   float            ucb_constant   = 1.414f);

  void OptimizeDesign(sta::dbSta*      sta,
                      utl::UniqueName& name_generator,
                      rsz::Resizer*    resizer,
                      utl::Logger*     logger);

  // Run n_iters_ MCTS iterations for a single cone.
  // Returns the GIA op sequence that produced the best RunGiaConeSlack reward.
  // Public so it can be called from ConeCpaRemapStrategy.
  std::vector<GiaOp> OptimizeCone(const ConeData&           cone,
                                   const std::vector<GiaOp>& all_ops,
                                   cut::AbcLibrary&          abc_library,
                                   sta::dbSta*               sta,
                                   utl::Logger*              logger);

 private:
  // MCTS tree node.  MAX backpropagation: best_reward = best slack seen
  // through this node.  Unvisited nodes have visits==0 and get infinite UCT.
  struct MctsNode
  {
    double best_reward = -std::numeric_limits<double>::max();
    int    visits      = 0;
    std::unordered_map<size_t, std::unique_ptr<MctsNode>> children;
  };

  sta::Scene*  corner_;
  sta::Slack   slack_threshold_;
  unsigned     n_iters_;
  int          max_depth_;
  int          bfs_hops_;
  int          max_instances_per_cone_;
  float        ucb_constant_;
  std::mt19937 rng_;
};

}  // namespace rmp
