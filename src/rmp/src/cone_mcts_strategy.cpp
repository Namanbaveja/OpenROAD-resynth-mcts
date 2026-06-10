// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#include "cone_mcts_strategy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cone_manager.h"
#include "cut/abc_library_factory.h"
#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "dpl/Opendp.h"
#include "est/EstimateParasitics.h"
#include "gia.h"
#include "odb/db.h"
#include "rsz/Resizer.hh"
#include "sta/Graph.hh"
#include "sta/MinMax.hh"
#include "sta/Scene.hh"
#include "utl/Logger.h"
#include "utl/unique_name.h"

namespace rmp {

using utl::RMP;

ConeMctsStrategy::ConeMctsStrategy(sta::Scene*  corner,
                                   sta::Slack   slack_threshold,
                                   unsigned     n_iters,
                                   int          max_depth,
                                   int          bfs_hops,
                                   int          max_instances_per_cone,
                                   unsigned     mcts_seed,
                                   float        ucb_constant)
    : corner_(corner),
      slack_threshold_(slack_threshold),
      n_iters_(n_iters),
      max_depth_(max_depth),
      bfs_hops_(bfs_hops),
      max_instances_per_cone_(max_instances_per_cone),
      ucb_constant_(ucb_constant),
      rng_(mcts_seed)
{
}

// ---------------------------------------------------------------------------
// OptimizeCone — per-cone MCTS.
//
// Tree structure:
//   • Root = empty op sequence.
//   • Each edge = one GIA op applied next.
//   • Node depth ∈ [0, max_depth_].
//
// Each iteration:
//   1. Selection: traverse using UCT until a node with an untried op is found.
//   2. Expansion: create the child node for the untried op.
//   3. Rollout: fill the remaining depth positions with random ops.
//   4. Evaluation: call RunGiaConeSlack on the full path.
//   5. Backpropagation: MAX-update best_reward and increment visits on all
//      nodes visited during selection+expansion.
// ---------------------------------------------------------------------------
std::vector<GiaOp> ConeMctsStrategy::OptimizeCone(
    const ConeData&           cone,
    const std::vector<GiaOp>& all_ops,
    cut::AbcLibrary&          abc_library,
    sta::dbSta*               sta,
    utl::Logger*              logger)
{
  if (all_ops.empty()) {
    return {};
  }

  // Evaluate baseline (original logic, no ops applied).
  // Return {} if MCTS never beats this — caller treats {} as "skip apply".
  const double baseline_reward = RunGiaConeSlack(
      sta,
      const_cast<cut::LogicCut&>(cone.cut),
      abc_library,
      {},
      cone.pi_arrivals,
      cone.po_requireds,
      cone.liberty_time_scale,
      cone.delay_budget,
      logger);

  MctsNode root;
  std::vector<GiaOp> best_path;         // {} = no improvement over baseline
  double best_reward = baseline_reward;  // must beat baseline to be accepted

  for (unsigned iter = 0; iter < n_iters_; iter++) {
    std::vector<GiaOp>      path;
    std::vector<MctsNode*>  visited;  // nodes updated during selection+expansion

    visited.push_back(&root);
    MctsNode* node = &root;

    // ---- Selection + expansion ----
    while (static_cast<int>(path.size()) < max_depth_) {
      // Identify ops not yet tried from this node.
      std::vector<size_t> untried_indices;
      for (size_t i = 0; i < all_ops.size(); i++) {
        if (node->children.find(all_ops[i].id) == node->children.end()) {
          untried_indices.push_back(i);
        }
      }

      if (!untried_indices.empty()) {
        // Expansion: pick a random untried op and create the child.
        const size_t pick
            = untried_indices[rng_() % untried_indices.size()];
        const GiaOp& op = all_ops[pick];
        path.push_back(op);
        node->children[op.id] = std::make_unique<MctsNode>();
        node = node->children[op.id].get();
        visited.push_back(node);
        break;  // stop selection; rollout fills the rest
      }

      // All ops tried at this node — UCT selection.
      const GiaOp* selected    = nullptr;
      double       best_uct    = -std::numeric_limits<double>::max();
      const int    parent_vis  = node->visits > 0 ? node->visits : 1;

      for (const GiaOp& op : all_ops) {
        auto cit = node->children.find(op.id);
        if (cit == node->children.end()) {
          continue;  // op not yet expanded at this node; skip in UCT
        }
        MctsNode* child = cit->second.get();
        double uct;
        if (child->visits == 0) {
          uct = std::numeric_limits<double>::max();
        } else {
          uct = child->best_reward
                + ucb_constant_
                      * std::sqrt(std::log(static_cast<double>(parent_vis))
                                  / child->visits);
        }
        if (uct > best_uct) {
          best_uct = uct;
          selected  = &op;
        }
      }

      if (!selected) {
        break;
      }
      path.push_back(*selected);
      node = node->children[selected->id].get();
      visited.push_back(node);
    }

    // ---- Rollout: fill remaining positions with random ops ----
    while (static_cast<int>(path.size()) < max_depth_) {
      path.push_back(all_ops[rng_() % all_ops.size()]);
    }

    // ---- Evaluation ----
    // RunGiaConeSlack is read-only on the cut: safe to call many times.
    const double reward = RunGiaConeSlack(
        sta,
        const_cast<cut::LogicCut&>(cone.cut),
        abc_library,
        path,
        cone.pi_arrivals,
        cone.po_requireds,
        cone.liberty_time_scale,
        cone.delay_budget,
        logger);

    debugPrint(logger,
               RMP,
               "cone_mcts",
               2,
               "iter {} / {}: reward={:.4f}  path_len={}",
               iter + 1,
               n_iters_,
               reward,
               path.size());

    if (reward > best_reward) {
      best_reward = reward;
      best_path   = path;
    }

    // ---- MAX backpropagation ----
    for (MctsNode* n : visited) {
      n->visits++;
      if (reward > n->best_reward) {
        n->best_reward = reward;
      }
    }
  }

  debugPrint(logger,
             RMP,
             "cone_mcts",
             1,
             "cone MCTS done: best_reward={:.4f}  best_path_len={}",
             best_reward,
             best_path.size());

  return best_path;
}

// ---------------------------------------------------------------------------
// LegalizeUnplaced — place UNPLACED instances inserted by InsertMappedAbcNetwork
// and mark them PLACED.  Returns the number of cells that could not be placed.
// ---------------------------------------------------------------------------
static int LegalizeUnplaced(rsz::Resizer* resizer,
                             sta::dbSta*   sta,
                             utl::Logger*  logger)
{
  auto* opendp = resizer->getOpendp();
  if (!opendp) {
    return 0;
  }

  odb::dbBlock* block = sta->db()->getChip()->getBlock();
  std::vector<odb::dbInst*> unplaced;
  for (odb::dbInst* inst : block->getInsts()) {
    if (!inst->getPlacementStatus().isPlaced()) {
      unplaced.push_back(inst);
    }
  }
  if (unplaced.empty()) {
    return 0;
  }

  debugPrint(logger,
             utl::RMP,
             "cone_mcts",
             1,
             "DPL: legalizing {} unplaced instance(s)",
             unplaced.size());

  const int failures
      = opendp->legalizeNewCellsIncremental(/*max_displacement_x=*/20,
                                            /*max_displacement_y=*/5);
  for (odb::dbInst* inst : unplaced) {
    inst->setPlacementStatus(odb::dbPlacementStatus::PLACED);
  }
  return failures;
}

// ---------------------------------------------------------------------------
// OptimizeDesign — iterative multi-cone orchestration.
//
// Each round:
//   1. Re-run STA to get fresh timing (after prior round's ODB changes).
//   2. Extract per-endpoint cones sorted by slack (worst first).
//   3. MCTS-optimize every cone independently (pure ABC, no ODB writes).
//   4. Apply non-overlapping cones to ODB (worst-slack cone first).
//   5. Legalize newly inserted instances (legalizeNewCellsIncremental).
//
// After all rounds:
//   6. repair_setup once on the fully stitched design.
//   7. DPL fine-tune + final WNS/TNS report.
//
// Rounds continue until no violating endpoints remain or no cone was applied
// in the latest round (i.e., all cones were skipped due to overlap).
// ---------------------------------------------------------------------------
void ConeMctsStrategy::OptimizeDesign(sta::dbSta*      sta,
                                       utl::UniqueName& name_generator,
                                       rsz::Resizer*    resizer,
                                       utl::Logger*     logger)
{
  using utl::RMP;

  // Ensure STA graph is ready.
  sta->ensureGraph();
  sta->ensureLevelized();
  sta->searchPreamble();
  for (auto mode : sta->modes()) {
    sta->ensureClkNetwork(mode);
  }

  // Build AbcLibrary once (Liberty data doesn't change across rounds).
  cut::AbcLibraryFactory factory(logger);
  factory.AddDbSta(sta);
  factory.AddResizer(resizer);
  factory.SetCorner(corner_);
  cut::AbcLibrary abc_library = factory.Build();

  const std::vector<GiaOp> all_ops = GiaOps(logger);

  sta::dbNetwork* network   = sta->getDbNetwork();
  odb::dbBlock*   block     = sta->db()->getChip()->getBlock();

  // ---- Iterative multi-cone loop ----
  // Run up to kMaxRounds.  The only early-exit is when a round applies 0 cones
  // (genuine stagnation: every selected cone's instances were already consumed
  // or every cone was too large / empty).  WNS-based stagnation is avoided
  // because post-apply WNS (before repair_setup) can temporarily regress
  // while net timing is still improving at the end-of-run level.
  constexpr int kMaxRounds        = 10;
  int           total_cones_applied = 0;
  int           round               = 0;

  while (round < kMaxRounds) {
    ++round;

    // Refresh STA before each round (previous round may have changed ODB).
    sta->searchPreamble();

    // ---- Extract per-endpoint cones sorted by slack (worst first) ----
    auto cone_groups = BuildConeGroups(sta,
                                       resizer,
                                       abc_library,
                                       slack_threshold_,
                                       bfs_hops_,
                                       max_instances_per_cone_,
                                       /*proximity_threshold_dbu=*/0,
                                       /*long_wire_threshold_s=*/0.0f,
                                       logger);
    if (cone_groups.empty()) {
      logger->info(RMP,
                   310,
                   "ConeMCTS: round {}: no violating cones — stopping.",
                   round);
      break;
    }

    logger->info(RMP,
                 311,
                 "ConeMCTS: round {}: {} cones  |  {} iters each  |  "
                 "depth={}  bfs_hops={}  max_insts={}",
                 round,
                 cone_groups.size(),
                 n_iters_,
                 max_depth_,
                 bfs_hops_,
                 max_instances_per_cone_);

    // ---- MCTS-optimize every cone (pure ABC, no ODB writes) ----
    std::vector<std::vector<GiaOp>> best_ops_per_cone;
    best_ops_per_cone.reserve(cone_groups.size());

    for (size_t i = 0; i < cone_groups.size(); i++) {
      const ConeData& cone = cone_groups[i];
      logger->info(RMP,
                   312,
                   "ConeMCTS: round {} cone {}/{}: {} ep  "
                   "pi={} po={} budget={:.4f}",
                   round,
                   i + 1,
                   cone_groups.size(),
                   cone.endpoints.size(),
                   cone.pi_arrivals.size(),
                   cone.po_requireds.size(),
                   cone.delay_budget);

      auto best_ops = OptimizeCone(cone, all_ops, abc_library, sta, logger);
      best_ops_per_cone.push_back(std::move(best_ops));
    }

    // ---- Apply non-overlapping cones (worst-slack first, crash-safe) ----
    int round_applied = 0;
    {
      auto* ep = resizer->getEstimateParasitics();
      const bool use_incremental
          = (ep != nullptr && ep->wireSignalCapacitance(corner_) > 0.0);
      std::unique_ptr<est::IncrementalParasiticsGuard> apply_guard;
      if (use_incremental) {
        apply_guard = std::make_unique<est::IncrementalParasiticsGuard>(ep);
      }

      for (size_t i = 0; i < cone_groups.size(); i++) {
        // Refresh live-instance set: detect cones whose gates were already
        // deleted by a previously applied cone in this round.
        std::unordered_set<odb::dbInst*> live_insts;
        for (odb::dbInst* inst : block->getInsts()) {
          live_insts.insert(inst);
        }

        bool any_dead = false;
        for (const sta::Instance* si : cone_groups[i].cut.cut_instances()) {
          if (live_insts.find(network->staToDb(si)) == live_insts.end()) {
            any_dead = true;
            break;
          }
        }
        if (any_dead) {
          logger->info(RMP,
                       321,
                       "ConeMCTS: round {} cone {}/{} skipped "
                       "(instances consumed by prior cone).",
                       round,
                       i + 1,
                       cone_groups.size());
          continue;
        }

        if (best_ops_per_cone[i].empty()) {
          debugPrint(logger, RMP, "cone_mcts", 1,
                     "ConeMCTS: round {} cone {}/{} skipped "
                     "(MCTS found no improvement over baseline).",
                     round, i + 1, cone_groups.size());
          continue;
        }

        logger->info(RMP,
                     313,
                     "ConeMCTS: round {} applying cone {}/{} ({} ops).",
                     round,
                     i + 1,
                     cone_groups.size(),
                     best_ops_per_cone[i].size());

        try {
          ApplyConeOps(sta,
                       cone_groups[i].cut,
                       abc_library,
                       best_ops_per_cone[i],
                       cone_groups[i].pi_arrivals,
                       cone_groups[i].po_requireds,
                       cone_groups[i].liberty_time_scale,
                       cone_groups[i].delay_budget,
                       name_generator,
                       logger);
          ++round_applied;
        } catch (const std::exception& ex) {
          debugPrint(logger, RMP, "cone_mcts", 1,
                     "ConeMCTS: round {} cone {}/{} apply failed ({}), skipping.",
                     round, i + 1, cone_groups.size(), ex.what());
        }
      }

      if (apply_guard) {
        apply_guard->update();
      }
    }

    total_cones_applied += round_applied;

    if (round_applied == 0) {
      logger->info(RMP,
                   322,
                   "ConeMCTS: round {}: no cone applied "
                   "(all skipped or no improvement found) — stopping.",
                   round);
      break;
    }

    // Legalize newly inserted instances before the next round's STA.
    const int dpl_fails = LegalizeUnplaced(resizer, sta, logger);
    if (dpl_fails > 0) {
      logger->warn(RMP,
                   320,
                   "ConeMCTS: round {}: DPL failed to place {} cell(s).",
                   round,
                   dpl_fails);
    }

    logger->info(RMP,
                 314,
                 "ConeMCTS: round {}: applied {} cone(s)  DPL done.",
                 round,
                 round_applied);
  }

  if (total_cones_applied == 0) {
    logger->info(RMP, 323, "ConeMCTS: no cones applied — nothing changed.");
    return;
  }

  // ---- repair_setup once on the fully stitched design ----
  logger->info(RMP, 315, "ConeMCTS: running repair_setup.");
  {
    auto* ep = resizer->getEstimateParasitics();
    const bool use_incremental
        = (ep != nullptr && ep->wireSignalCapacitance(corner_) > 0.0);
    std::unique_ptr<est::IncrementalParasiticsGuard> repair_guard;
    if (use_incremental) {
      repair_guard = std::make_unique<est::IncrementalParasiticsGuard>(ep);
    }

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
                           /*skip_buffer_removal=*/true,
                           /*skip_last_gasp=*/true,
                           /*skip_vt_swap=*/true,
                           /*skip_crit_vt_swap=*/true);
      logger->info(RMP, 316, "ConeMCTS: repair_setup completed.");
    } catch (const std::exception& e) {
      logger->warn(RMP, 317, "ConeMCTS: repair_setup failed: {}.", e.what());
    } catch (...) {
      logger->warn(RMP, 318, "ConeMCTS: repair_setup failed (unknown).");
    }

    if (repair_guard) {
      repair_guard->update();
    }

    // DPL after repair_setup (rsz inserts buffers/clones with UNPLACED status).
    LegalizeUnplaced(resizer, sta, logger);
    if (auto* opendp = resizer->getOpendp()) {
      opendp->detailedPlacement(0, 0, "", /*incremental=*/true);
    }
  }

  // ---- Final timing report ----
  sta::Slack   wns;
  sta::Vertex* worst_vertex = nullptr;
  sta->worstSlack(corner_, sta::MinMax::max(), wns, worst_vertex);
  const sta::Slack tns = sta->totalNegativeSlack(corner_, sta::MinMax::max());

  logger->info(RMP,
               319,
               "ConeMCTS: done ({} rounds, {} cones applied).  "
               "WNS={:.4f} ps  TNS={:.4f} ps",
               round,
               total_cones_applied,
               static_cast<double>(wns) * 1e12,
               static_cast<double>(tns) * 1e12);
}

}  // namespace rmp
