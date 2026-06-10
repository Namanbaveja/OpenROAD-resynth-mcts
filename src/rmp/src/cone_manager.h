// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "cut/abc_library_factory.h"
#include "cut/logic_cut.h"
#include "sta/Delay.hh"
#include "sta/Graph.hh"

namespace rsz {
class Resizer;
}

namespace sta {
class dbSta;
class Vertex;
}  // namespace sta

namespace utl {
class Logger;
}

namespace rmp {

// Pre-extracted cone for one group of physically-close violating endpoints.
//
// All STA timing values are captured from the pre-repair design before any
// ODB commit.  Because cones are clustered so that their internal instances
// are physically non-overlapping, committing one cone never invalidates the
// stored data of another.
struct ConeData
{
  // Violating endpoint vertices that define this cone group.
  std::vector<sta::Vertex*> endpoints;

  // Logic cut: primary-input/output nets + internal instances.
  // Extracted once; reused for every MCTS iteration and the final apply.
  cut::LogicCut cut;

  // Per-CI arrival time in Liberty time units (seconds / liberty_time_scale).
  // Key = ABC CI name = driver-pin name of the primary-input net.
  std::unordered_map<std::string, float> pi_arrivals;

  // Per-CO required time in Liberty time units.
  // Key = endpoint pin name (same string ABC assigns to the CO).
  std::unordered_map<std::string, float> po_requireds;

  // Multiplier: STA internal seconds per Liberty time unit (e.g. 1e-12 for ps).
  float liberty_time_scale = 1.0f;

  // Scalar fallback budget = min_required - max_arrival (Liberty units).
  // Passed as DelayTarget to Abc_NtkMap when per-pin lookup misses.
  double delay_budget = 0.0;

  // Physical centroid of the cone's endpoint instances (ODB database units).
  int centroid_x = 0;
  int centroid_y = 0;
};

// Build cone groups from violating endpoints.
//
//   Phase 1 — Sort violating endpoints worst-first.
//   Phase 2 — Instance-set extraction per endpoint:
//     • proximity_threshold_dbu > 0: GrowConeByProximity — absorb fanin
//       drivers within the threshold Manhattan distance (ODB dbu).  Produces
//       physically-tight cones; buffers are boundaries.
//     • proximity_threshold_dbu == 0: K-hop backward BFS (bfs_hops depth);
//       buffers are still excluded as they offer no ABC restructuring.
//   Phase 3 — Union-find merge: endpoints sharing any instance are co-optimised.
//   Phase 4 — Build LogicCut per independent group; skip if empty or too large.
//
// The resulting cones are structurally independent (no shared instances).
std::vector<ConeData> BuildConeGroups(sta::dbSta* sta,
                                      rsz::Resizer* resizer,
                                      cut::AbcLibrary& abc_library,
                                      sta::Slack slack_threshold,
                                      int bfs_hops,
                                      int max_instances_per_cone,
                                      int proximity_threshold_dbu,
                                      float long_wire_threshold_s,
                                      utl::Logger* logger);

}  // namespace rmp
