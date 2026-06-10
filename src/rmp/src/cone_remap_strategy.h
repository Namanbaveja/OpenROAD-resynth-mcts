// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#pragma once

#include "cut/abc_library_factory.h"
#include "sta/Scene.hh"
#include "sta/Sta.hh"

namespace rsz {
class Resizer;
}
namespace utl {
class Logger;
class UniqueName;
}  // namespace utl

namespace rmp {

// ---------------------------------------------------------------------------
// ConeCpaRemapStrategy — CPA-remap paper approach.
//
// For each cone group (K-hop BFS + union-find merge):
//   1. Run ABC balance + map -D budget with per-PI arrivals / per-PO requireds
//      encoded as ABC timing constraints (via SetPerCiArrivals /
//      SetPerCoRequireds).  This is the C-API equivalent of CPA's
//      read_constr + map -D T.
//   2. Accept if ABC-predicted min slack improves over the current STA-derived
//      delay budget (conservative: only apply when remapping helps).
//   3. Legalize newly inserted cells after each round.
//   4. Stop when a round produces no applied cones (stagnation) or no
//      violating endpoints remain.
// ---------------------------------------------------------------------------
class ConeCpaRemapStrategy
{
 public:
  ConeCpaRemapStrategy(sta::Scene* corner,
                       sta::Slack  slack_threshold,
                       int         bfs_hops,
                       int         max_instances_per_cone,
                       int         proximity_threshold_dbu,
                       float       long_wire_threshold_ps,
                       unsigned    mcts_iters,
                       int         mcts_max_depth,
                       float       mcts_ucb_constant,
                       int         max_cones_per_round);

  void OptimizeDesign(sta::dbSta*      sta,
                      utl::UniqueName& name_generator,
                      rsz::Resizer*    resizer,
                      utl::Logger*     logger);

 private:
  sta::Scene* corner_;
  sta::Slack  slack_threshold_;
  int         bfs_hops_;
  int         max_instances_per_cone_;
  int         proximity_threshold_dbu_;
  float       long_wire_threshold_ps_;  // 0 = disabled
  unsigned    mcts_iters_;              // 0 = pure balance+map (no MCTS)
  int         mcts_max_depth_;
  float       mcts_ucb_constant_;
  int         max_cones_per_round_;     // 0 = unlimited
};

}  // namespace rmp
