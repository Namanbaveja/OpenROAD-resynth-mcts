// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#include "cone_manager.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cut/abc_library_factory.h"
#include "cut/logic_cut.h"
#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "odb/db.h"
#include "rsz/Resizer.hh"
#include "sta/Delay.hh"
#include "sta/Graph.hh"
#include "sta/MinMax.hh"
#include "sta/PathExpanded.hh"
#include "sta/PortDirection.hh"
#include "sta/Search.hh"
#include "sta/Units.hh"
#include "utils.h"
#include "utl/Logger.h"

namespace rmp {

using utl::RMP;

namespace {

// Return (x, y) placement of the ODB instance driving vertex's pin.
std::pair<int, int> GetVertexLocation(sta::Vertex* vertex, sta::dbSta* sta)
{
  sta::dbNetwork* network = sta->getDbNetwork();
  sta::Pin*        pin    = vertex->pin();
  odb::dbITerm*    iterm  = nullptr;
  odb::dbBTerm*    bterm  = nullptr;
  odb::dbModITerm* moditerm = nullptr;
  network->staToDb(pin, iterm, bterm, moditerm);
  if (!iterm) {
    return {0, 0};
  }
  odb::dbInst* inst = iterm->getInst();
  if (!inst || !inst->getPlacementStatus().isPlaced()) {
    return {0, 0};
  }
  int x = 0, y = 0;
  inst->getLocation(x, y);
  return {x, y};
}

// True when the instance is a sequential element or macro.
// These are cone boundaries: their output pins become PIs of the cone.
bool IsSequentialOrMacro(const sta::Instance* inst, sta::dbNetwork* network)
{
  sta::LibertyCell* cell = network->libertyCell(inst);
  if (!cell) {
    return false;
  }
  return cell->hasSequentials() || cell->isMacro();
}

// True when the instance is a pure buffer.
// Buffers are delay-padding with no Boolean logic; they cause excessive cone
// merging and offer no restructuring opportunity for ABC.
// NOTE: inverters are intentionally NOT excluded — in ASAP7 critical paths
// commonly use NAND→INV→NAND chains (AND-invert tree), and excluding the
// inverters breaks the logical chain that ABC needs to restructure.
bool IsPassThroughGate(const sta::Instance* inst, sta::dbNetwork* network)
{
  sta::LibertyCell* cell = network->libertyCell(inst);
  return cell && cell->isBuffer();
}

// Return ODB (x, y) placement of a sta::Instance in database units.
// Returns {0, 0} when the instance is not placed.
static std::pair<int, int> GetInstanceLocation(const sta::Instance* inst,
                                                sta::dbNetwork*      network)
{
  odb::dbInst* db_inst = network->staToDb(inst);
  if (!db_inst || !db_inst->getPlacementStatus().isPlaced()) {
    return {0, 0};
  }
  int x = 0, y = 0;
  db_inst->getLocation(x, y);
  return {x, y};
}

// ---------------------------------------------------------------------------
// ExtractPathInstances — collect all combinational sta::Instance* on the
// worst-slack path of a vertex.  Returns empty set if no path exists.
//
// long_wire_threshold_s: when > 0, the path is split at net traversals whose
//   arrival-time difference exceeds this value (seconds).  Only the final
//   segment (closest to the endpoint) is returned — logic beyond a long wire
//   is physically remote and can't be improved by local restructuring.
// ---------------------------------------------------------------------------
std::unordered_set<sta::Instance*> ExtractPathInstances(
    sta::Vertex* endpoint,
    sta::dbSta*  sta,
    float        long_wire_threshold_s = 0.0f)
{
  sta::Path* path = sta->vertexWorstSlackPath(endpoint, sta::MinMax::max());
  if (!path) {
    return {};
  }
  sta::PathExpanded expanded(path, sta);
  sta::dbNetwork*   network = sta->getDbNetwork();

  // PathExpanded lays out the full path: [0, startIndex()) = launch clock
  // tree, [startIndex(), size()) = data path.  We only want the data path:
  // clock-tree cells (buffers, inverters) appear in every timing path because
  // every launch FF shares the clock root, so including them would merge all
  // cones into one giant group.
  const size_t data_start = expanded.startIndex();

  // ---- Long-wire breaking -------------------------------------------------
  // Walk forward; whenever two consecutive path steps are in DIFFERENT
  // instances (a net/wire arc), compute the arrival-time difference.
  // If it exceeds long_wire_threshold_s, update the effective start to the
  // step AFTER the wire — we only keep the local segment nearest the endpoint.
  size_t effective_start = data_start;
  if (long_wire_threshold_s > 0.0f && expanded.size() > data_start + 1) {
    sta::Instance* prev_inst = nullptr;
    sta::Arrival   prev_arr  = 0.0;
    for (size_t i = data_start; i < expanded.size(); i++) {
      const sta::Path* p   = expanded.path(i);
      const sta::Pin*  pin = p->pin(sta);
      sta::Instance*   inst
          = network->instance(const_cast<sta::Pin*>(pin));
      const sta::Arrival arr = p->arrival();

      if (prev_inst && inst && inst != prev_inst) {
        // Net arc between different instances — this is (cell+wire) or pure
        // wire delay.  Use arrival difference as a proxy for wire delay.
        // Do NOT trigger on the arc into a sequential/macro (endpoint FF):
        // that's always a boundary, not a restructurable break point.
        if (!IsSequentialOrMacro(inst, network)) {
          const float step_delay = static_cast<float>(arr - prev_arr);
          if (step_delay > long_wire_threshold_s) {
            // Break here: keep only instances from this point onward.
            effective_start = i;
          }
        }
      }
      prev_inst = inst;
      prev_arr  = arr;
    }
  }

  std::unordered_set<sta::Instance*> insts;
  for (size_t i = effective_start; i < expanded.size(); i++) {
    const sta::Path* p    = expanded.path(i);
    const sta::Pin*  pin  = p->pin(sta);
    sta::Instance*   inst = network->instance(const_cast<sta::Pin*>(pin));
    if (inst && !network->isTopInstance(inst)
        && !IsSequentialOrMacro(inst, network)
        && !IsPassThroughGate(inst, network)) {
      insts.insert(inst);
    }
  }
  return insts;
}

// ---------------------------------------------------------------------------
// ExpandHops — given a seed instance set, expand `extra_hops` levels of
// backward BFS (from each instance's input pins to their driving instances).
// Stops at sequential elements, macros, and top-level ports.
// Operates in-place on `insts`.
// ---------------------------------------------------------------------------
void ExpandHops(std::unordered_set<sta::Instance*>& insts,
                int                                  extra_hops,
                sta::dbNetwork*                      network)
{
  for (int hop = 0; hop < extra_hops; ++hop) {
    std::unordered_set<sta::Instance*> to_add;
    for (sta::Instance* inst : insts) {
      auto pin_iter = std::unique_ptr<sta::InstancePinIterator>(
          network->pinIterator(inst));
      while (pin_iter->hasNext()) {
        sta::Pin* pin = pin_iter->next();
        if (!network->direction(pin)->isInput()) {
          continue;
        }
        sta::Net* net = network->net(pin);
        if (!net) {
          continue;
        }
        sta::PinSet* drivers = network->drivers(net);
        if (!drivers) {
          continue;
        }
        for (const sta::Pin* drv_pin : *drivers) {
          sta::Instance* drv_inst
              = network->instance(const_cast<sta::Pin*>(drv_pin));
          if (!drv_inst || network->isTopInstance(drv_inst)) {
            continue;
          }
          if (insts.count(drv_inst) || to_add.count(drv_inst)) {
            continue;
          }
          if (!IsSequentialOrMacro(drv_inst, network)
              && !IsPassThroughGate(drv_inst, network)) {
            to_add.insert(drv_inst);
          }
        }
      }
    }
    if (to_add.empty()) {
      break;
    }
    insts.insert(to_add.begin(), to_add.end());
  }
}

// ---------------------------------------------------------------------------
// BuildKHopLogicCut — construct a LogicCut directly from a known instance set
// and a list of endpoints (may be more than one after merging).
//
// Primary inputs  (PIs):
//   For every INPUT pin of every cone instance, if its driver instance is NOT
//   in cone_insts (or is a port / not an instance), the net on that pin is a PI.
//
// Primary outputs (POs):
//   1. The input net of EACH endpoint pin (one PO per endpoint).
//   2. Any output net of a cone instance that feeds an instance outside the cone.
//
// Having multiple POs (one per merged endpoint) is correct because
// InsertMappedAbcNetwork reconnects every PO net after replacement.
// ---------------------------------------------------------------------------
cut::LogicCut BuildKHopLogicCut(
    const std::unordered_set<sta::Instance*>& cone_insts,
    const std::vector<sta::Vertex*>&           endpoints,
    sta::dbNetwork*                            network,
    utl::Logger*                               /*logger*/)
{
  // Build the typed InstanceSet required by LogicCut.
  sta::InstanceSet cut_instances(network);
  for (sta::Instance* inst : cone_insts) {
    cut_instances.insert(inst);
  }

  // ------------------------------------------------------------------ POs --
  std::unordered_set<sta::Net*> po_net_set;

  // 1. Each endpoint's input net is a PO — but only when directly driven by
  //    a cone instance.  If the driver is a buffer/inverter that was excluded
  //    from cone_insts (pass-through), the driver won't be in cut_instances
  //    and BuildMappedAbcNetwork would crash at abc_instances.at(buffer).
  //    Step 2 below adds the cone-internal output net as the actual PO for
  //    the buffer-chain case: gate→net_A→buf→net_B→endpoint becomes PO=net_A.
  for (sta::Vertex* ep : endpoints) {
    sta::Pin* pin    = ep->pin();
    sta::Net* po_net = network->net(pin);
    if (!po_net) {
      sta::Term* term = network->term(pin);
      if (term) {
        po_net = network->net(term);
      }
    }
    if (!po_net) {
      continue;
    }
    // Only add as PO if directly driven by a cone instance.
    sta::PinSet* drivers = network->drivers(po_net);
    bool driver_in_cone = false;
    if (drivers) {
      for (const sta::Pin* drv : *drivers) {
        sta::Instance* drv_inst
            = network->instance(const_cast<sta::Pin*>(drv));
        if (drv_inst
            && cut_instances.find(drv_inst) != cut_instances.end()) {
          driver_in_cone = true;
          break;
        }
      }
    }
    if (driver_in_cone) {
      po_net_set.insert(po_net);
    }
    // If not driven by cone (e.g. driven by a buffer), step 2 adds the
    // correct upstream cone output net as the PO.
  }

  // 2. Any output net of a cone instance that feeds outside the cone.
  for (sta::Instance* inst : cone_insts) {
    auto pin_iter = std::unique_ptr<sta::InstancePinIterator>(
        network->pinIterator(inst));
    while (pin_iter->hasNext()) {
      sta::Pin* pin = pin_iter->next();
      if (!network->direction(pin)->isOutput()) {
        continue;
      }
      sta::Net* out_net = network->net(pin);
      if (!out_net) {
        continue;
      }
      auto load_iter = std::unique_ptr<sta::NetPinIterator>(
          network->pinIterator(out_net));
      while (load_iter->hasNext()) {
        const sta::Pin* load_pin = load_iter->next();
        if (network->direction(load_pin)->isOutput()) {
          continue;  // skip driver pin
        }
        sta::Instance* load_inst
            = network->instance(const_cast<sta::Pin*>(load_pin));
        bool outside = (!load_inst || network->isTopInstance(load_inst)
                        || cut_instances.find(load_inst) == cut_instances.end());
        if (outside) {
          po_net_set.insert(out_net);
          break;
        }
      }
    }
  }

  std::vector<sta::Net*> primary_outputs(po_net_set.begin(), po_net_set.end());

  // ------------------------------------------------------------------ PIs --
  std::unordered_set<sta::Net*> pi_net_set;
  for (sta::Instance* inst : cone_insts) {
    auto pin_iter = std::unique_ptr<sta::InstancePinIterator>(
        network->pinIterator(inst));
    while (pin_iter->hasNext()) {
      sta::Pin* pin = pin_iter->next();
      if (!network->direction(pin)->isInput()) {
        continue;
      }
      sta::Net* in_net = network->net(pin);
      if (!in_net) {
        continue;
      }
      if (po_net_set.count(in_net)) {
        continue;  // already a PO (driven inside the cone)
      }
      sta::PinSet* drivers = network->drivers(in_net);
      if (!drivers || drivers->empty()) {
        pi_net_set.insert(in_net);
        continue;
      }
      bool driver_inside = false;
      for (const sta::Pin* drv_pin : *drivers) {
        sta::Instance* drv_inst
            = network->instance(const_cast<sta::Pin*>(drv_pin));
        if (drv_inst
            && cut_instances.find(drv_inst) != cut_instances.end()) {
          driver_inside = true;
          break;
        }
      }
      if (!driver_inside) {
        pi_net_set.insert(in_net);
      }
    }
  }

  std::vector<sta::Net*> primary_inputs(pi_net_set.begin(), pi_net_set.end());

  return cut::LogicCut(std::move(primary_inputs),
                       std::move(primary_outputs),
                       std::move(cut_instances));
}

// ---------------------------------------------------------------------------
// GrowConeByProximity — proximity-based cone expansion (paper Algorithm 4).
//
// Starting from the seed instance set (the data-path instances from
// ExtractPathInstances), expands backward via BFS: a fanin driver is absorbed
// into the cone only if its Manhattan distance (in ODB database units) from
// the instance it drives is ≤ proximity_threshold_dbu.
//
// This replaces K-hop BFS when proximity_threshold_dbu > 0, producing
// physically tight cones rather than timing-graph-wide expansions.
// Skips sequential elements, macros, buffers, and unplaced instances.
// ---------------------------------------------------------------------------
static void GrowConeByProximity(std::unordered_set<sta::Instance*>& insts,
                                 int              proximity_threshold_dbu,
                                 sta::dbNetwork*  network)
{
  std::queue<sta::Instance*> frontier;
  for (sta::Instance* inst : insts) {
    frontier.push(inst);
  }

  while (!frontier.empty()) {
    sta::Instance* inst = frontier.front();
    frontier.pop();

    auto [ix, iy] = GetInstanceLocation(inst, network);

    auto pin_iter = std::unique_ptr<sta::InstancePinIterator>(
        network->pinIterator(inst));
    while (pin_iter->hasNext()) {
      sta::Pin* pin = pin_iter->next();
      if (!network->direction(pin)->isInput()) {
        continue;
      }
      sta::Net* net = network->net(pin);
      if (!net) {
        continue;
      }
      sta::PinSet* drivers = network->drivers(net);
      if (!drivers) {
        continue;
      }
      for (const sta::Pin* drv_pin : *drivers) {
        sta::Instance* drv_inst
            = network->instance(const_cast<sta::Pin*>(drv_pin));
        if (!drv_inst || network->isTopInstance(drv_inst)) {
          continue;
        }
        if (insts.count(drv_inst)) {
          continue;
        }
        if (IsSequentialOrMacro(drv_inst, network) || IsPassThroughGate(drv_inst, network)) {
          continue;
        }
        // Only absorb the driver if it is placed and physically close to the
        // instance it feeds.  This keeps cones spatially coherent.
        auto [dx, dy] = GetInstanceLocation(drv_inst, network);
        if (dx == 0 && dy == 0) {
          continue;  // unplaced — skip
        }
        const int manhattan = std::abs(dx - ix) + std::abs(dy - iy);
        if (manhattan <= proximity_threshold_dbu) {
          insts.insert(drv_inst);
          frontier.push(drv_inst);
        }
      }
    }
  }
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// BuildConeGroups — K-hop BFS cone extraction + union-find merge.
//
// Phase 1 — Sort violating endpoints worst-first.
//
// Phase 2 — Per-endpoint K-hop cone building:
//   a. Trace the worst-slack path → extract all combinational instances
//      (via PathExpanded).  This is the "K=1" base.
//   b. Expand (bfs_hops - 1) additional backward-BFS hops from those
//      path instances.
//
// Phase 3 — Union-find merge:
//   Walk all endpoints.  For each instance already claimed by a previous
//   endpoint, union the two endpoint groups.  After the full pass every
//   endpoint belongs to exactly one group, and groups are disjoint.
//   Unlike the old greedy-skip approach, NO endpoint is deferred —
//   overlapping endpoints are co-optimized in the same cone.
//
// Phase 4 — Build one ConeData per merged group:
//   Merge instance sets, collect all endpoints as POs, annotate timing.
// ---------------------------------------------------------------------------
std::vector<ConeData> BuildConeGroups(sta::dbSta*      sta,
                                      rsz::Resizer*    resizer,
                                      cut::AbcLibrary& abc_library,
                                      sta::Slack       slack_threshold,
                                      int              bfs_hops,
                                      int              max_instances_per_cone,
                                      int              proximity_threshold_dbu,
                                      float            long_wire_threshold_s,
                                      utl::Logger*     logger)
{
  // ---- Phase 1: collect and sort violating endpoints (worst-first) ----
  auto all_endpoints = GetEndpoints(sta, resizer, slack_threshold);
  if (all_endpoints.empty()) {
    logger->info(RMP, 300,
                 "ConeMCTS: no violating endpoints (slack_threshold={}).",
                 static_cast<double>(slack_threshold));
    return {};
  }

  std::sort(all_endpoints.begin(), all_endpoints.end(),
            [&sta](sta::Vertex* a, sta::Vertex* b) {
              const sta::Slack sa = sta->slack(a,
                                               sta::RiseFallBoth::riseFall(),
                                               sta->scenes(),
                                               sta::MinMax::max());
              const sta::Slack sb = sta->slack(b,
                                               sta::RiseFallBoth::riseFall(),
                                               sta->scenes(),
                                               sta::MinMax::max());
              return sa < sb;
            });

  const int       n       = static_cast<int>(all_endpoints.size());
  sta::dbNetwork* network = sta->getDbNetwork();

  logger->info(RMP, 301,
               "ConeMCTS: {} violating endpoints; building {} cones ({}).",
               n,
               proximity_threshold_dbu > 0 ? "proximity" : "K-hop",
               proximity_threshold_dbu > 0
                   ? "proximity_dbu=" + std::to_string(proximity_threshold_dbu)
                   : "bfs_hops=" + std::to_string(bfs_hops));

  // ---- Phase 2: build per-endpoint instance sets ----
  //
  // Two modes controlled by proximity_threshold_dbu:
  //   • proximity_threshold_dbu > 0 → GrowConeByProximity: absorb fanin
  //     drivers that are within the threshold Manhattan distance (dbu) of the
  //     instance they drive.  Produces physically-tight cones.
  //   • proximity_threshold_dbu == 0 → ExpandHops: K-hop backward BFS on the
  //     timing graph (original behaviour; bfs_hops controls depth).
  //
  // In both modes, buffers are now excluded from cones (they offer no
  // Boolean restructuring opportunity for ABC and cause excessive merging).
  std::vector<std::unordered_set<sta::Instance*>> cone_insts(n);
  for (int i = 0; i < n; i++) {
    cone_insts[i] = ExtractPathInstances(all_endpoints[i], sta, long_wire_threshold_s);
    if (!cone_insts[i].empty()) {
      if (proximity_threshold_dbu > 0) {
        GrowConeByProximity(cone_insts[i], proximity_threshold_dbu, network);
      } else if (bfs_hops > 1) {
        ExpandHops(cone_insts[i], bfs_hops - 1, network);
      }
    }
  }

  // ---- Phase 2.5: diagnostic — find the most-shared instances ----
  // Count how many endpoint cones each instance appears in.
  {
    std::unordered_map<sta::Instance*, int> inst_count;
    for (int i = 0; i < n; i++) {
      for (sta::Instance* inst : cone_insts[i]) {
        inst_count[inst]++;
      }
    }
    // Sort by count descending.
    std::vector<std::pair<int, sta::Instance*>> ranked;
    ranked.reserve(inst_count.size());
    for (auto& [inst, cnt] : inst_count) {
      ranked.emplace_back(cnt, inst);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    // Print instances that appear in more than 1 endpoint cone.
    int printed = 0;
    for (auto& [cnt, inst] : ranked) {
      if (cnt <= 1 || printed >= 20) {
        break;
      }
      debugPrint(logger, RMP, "cone_mcts", 1,
                 "  shared instance (in {} / {} cones): {}",
                 cnt, n, network->pathName(inst));
      ++printed;
    }
    if (printed == 0) {
      debugPrint(logger, RMP, "cone_mcts", 1,
                 "  no instances shared across multiple cones");
    }
  }

  // ---- Phase 3: group assignment ----
  //
  // Union-find merge: endpoints whose cone_insts overlap (share any instance,
  // including buffers included for connectivity) are merged into one group.
  // This applies to both K-hop and proximity modes.  Buffers are excluded
  // from the actual ABC cone in BuildKHopLogicCut.

  std::unordered_map<int, std::vector<int>> root_to_members;

  // Union-find merge: both K-hop and proximity modes now merge overlapping
  // cones.  Buffers are included in cone_insts for connectivity analysis, so
  // two endpoints connected through a buffer chain will naturally share
  // instances and get merged into one group here.  The buffers are stripped
  // from the ABC cone in BuildKHopLogicCut.
  {
    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);

    std::function<int(int)> find_root = [&](int x) -> int {
      if (parent[x] != x) {
        parent[x] = find_root(parent[x]);
      }
      return parent[x];
    };
    auto union_groups = [&](int a, int b) {
      a = find_root(a);
      b = find_root(b);
      if (a != b) {
        parent[b] = a;
      }
    };

    std::unordered_map<sta::Instance*, int> inst_first_owner;
    inst_first_owner.reserve(n * 20);

    for (int i = 0; i < n; i++) {
      for (sta::Instance* inst : cone_insts[i]) {
        auto [it, inserted] = inst_first_owner.emplace(inst, i);
        if (!inserted) {
          union_groups(i, it->second);
        }
      }
    }

    for (int i = 0; i < n; i++) {
      if (!cone_insts[i].empty()) {
        root_to_members[find_root(i)].push_back(i);
      }
    }

    logger->info(RMP, 341,
                 "ConeMCTS: {} endpoints → {} merged cone group(s) "
                 "(bfs_hops={}).",
                 n, static_cast<int>(root_to_members.size()), bfs_hops);
  }

  // ---- Phase 4: build ConeData for each merged group ----
  const double lib_scale = static_cast<double>(
      sta->units()->timeUnit()->scale());

  std::vector<ConeData> groups;
  groups.reserve(root_to_members.size());

  int skipped_empty = 0;
  int skipped_large = 0;

  for (auto& [root, members] : root_to_members) {
    // Union of all instance sets in this merged group.
    std::unordered_set<sta::Instance*> merged_insts;
    std::vector<sta::Vertex*>           merged_endpoints;

    for (int idx : members) {
      merged_insts.insert(cone_insts[idx].begin(), cone_insts[idx].end());
      merged_endpoints.push_back(all_endpoints[idx]);
    }

    if (merged_insts.empty()) {
      ++skipped_empty;
      continue;
    }

    const int n_insts = static_cast<int>(merged_insts.size());
    if (max_instances_per_cone > 0 && n_insts > max_instances_per_cone) {
      ++skipped_large;
      debugPrint(logger, RMP, "cone_mcts", 1,
                 "merged group ({} eps): {} instances > max {} — skipping",
                 members.size(), n_insts, max_instances_per_cone);
      continue;
    }

    // Build LogicCut from the merged instance set + all endpoints as POs.
    cut::LogicCut cut
        = BuildKHopLogicCut(merged_insts, merged_endpoints, network, logger);

    if (cut.IsEmpty()) {
      ++skipped_empty;
      continue;
    }

    // ---- per-PO required times (one entry per endpoint net) ----
    std::unordered_map<std::string, float> po_requireds;
    float min_required_f = std::numeric_limits<float>::max();

    for (sta::Vertex* ep : merged_endpoints) {
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
        sta::Term* term = network->term(pin);
        if (term) {
          po_net = network->net(term);
        }
      }
      if (po_net && lib_scale > 0.0) {
        std::string net_path = network->pathName(po_net);
        if (!net_path.empty()) {
          // Use the tightest required time if the same net appears twice.
          auto [it, ok] = po_requireds.emplace(net_path,
                                               req_f / static_cast<float>(lib_scale));
          if (!ok && req_f / static_cast<float>(lib_scale) < it->second) {
            it->second = req_f / static_cast<float>(lib_scale);
          }
        }
      }
    }

    // ---- per-PI arrival times ----
    std::unordered_map<std::string, float> pi_arrivals;
    float max_arrival_f = std::numeric_limits<float>::lowest();

    for (const sta::Net* net : cut.primary_inputs()) {
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
          && lib_scale > 0.0) {
        std::string net_path = network->pathName(net);
        if (!net_path.empty()) {
          pi_arrivals[net_path]
              = net_arrival_f / static_cast<float>(lib_scale);
        }
      }
    }

    // ---- scalar delay budget ----
    // budget = min(required) - max(arrival): the maximum combinational delay
    // the cone logic may consume before violating the path.
    // NOTE: do NOT clamp max_arrival to 0 — arrivals can be negative (e.g.
    // clock-driven paths analysed from a negative epoch), and clamping would
    // turn a met cone (positive budget) into an apparent violation, causing
    // unnecessary / counter-productive remapping.
    double delay_budget = 0.0;
    if (min_required_f < std::numeric_limits<float>::max()
        && lib_scale > 0.0) {
      const double budget_s = static_cast<double>(
          min_required_f - max_arrival_f);
      delay_budget = budget_s / lib_scale;
    }

    // ---- physical centroid (average over merged endpoints) ----
    int centroid_x = 0, centroid_y = 0;
    {
      long long sum_x = 0, sum_y = 0;
      int       count = 0;
      for (sta::Vertex* ep : merged_endpoints) {
        auto [cx, cy] = GetVertexLocation(ep, sta);
        if (cx != 0 || cy != 0) {
          sum_x += cx;
          sum_y += cy;
          ++count;
        }
      }
      if (count > 0) {
        centroid_x = static_cast<int>(sum_x / count);
        centroid_y = static_cast<int>(sum_y / count);
      }
    }

    debugPrint(logger, RMP, "cone_mcts", 1,
               "cone {}/{}: eps={}  budget={:.4f}  pi={}  po={}  insts={}",
               groups.size() + 1,
               root_to_members.size(),
               merged_endpoints.size(),
               delay_budget,
               pi_arrivals.size(),
               po_requireds.size(),
               n_insts);

    groups.push_back({std::move(merged_endpoints),
                      std::move(cut),
                      std::move(pi_arrivals),
                      std::move(po_requireds),
                      static_cast<float>(lib_scale),
                      delay_budget,
                      centroid_x,
                      centroid_y});
  }

  logger->info(RMP, 303,
               "ConeMCTS: {} cone group(s) built ({} empty, {} too large).",
               groups.size(), skipped_empty, skipped_large);
  return groups;
}

}  // namespace rmp
