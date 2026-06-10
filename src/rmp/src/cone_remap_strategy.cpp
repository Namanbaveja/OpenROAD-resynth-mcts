// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#include "cone_remap_strategy.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cone_manager.h"
#include "cone_mcts_strategy.h"
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

ConeCpaRemapStrategy::ConeCpaRemapStrategy(sta::Scene* corner,
                                           sta::Slack  slack_threshold,
                                           int         bfs_hops,
                                           int         max_instances_per_cone,
                                           int         proximity_threshold_dbu,
                                           float       long_wire_threshold_ps,
                                           unsigned    mcts_iters,
                                           int         mcts_max_depth,
                                           float       mcts_ucb_constant,
                                           int         max_cones_per_round)
    : corner_(corner),
      slack_threshold_(slack_threshold),
      bfs_hops_(bfs_hops),
      max_instances_per_cone_(max_instances_per_cone),
      proximity_threshold_dbu_(proximity_threshold_dbu),
      long_wire_threshold_ps_(long_wire_threshold_ps),
      mcts_iters_(mcts_iters),
      mcts_max_depth_(mcts_max_depth),
      mcts_ucb_constant_(mcts_ucb_constant),
      max_cones_per_round_(max_cones_per_round)
{
}

// ---------------------------------------------------------------------------
// LegalizeUnplaced — place UNPLACED instances and mark them PLACED.
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

  debugPrint(logger, RMP, "cone_mcts", 1,
             "CPA-remap: legalizing {} unplaced instance(s)", unplaced.size());

  const int failures
      = opendp->legalizeNewCellsIncremental(/*max_displacement_x=*/20,
                                            /*max_displacement_y=*/5);
  for (odb::dbInst* inst : unplaced) {
    inst->setPlacementStatus(odb::dbPlacementStatus::PLACED);
  }
  return failures;
}

// ---------------------------------------------------------------------------
// OptimizeDesign — CPA-remap iterative loop.
//
// Each round:
//   1. Re-run STA to get fresh timing.
//   2. Extract cone groups (K-hop BFS + long-wire break + union-find merge).
//      Buffers and inverters are treated as cone boundaries (paper §III-A).
//   3. For each cone:
//      a. If mcts_iters_ > 0: run MCTS with per-PI/PO timing to find the
//         best ABC op sequence.  The MCTS reward = RunGiaConeSlack(ops, …)
//         which uses SetPerCiArrivals/SetPerCoRequireds — equivalent to the
//         paper's read_constr.
//      b. Else: run balance + map -D budget only (no MCTS exploration).
//      Accept the cone if the ABC-predicted slack > current delay budget.
//   4. Apply accepted cones (up to max_cones_per_round_ per round, worst first).
//      Cones whose instances were consumed by an earlier applied cone are skipped.
//   5. Legalize newly inserted instances.
// Stop when no cone applied in a round or no violating endpoints remain.
// ---------------------------------------------------------------------------
void ConeCpaRemapStrategy::OptimizeDesign(sta::dbSta*      sta,
                                          utl::UniqueName& name_generator,
                                          rsz::Resizer*    resizer,
                                          utl::Logger*     logger)
try {
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

  // All GIA ops for MCTS exploration (only needed when mcts_iters_ > 0).
  const std::vector<GiaOp> all_ops = (mcts_iters_ > 0) ? GiaOps(logger)
                                                        : std::vector<GiaOp>{};

  sta::dbNetwork* network = sta->getDbNetwork();
  odb::dbBlock*   block   = sta->db()->getChip()->getBlock();

  // Convert long-wire threshold from ps → seconds for cone_manager.
  const float long_wire_s = long_wire_threshold_ps_ * 1e-12f;

  constexpr int kMaxRounds       = 10;
  int           total_applied    = 0;
  int           round            = 0;

  while (round < kMaxRounds) {
    ++round;

    sta->searchPreamble();

    auto cone_groups = BuildConeGroups(sta,
                                       resizer,
                                       abc_library,
                                       slack_threshold_,
                                       bfs_hops_,
                                       max_instances_per_cone_,
                                       proximity_threshold_dbu_,
                                       long_wire_s,
                                       logger);
    if (cone_groups.empty()) {
      logger->info(RMP, 400,
                   "CPA-remap: round {}: no violating cones — stopping.", round);
      break;
    }

    logger->info(RMP, 401,
                 "CPA-remap: round {}: {} cones  |  "
                 "proximity={}dbu  long_wire={:.0f}ps  bfs={}  mcts={}iters",
                 round,
                 cone_groups.size(),
                 proximity_threshold_dbu_,
                 static_cast<double>(long_wire_threshold_ps_),
                 bfs_hops_,
                 mcts_iters_);

    // ---- Evaluate each cone ----
    //
    // Two evaluation modes:
    //   mcts_iters_ > 0 → MCTS explores different ABC op sequences; each
    //       iteration calls RunGiaConeSlack(ops, pi_arrivals, po_requireds, …)
    //       which encodes per-PI/PO timing constraints via SetPerCiArrivals /
    //       SetPerCoRequireds — the C-API equivalent of the paper's read_constr.
    //       Returns the best op sequence found.
    //   mcts_iters_ == 0 → plain balance + map -D budget (empty ops).
    //
    // Accept criterion (CPA paper §III-B, Algorithm 1):
    //   Tr < To  →  RunGiaConeSlack returns To - Tr > 0
    // We accept the cone if remapping reduced the cone's worst-case path delay.
    std::vector<bool>             should_apply(cone_groups.size(), false);
    std::vector<std::vector<GiaOp>> best_ops_vec(cone_groups.size());

    // Build MCTS strategy object once (reused across cones, carries RNG state).
    std::unique_ptr<ConeMctsStrategy> mcts_strategy;
    if (mcts_iters_ > 0) {
      mcts_strategy = std::make_unique<ConeMctsStrategy>(
          corner_,
          slack_threshold_,
          mcts_iters_,
          mcts_max_depth_,
          bfs_hops_,
          max_instances_per_cone_,
          /*seed=*/0,
          mcts_ucb_constant_);
    }

    for (size_t i = 0; i < cone_groups.size(); i++) {
      const ConeData& cone = cone_groups[i];

      logger->info(RMP, 402,
                   "CPA-remap: round {} cone {}/{}: {} ep  "
                   "pi={} po={} insts={} budget={:.4f}",
                   round, i + 1, cone_groups.size(),
                   cone.endpoints.size(),
                   cone.pi_arrivals.size(),
                   cone.po_requireds.size(),
                   static_cast<int>(cone.cut.cut_instances().size()),
                   cone.delay_budget);

      std::vector<GiaOp> ops;
      if (mcts_strategy) {
        // MCTS: find best op sequence using per-PI/PO timing constraints.
        // OptimizeCone already calls RunGiaConeSlack with cone.pi_arrivals and
        // cone.po_requireds internally — this is the paper's read_constr approach.
        try {
          ops = mcts_strategy->OptimizeCone(
              cone, all_ops, abc_library, sta, logger);
        } catch (const std::exception& ex) {
          debugPrint(logger, RMP, "cone_mcts", 1,
                     "CPA-remap: cone {}/{}: MCTS failed ({}), using empty ops.",
                     i + 1, cone_groups.size(), ex.what());
        }
      }
      // (empty ops = balance + map -D budget only)

      double abc_slack = 0.0;  // safe default: To - Tr = 0 → no improvement
      try {
        abc_slack = RunGiaConeSlack(
            sta,
            const_cast<cut::LogicCut&>(cone.cut),
            abc_library,
            ops,
            cone.pi_arrivals,
            cone.po_requireds,
            cone.liberty_time_scale,
            cone.delay_budget,
            logger);
      } catch (const std::exception& ex) {
        debugPrint(logger, RMP, "cone_mcts", 1,
                   "CPA-remap: cone {}/{}: RunGiaConeSlack failed ({}), skipping.",
                   i + 1, cone_groups.size(), ex.what());
        should_apply[i] = false;
        best_ops_vec[i] = std::move(ops);
        continue;
      }

      // abc_slack = To - Tr: positive means cone delay improved.
      //
      // For violated cones (delay_budget < 0), require the ABC improvement to
      // exceed the violation magnitude.  A tiny improvement (e.g., 1 ps) on a
      // deeply violated cone is within ABC's modelling error relative to real
      // parasitics and tends to cause WNS regression.  For cones that already
      // meet timing (delay_budget >= 0), any positive improvement is worth
      // taking.
      const double accept_threshold = std::max(0.0, -cone.delay_budget);
      const bool   accept           = abc_slack > accept_threshold;
      should_apply[i]   = accept;
      best_ops_vec[i]   = std::move(ops);

      if (accept) {
        logger->info(RMP, 416,
                     "CPA-remap: round {} cone {}/{}: ACCEPT  "
                     "delay_improvement={:.4f}  threshold={:.4f}  "
                     "budget={:.4f}  ops={}",
                     round, i + 1, cone_groups.size(),
                     abc_slack, accept_threshold,
                     cone.delay_budget,
                     best_ops_vec[i].size());
      }
      debugPrint(logger, RMP, "cone_mcts", 1,
                 "CPA-remap: cone {}/{}: delay_improvement={:.4f}  "
                 "threshold={:.4f}  budget={:.4f}  {}",
                 i + 1, cone_groups.size(),
                 abc_slack, accept_threshold,
                 cone.delay_budget,
                 accept ? "ACCEPT" : "skip");
    }

    // ---- Apply accepted cones to ODB (worst-slack first, up to limit) ----
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
        if (!should_apply[i]) {
          continue;
        }

        // Enforce max_cones_per_round_ limit.
        if (max_cones_per_round_ > 0
            && round_applied >= max_cones_per_round_) {
          break;
        }

        // Skip if any instance in this cone was already deleted by an earlier
        // applied cone in this round.
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
          logger->info(RMP, 403,
                       "CPA-remap: round {} cone {}/{} skipped "
                       "(instances consumed by prior cone).",
                       round, i + 1, cone_groups.size());
          continue;
        }

        logger->info(RMP, 404,
                     "CPA-remap: round {} applying cone {}/{} "
                     "({} eps, {} ops).",
                     round, i + 1, cone_groups.size(),
                     cone_groups[i].endpoints.size(),
                     best_ops_vec[i].size());

        try {
          ApplyConeOps(sta,
                       cone_groups[i].cut,
                       abc_library,
                       best_ops_vec[i],
                       cone_groups[i].pi_arrivals,
                       cone_groups[i].po_requireds,
                       cone_groups[i].liberty_time_scale,
                       cone_groups[i].delay_budget,
                       name_generator,
                       logger);
          ++round_applied;
        } catch (const std::exception& ex) {
          debugPrint(logger, RMP, "cone_mcts", 1,
                     "CPA-remap: cone {}/{} apply failed ({}), skipping.",
                     i + 1, cone_groups.size(), ex.what());
        }
      }

      if (apply_guard) {
        apply_guard->update();
      }
    }

    total_applied += round_applied;

    if (round_applied == 0) {
      logger->info(RMP, 405,
                   "CPA-remap: round {}: no cone applied — stopping.", round);
      break;
    }

    const int dpl_fails = LegalizeUnplaced(resizer, sta, logger);
    if (dpl_fails > 0) {
      logger->warn(RMP, 406,
                   "CPA-remap: round {}: DPL failed to place {} cell(s).",
                   round, dpl_fails);
    }

    logger->info(RMP, 407,
                 "CPA-remap: round {}: applied {} cone(s)  DPL done.",
                 round, round_applied);
  }

  if (total_applied == 0) {
    logger->info(RMP, 408, "CPA-remap: no cones applied — nothing changed.");
    return;
  }

  // ---- repair_setup once on the final design ----
  logger->info(RMP, 409, "CPA-remap: running repair_setup.");
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
      logger->info(RMP, 410, "CPA-remap: repair_setup completed.");
    } catch (const std::exception& e) {
      logger->warn(RMP, 411, "CPA-remap: repair_setup failed: {}.", e.what());
    } catch (...) {
      logger->warn(RMP, 412, "CPA-remap: repair_setup failed (unknown).");
    }

    if (repair_guard) {
      repair_guard->update();
    }

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

  logger->info(RMP, 413,
               "CPA-remap: done ({} rounds, {} cones applied).  "
               "WNS={:.4f} ps  TNS={:.4f} ps",
               round, total_applied,
               static_cast<double>(wns) * 1e12,
               static_cast<double>(tns) * 1e12);
} catch (const std::exception& ex) {
  // Top-level safety net: any C++ exception that escapes the inner per-cone
  // try-catch blocks is caught here and reported as a warning.  This prevents
  // CPA-remap from crashing OpenROAD / the Tcl interpreter.
  logger->warn(RMP, 414,
               "CPA-remap: aborted due to unexpected error: {}.  "
               "Design may be partially modified.",
               ex.what());
} catch (...) {
  logger->warn(RMP, 415,
               "CPA-remap: aborted due to unknown error.  "
               "Design may be partially modified.");
}

}  // namespace rmp
