// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#include "gia.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iomanip>
// std::numeric_limits is used when initialising min_required / max_arrival
// for the per-path cone delay budget computation below.
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "aig/aig/aig.h"
#include "aig/gia/giaAig.h"
#include "base/abc/abc.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "map/if/if.h"
#pragma GCC diagnostic pop
#include "aig/gia/gia.h"
#include "cut/abc_library_factory.h"
#include "cut/logic_cut.h"
#include "cut/logic_extractor.h"
#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "map/mio/mio.h"
#include "map/scl/sclLib.h"
#include "map/scl/sclSize.h"
#include "misc/extra/extra.h"
#include "misc/nm/nm.h"
#include "misc/util/abc_global.h"
#include "misc/vec/vecPtr.h"
#include "odb/db.h"
#include "proof/dch/dch.h"
#include "sta/Graph.hh"
#include "sta/GraphDelayCalc.hh"
#include "sta/Liberty.hh"
#include "sta/PortDirection.hh"
#include "sta/Search.hh"
#include "sta/Units.hh"
#include "utils.h"
#include "utl/Logger.h"
#include "utl/SuppressStdout.h"
#include "utl/deleter.h"
#include "utl/unique_name.h"

// Provides Abc_Frame_t typedef, Abc_FrameGetGlobalFrame, Cmd_CommandExecute.
#include "base/main/abcapis.h"

namespace abc {
// NOLINTBEGIN(readability-identifier-naming)
extern void       Io_WriteBlif(Abc_Ntk_t* pNtk, char* pFileName,
                               int fWriteLatches, int fBb2Wb, int fSeq);
extern Abc_Ntk_t* Abc_FrameReadNtk(Abc_Frame_t* p);
extern void*      Abc_FrameReadLibGen();
extern void       Mio_WriteLibrary(FILE* pFile, Mio_Library_t* pLib,
                                   int fPrintSops, int fShort, int fSelected);
extern Abc_Ntk_t* Abc_NtkFromAigPhase(Aig_Man_t* pMan);
extern Abc_Ntk_t* Abc_NtkFromCellMappedGia(Gia_Man_t* p, int fUseBuffs);
extern Abc_Ntk_t* Abc_NtkFromDarChoices(Abc_Ntk_t* pNtkOld, Aig_Man_t* pMan);
extern Abc_Ntk_t* Abc_NtkFromMappedGia(Gia_Man_t* p,
                                       int fFindEnables,
                                       int fUseBuffs);
extern Abc_Ntk_t* Abc_NtkMap(Abc_Ntk_t* pNtk,
                             Mio_Library_t* userLib,
                             double DelayTarget,
                             double AreaMulti,
                             double DelayMulti,
                             float LogFan,
                             float Slew,
                             float Gain,
                             int nGatesMin,
                             int fRecovery,
                             int fSwitching,
                             int fSkipFanout,
                             int fUseProfile,
                             int fUseBuffs,
                             int fVerbose);
extern Aig_Man_t* Abc_NtkToDar(Abc_Ntk_t* pNtk, int fExors, int fRegisters);
extern Aig_Man_t* Abc_NtkToDarChoices(Abc_Ntk_t* pNtk);
extern Gia_Man_t* Gia_ManAigSynch2(Gia_Man_t* p,
                                   void* pPars,
                                   int nLutSize,
                                   int nRelaxRatio);
extern Gia_Man_t* Gia_ManCheckFalse(Gia_Man_t* p,
                                    int nSlackMax,
                                    int nTimeOut,
                                    int fVerbose,
                                    int fVeryVerbose);
extern Vec_Ptr_t* Abc_NtkCollectCiNames(Abc_Ntk_t* pNtk);
extern Vec_Ptr_t* Abc_NtkCollectCoNames(Abc_Ntk_t* pNtk);
extern void Abc_NtkRedirectCiCo(Abc_Ntk_t* pNtk);
extern void Abc_FrameSetLibGen(void* pLib);
// NOTE: Abc_SclDeriveGenlib, Mio_LibraryDelete, Abc_NtkTimeSetArrival,
// Abc_NtkTimeSetRequired, Abc_NodeReadArrivalWorst, and Abc_NtkDelayTrace
// are all declared in the abc namespace via #include "base/abc/abc.h" above
// (through cut/abc_library_factory.h).  No redundant extern needed here.
// NOLINTEND(readability-identifier-naming)
}  // namespace abc

namespace rmp {
using utl::RMP;

static void replaceGia(GiaOp::AigManager& gia, abc::Gia_Man_t* new_gia)
{
  // Operation does not allocate new gia
  if (gia.get() == new_gia) {
    return;
  }
  if (gia->vNamesIn && !new_gia->vNamesIn) {
    std::swap(gia->vNamesIn, new_gia->vNamesIn);
  }
  if (gia->vNamesOut && !new_gia->vNamesOut) {
    std::swap(gia->vNamesOut, new_gia->vNamesOut);
  }
  if (gia->vNamesNode && !new_gia->vNamesNode) {
    std::swap(gia->vNamesNode, new_gia->vNamesNode);
  }
  gia.reset(new_gia);
}

std::vector<GiaOp> GiaOps(utl::Logger* logger)
{
  // GIA ops as lambdas
  // All the magic numbers are defaults from abc/src/base/abci/abc.c
  // Or from the ORFS abc_speed script
  auto step = [logger](const char* msg, auto func) {
    return [logger, msg, func](auto& gia) {
      debugPrint(logger, RMP, "gia", 1, msg);
      replaceGia(gia, func(gia));
    };
  };

  // Trimmed to 7 ops for pure combinational timing optimization.
  //
  // Removed:
  //   &st     — redundant cleanup; syn4 normalizes internally
  //   &syn2   — Lf-mapper flow; subsumed by syn4+synch2 for timing focus
  //   &syn3   — Jf K=6/K=4 without Fx; strictly dominated by syn4
  //   &retime — moves flip-flops; complete no-op on combinational cones
  //   &b      — Gia_ManAreaBalance with ABC_INFINITY depth limit;
  //             area-first balance trades depth for nodes — counterproductive
  //             for timing optimization
  std::vector<GiaOp::OpExecutor> all_ops = {
      // 0: &dch — SAT-based equivalence discovery + reduce.
      // Pattern-matching in syn4 cannot find deep structural equivalences;
      // SAT sweeping can. Strongest structural redundancy remover.
      [logger](auto& g) {
        if (!g->pReprs) {
          debugPrint(
              logger, RMP, "gia", 1, "Computing choices before equiv reduce");
          abc::Dch_Pars_t pars = {};
          Dch_ManSetDefaultParams(&pars);
          replaceGia(g, Gia_ManPerformDch(g.get(), &pars));
        }
        debugPrint(logger, RMP, "gia", 1, "Starting equiv reduce");
        replaceGia(g, Gia_ManEquivReduce(g.get(), true, false, false, false));
      },
      // 1: &syn4 — primary structural workhorse for combinational timing.
      // Internally: AreaBalance → Jf_Map(K=7) → Fx → AreaBalance
      //           → Jf_Map(K=5) → Fx → AreaBalance.
      // Fx (factor-graph extraction) finds shared sub-expressions that
      // rewriting/SAT both miss. Strictly more powerful than syn2/syn3.
      step("Starting syn4",
           [](auto& g) { return Gia_ManAigSyn4(g.get(), false, false); }),
      // 2: &dc2 — balance-interleaved rewriting (Compress2).
      // Uses DAG-aware rewriting inside repeated balance steps — a different
      // structural axis from syn4's LUT mapping + factorization. Finds local
      // node merges that global factorization misses on irregular cones.
      step("Starting heavy rewriting",
           [](auto& g) { return Gia_ManCompress2(g.get(), true, false); }),
      // 3: &b -d — depth-first AIG balance.
      // Directly minimizes critical path length (level count). syn4's
      // internal steps use Gia_ManAreaBalance (area-priority); this op
      // uses Gia_ManBalance (depth-priority) — a different objective.
      // Essential for lowering the depth floor before Abc_NtkMap.
      step(
          "Starting &b -d (depth balance)",
          [](auto& g) { return Gia_ManBalance(g.get(), false, false, false); }),
      // 4: &reduce — SAT equiv reduce + immediate remap in one pass.
      // Strictly more powerful than &dch alone: after merging equiv nodes
      // it immediately remaps to exploit the reduced structure. Best
      // single-op for combinational cones with redundant sub-expressions.
      [logger](auto& g) {
        if (!g->pReprs) {
          debugPrint(
              logger, RMP, "gia", 1, "Computing choices before equiv reduce");
          abc::Dch_Pars_t pars = {};
          Dch_ManSetDefaultParams(&pars);
          replaceGia(g, Gia_ManPerformDch(g.get(), &pars));
        }
        debugPrint(logger, RMP, "gia", 1, "Starting equiv reduce and remap");
        replaceGia(g, Gia_ManEquivReduceAndRemap(g.get(), true, false));
      },
      // 5: &if -g -K 6 — delay-optimized LUT mapping (fDelayOpt=true).
      // The ONLY op with a genuine timing objective in cut selection.
      // syn4's Jf mapper uses nRelaxRatio=40 (area-biased); this uses
      // fDelayOpt=true which drives depth minimization during cut enum.
      [logger](auto& g) {
        if (Gia_ManHasMapping(g.get())) {
          debugPrint(logger,
                     RMP,
                     "gia",
                     1,
                     "GIA has mapping - rehashing before mapping");
          replaceGia(g, Gia_ManRehash(g.get(), false));
        }
        abc::If_Par_t pars = {};
        Gia_ManSetIfParsDefault(&pars);
        pars.fDelayOpt = true;
        pars.nLutSize = 6;
        pars.fTruth = true;
        pars.fCutMin = true;
        pars.fExpRed = false;
        debugPrint(logger, RMP, "gia", 1, "Starting SOP balancing (&if -g -K 6)");
        replaceGia(g, Gia_ManPerformMapping(g.get(), &pars));
      },
      // 6: &synch2 — SAT sweeping + LUT synthesis co-optimization.
      // Most powerful single op for combinational timing: runs bounded SAT
      // (100 BT limit) to find deep structural equivalences, then
      // synthesizes into K=6 LUT network with 20% size relaxation.
      // Combines the power of &dch with LUT-aware structural optimization.
      [logger](auto& g) {
        abc::Dch_Pars_t pars = {};
        Dch_ManSetDefaultParams(&pars);
        pars.nBTLimit = 100;
        debugPrint(logger, RMP, "gia", 1, "Starting synch2");
        replaceGia(g, Gia_ManAigSynch2(g.get(), &pars, 6, 20));
      },
      // 7: &sopb — SOP-based balancing (nCutNum=10, delay-optimal).
      // Works at the truth-table / SOP level — a structurally different axis
      // from all AIG-level ops (dch, syn4, dc2, b-d, reduce, if-K6, synch2).
      // &sopb decomposes each node into a sum-of-products and re-balances the
      // resulting tree for minimum depth, finding restructurings that AIG
      // rewriting misses.  Fast and safe — no SAT calls.
      step("Starting &sopb (SOP depth balance)",
           [](auto& g) {
             return Gia_ManPerformSopBalance(g.get(), 10, 0, false);
           }),
      // ops 8 (&syn2 -d) and 9 (&dsdb) were added 2026-05-24 and removed due to
      // Signal 11 crashes in RunGia.  Gia_ManAigSyn2 with fDelayMin internally
      // calls Lf_ManPerformMapping which modifies the GIA in-place, leaving a
      // LUT mapping on the GIA that subsequent Gia_ManBalance or Gia_ManToAig
      // calls cannot safely handle.  Gia_ManPerformDsdBalance uses a global
      // ABC DSD manager (Abc_FrameSetManDsd2) that accumulates state across
      // repeated calls and corrupts on iteration 5+.
  };

  // NOTE: ops 8-9 removed — back to 8 ops (0-7).
  std::vector<GiaOp> ops;
  ops.reserve(all_ops.size());
  for (size_t i = 0; i < all_ops.size(); ++i) {
    ops.emplace_back(i, all_ops[i]);
  }
  return ops;
}

static GiaOp::AigManager initGia(abc::Abc_Ntk_t* ntk, utl::Logger* logger)
{
  debugPrint(logger, RMP, "gia", 1, "Converting to GIA");
  assert(!Abc_NtkIsStrash(ntk));
  // derive comb GIA
  auto strash = WrapUnique(Abc_NtkStrash(ntk, false, true, false));
  auto aig = WrapUnique(Abc_NtkToDar(strash.get(), false, false));
  abc::Gia_Man_t* gia = Gia_ManFromAig(aig.get());
  // perform undc/zero
  auto inits = Abc_NtkCollectLatchValuesStr(ntk);
  {
    auto temp = WrapUnique(gia);
    gia = Gia_ManDupZeroUndc(gia, inits, 0, false, false);
  }
  ABC_FREE(inits);
  // copy names
  gia->vNamesIn = abc::Abc_NtkCollectCiNames(ntk);
  gia->vNamesOut = abc::Abc_NtkCollectCoNames(ntk);
  return WrapUnique(gia);
}

// PO-driver and internal-cell "upsize to strongest equiv" sizing was
// replaced by a bounded rsz::repairSetup pass inside
// SolutionSlack::Evaluate (slack_tuning_strategy.cpp, step 2b).  rsz sizes
// cells based on real STA slack/slew rather than blindly picking the
// strongest equiv — strictly more accurate.  Kept here, unused, behind
// [[maybe_unused]] for reference and to ease re-enabling if needed.
[[maybe_unused]] static void UpsizePrimaryOutputDrivers(const cut::LogicCut& cut,
                                       sta::dbSta*          sta,
                                       utl::Logger*         logger)
{
  sta::dbNetwork* network = sta->getDbNetwork();

  // Sta::equivCells() returns nullptr for every cell until makeEquivCells()
  // is called.  rsz calls it lazily before gate swapping, but rmp can run
  // without rsz (e.g. resynth_annealing before repair_design).  Detect
  // whether the table exists by probing cells from all loaded libraries;
  // if none return a non-null equiv list, build the table now.
  //
  // The probe is cheap (O(#libs × early-exit)) on subsequent SA iterations
  // because the first non-null equiv list causes an immediate break.
  {
    bool needs_build = true;
    sta::LibertyLibraryIterator* probe_lib_it
        = network->libertyLibraryIterator();
    while (probe_lib_it->hasNext() && needs_build) {
      sta::LibertyLibrary* lib = probe_lib_it->next();
      sta::LibertyCellIterator cell_it(lib);
      while (cell_it.hasNext()) {
        if (sta->equivCells(cell_it.next()) != nullptr) {
          needs_build = false;
          break;
        }
      }
    }
    delete probe_lib_it;

    if (needs_build) {
      sta::LibertyLibrarySeq libs;
      sta::LibertyLibraryIterator* it = network->libertyLibraryIterator();
      while (it->hasNext()) {
        libs.emplace_back(it->next());
      }
      delete it;
      sta->makeEquivCells(&libs, nullptr);
      debugPrint(logger,
                 RMP,
                 "sa_eval",
                 1,
                 "PO driver upsize: built equiv cell table from {} liberty "
                 "lib(s) (first call — rsz had not been run yet)",
                 libs.size());
    }
  }

  int upsized          = 0;
  int skipped_no_drvr  = 0;  // no driver or >1 driver on PO net
  int skipped_external = 0;  // driver is outside the cone (feed-through PI)
  int skipped_no_equiv = 0;  // no equiv cell with better drive resistance

  for (sta::Net* po_net : cut.primary_outputs()) {
    sta::PinSet* drivers = network->drivers(po_net);
    if (!drivers || drivers->size() != 1) {
      ++skipped_no_drvr;
      continue;
    }

    const sta::Pin* drvr_pin  = *drivers->begin();
    sta::Instance*  drvr_inst = network->instance(drvr_pin);

    // Only upsize instances that were inserted by ABC — skip external drivers
    // (feed-through paths where a PI net is also a PO net).
    if (cut.cut_instances().find(drvr_inst) == cut.cut_instances().end()) {
      ++skipped_external;
      continue;
    }

    sta::LibertyCell* current_cell = network->libertyCell(drvr_inst);
    if (!current_cell) {
      ++skipped_no_equiv;
      continue;
    }

    sta::LibertyPort* drvr_lib_port = network->libertyPort(drvr_pin);
    if (!drvr_lib_port) {
      ++skipped_no_equiv;
      continue;
    }
    const float current_drive_res = drvr_lib_port->driveResistance();

    sta::LibertyCellSeq* equiv_cells = sta->equivCells(current_cell);
    if (!equiv_cells || equiv_cells->empty()) {
      ++skipped_no_equiv;
      continue;
    }

    sta::LibertyCell* best_cell     = current_cell;
    float             best_drive_res = current_drive_res;

    for (sta::LibertyCell* equiv : *equiv_cells) {
      if (equiv == current_cell) {
        continue;
      }
      sta::LibertyPort* equiv_out
          = equiv->findLibertyPort(drvr_lib_port->name());
      if (!equiv_out) {
        continue;
      }
      const float equiv_drive_res = equiv_out->driveResistance();
      if (equiv_drive_res < best_drive_res) {
        best_drive_res = equiv_drive_res;
        best_cell      = equiv;
      }
    }

    if (best_cell == current_cell) {
      ++skipped_no_equiv;
      continue;
    }

    sta->replaceCell(drvr_inst, best_cell);
    ++upsized;
  }

  debugPrint(logger,
             RMP,
             "sa_eval",
             1,
             "PO driver upsize: {}/{} output drivers upsized to strongest "
             "equiv cell  (skipped: {}/no-driver  {}/feed-through  "
             "{}/already-strongest)",
             upsized,
             cut.primary_outputs().size(),
             skipped_no_drvr,
             skipped_external,
             skipped_no_equiv);

  // Second pass: upsize ALL remaining internal cut cells (those not driving a
  // PO net directly).  ABC's minimum-area fallback maps to the smallest cell
  // in each equivalence group (e.g. INVx1, NAND2x1).  These minimum-sized
  // internal cells drive higher fanout than the original design (fewer total
  // cells → more fanout per cell) and produce massive real-STA delay under
  // wire RC, causing catastrophic WNS regression that SA rejects every time.
  // PO drivers already replaced above now read back as the upsized cell, so
  // equivCells() finds no stronger option and they are skipped harmlessly.
  int internal_upsized = 0;
  int internal_skipped = 0;

  for (const sta::Instance* const_inst : cut.cut_instances()) {
    sta::Instance* inst = const_cast<sta::Instance*>(const_inst);
    sta::LibertyCell* current_cell = network->libertyCell(inst);
    if (!current_cell) {
      ++internal_skipped;
      continue;
    }

    // Find the first output port — ABC maps to single-output standard cells
    // (INV, NAND2, NOR2, AND2, …) so the first output port is the only one.
    sta::LibertyPort* out_port = nullptr;
    sta::LibertyCellPortIterator port_it(current_cell);
    while (port_it.hasNext()) {
      sta::LibertyPort* lp = port_it.next();
      if (lp->direction()->isOutput()) {
        out_port = lp;
        break;
      }
    }
    if (!out_port) {
      ++internal_skipped;
      continue;
    }

    sta::LibertyCellSeq* equiv_cells = sta->equivCells(current_cell);
    if (!equiv_cells || equiv_cells->empty()) {
      ++internal_skipped;
      continue;
    }

    sta::LibertyCell* best_cell      = current_cell;
    float             best_drive_res = out_port->driveResistance();

    for (sta::LibertyCell* equiv : *equiv_cells) {
      if (equiv == current_cell) {
        continue;
      }
      sta::LibertyPort* equiv_out = equiv->findLibertyPort(out_port->name());
      if (!equiv_out) {
        continue;
      }
      const float equiv_drive_res = equiv_out->driveResistance();
      if (equiv_drive_res < best_drive_res) {
        best_drive_res = equiv_drive_res;
        best_cell      = equiv;
      }
    }

    if (best_cell == current_cell) {
      ++internal_skipped;
      continue;
    }

    sta->replaceCell(inst, best_cell);
    ++internal_upsized;
  }

  debugPrint(logger,
             RMP,
             "sa_eval",
             1,
             "Internal cell upsize: {}/{} cut instances upsized to strongest "
             "equiv cell  ({} already-strongest/no-equiv)",
             internal_upsized,
             cut.cut_instances().size(),
             internal_skipped);
}

// ---------------------------------------------------------------------------
// Shared helper: run the GIA structural-transform pipeline on current_network.
// On entry current_network must be a logic network (not strashed).
// On exit current_network is the network rebuilt from GIA (not yet strashed).
// ---------------------------------------------------------------------------
static void RunGiaPipeline(
    utl::UniquePtrWithDeleter<abc::Abc_Ntk_t>& current_network,
    const std::vector<GiaOp>& gia_ops,
    utl::Logger* logger)
{
  auto gia = initGia(current_network.get(), logger);

  replaceGia(gia, abc::Gia_ManBalance(gia.get(), false, false, false));
  const int depth_after_balance = abc::Gia_ManLevelNum(gia.get());

  for (auto& op : gia_ops) {
    op.op(gia);
  }

  if (abc::Gia_ManLevelNum(gia.get()) > depth_after_balance) {
    replaceGia(gia, abc::Gia_ManBalance(gia.get(), false, false, false));
  }

  if (Gia_ManHasCellMapping(gia.get())) {
    current_network = WrapUnique(abc::Abc_NtkFromCellMappedGia(gia.get(), false));
  } else if (Gia_ManHasMapping(gia.get()) || gia->pMuxes) {
    current_network = WrapUnique(Abc_NtkFromMappedGia(gia.get(), false, false));
  } else {
    if (Gia_ManHasDangling(gia.get()) != 0) {
      replaceGia(gia, Gia_ManRehash(gia.get(), false));
    }
    auto aig = WrapUnique(abc::Gia_ManToAig(gia.get(), false));
    current_network = WrapUnique(Abc_NtkFromAigPhase(aig.get()));
    current_network->pName = abc::Extra_UtilStrsav(aig->pName);
  }

  assert(gia->vNamesIn);
  for (int i = 0; i < abc::Abc_NtkCiNum(current_network.get()); i++) {
    assert(i < Vec_PtrSize(gia->vNamesIn));
    abc::Abc_Obj_t* obj = abc::Abc_NtkCi(current_network.get(), i);
    Nm_ManDeleteIdName(current_network->pManName, obj->Id);
    Abc_ObjAssignName(
        obj, static_cast<char*>(Vec_PtrEntry(gia->vNamesIn, i)), nullptr);
  }
  assert(gia->vNamesOut);
  for (int i = 0; i < abc::Abc_NtkCoNum(current_network.get()); i++) {
    assert(i < Vec_PtrSize(gia->vNamesOut));
    abc::Abc_Obj_t* obj = Abc_NtkCo(current_network.get(), i);
    Nm_ManDeleteIdName(current_network->pManName, obj->Id);
    assert(Abc_ObjIsPo(obj));
    Abc_ObjAssignName(
        obj, static_cast<char*>(Vec_PtrEntry(gia->vNamesOut, i)), nullptr);
  }
  if (!Abc_NtkIsStrash(current_network.get())
      && (gia->vNamesIn || gia->vNamesOut)) {
    abc::Abc_NtkRedirectCiCo(current_network.get());
  }
}

// ---------------------------------------------------------------------------
// Set per-CI arrival and per-CO required times on an ABC network using
// name-keyed maps (Liberty time units, matching Abc_SclDeriveGenlib output).
// ---------------------------------------------------------------------------
static void SetPerCiArrivals(
    abc::Abc_Ntk_t* pNtk,
    const std::unordered_map<std::string, float>& pi_arrivals)
{
  for (int i = 0; i < abc::Abc_NtkCiNum(pNtk); i++) {
    abc::Abc_Obj_t* obj = abc::Abc_NtkCi(pNtk, i);
    const char* name = Abc_ObjName(obj);
    if (!name) {
      continue;
    }
    auto it = pi_arrivals.find(std::string(name));
    if (it == pi_arrivals.end()) {
      continue;
    }
    abc::Abc_NtkTimeSetArrival(pNtk, obj->Id, it->second, it->second);
  }
}

static void SetPerCoRequireds(
    abc::Abc_Ntk_t* pNtk,
    const std::unordered_map<std::string, float>& po_requireds)
{
  for (int i = 0; i < abc::Abc_NtkCoNum(pNtk); i++) {
    abc::Abc_Obj_t* obj = Abc_NtkCo(pNtk, i);
    const char* name = Abc_ObjName(obj);
    if (!name) {
      continue;
    }
    auto it = po_requireds.find(std::string(name));
    if (it == po_requireds.end()) {
      continue;
    }
    abc::Abc_NtkTimeSetRequired(pNtk, obj->Id, it->second, it->second);
  }
}

// ---------------------------------------------------------------------------
// GetMaxCoArrival — measure the worst-case CO arrival through a mapped ABC
// network after setting per-CI arrivals from pi_arrivals (global STA times).
//
// Using global STA arrival times (not all-zero) makes the "Tr < To"
// comparison timing-aware: ABC can reroute computation through less-critical
// PIs, which is the key mechanism described in the CPA-remap paper.
// ---------------------------------------------------------------------------
static double GetMaxCoArrival(
    abc::Abc_Ntk_t*                                  pNtk,
    const std::unordered_map<std::string, float>&    pi_arrivals)
{
  SetPerCiArrivals(pNtk, pi_arrivals);
  abc::Abc_NtkDelayTrace(pNtk, nullptr, nullptr, 0);

  double max_arr = 0.0;
  for (int i = 0; i < abc::Abc_NtkCoNum(pNtk); i++) {
    abc::Abc_Obj_t* co      = abc::Abc_NtkCo(pNtk, i);
    abc::Abc_Obj_t* driver  = abc::Abc_ObjFanin0(co);
    if (!driver) {
      continue;
    }
    const double arr = static_cast<double>(abc::Abc_NodeReadArrivalWorst(driver));
    if (arr > max_arr) {
      max_arr = arr;
    }
  }
  return max_arr;
}

// ---------------------------------------------------------------------------
// RunGiaConeSlack — pure-ABC cone evaluation (no ODB writes).
//
// Pipeline:
//   1. BuildMappedAbcNetwork from pre-extracted cut.
//   2. Clone the logic network → baseline (no GIA ops); map → measure To.
//   3. Apply gia_ops to original logic network; strash; map → measure Tr.
//   4. Both To and Tr are measured using global pi_arrivals (timing-aware).
//   5. Return To - Tr.
//
// Return value:
//   > 0 → cone CO arrival improved (Tr < To) → ACCEPT.
//   ≤ 0 → no improvement or degradation → skip.
//
// Using global pi_arrivals (not all-zero) is the key: it allows the mapper
// to exploit slack on less-critical PIs, matching the paper's "read_constr"
// mechanism.  MCTS uses the return value as its reward.
// ---------------------------------------------------------------------------
double RunGiaConeSlack(
    sta::dbSta* sta,
    cut::LogicCut& cut,
    cut::AbcLibrary& abc_library,
    const std::vector<GiaOp>& gia_ops,
    const std::unordered_map<std::string, float>& pi_arrivals,
    const std::unordered_map<std::string, float>& po_requireds,
    float /*liberty_time_scale*/,  // pi_arrivals/po_requireds already in Liberty units
    double delay_budget_fallback,
    utl::Logger* logger)
{
  // No structural ops → baseline and optimised networks are identical.
  // Two separate Abc_NtkMap calls on identical input can produce marginally
  // different delays due to internal heuristics, so Tr ≈ To only within
  // mapping noise.  Return 0 (no improvement) rather than accepting noise.
  if (gia_ops.empty()) {
    return 0.0;
  }

  sta::dbNetwork* network = sta->getDbNetwork();

  utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> mapped_abc_network
      = cut.BuildMappedAbcNetwork(abc_library, network, logger);
  if (!mapped_abc_network) {
    return -std::numeric_limits<double>::infinity();
  }

  // Must register the Mio library in the global ABC frame BEFORE calling
  // Abc_NtkToLogic: the conversion calls Abc_NtkLogicMakeSimpleCos →
  // Abc_NtkCreateNodeBuf → Mio_LibraryReadBuf(pLibGen), which crashes if
  // pLibGen is null (i.e., no prior GENLIB load set it).
  auto* library
      = static_cast<abc::Mio_Library_t*>(mapped_abc_network->pManFunc);
  abc::Abc_FrameSetLibGen(library);

  // Derive real_genlib EARLY — it is used for both To and Tr measurements so
  // that both use the same delay model and the comparison is fair.
  // (BuildMappedAbcNetwork uses Abc_SclDeriveGenlibSimple; Abc_NtkMap uses
  // Abc_SclDeriveGenlib.  Measuring To with one and Tr with the other
  // produces systematically negative improvement even for identical cones.)
  abc::Mio_Library_t* real_genlib = abc::Abc_SclDeriveGenlib(
      abc_library.abc_library(), nullptr, 0.0f, 250.0f, 0, false);

  utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> current_network(
      abc::Abc_NtkToLogic(
          const_cast<abc::Abc_Ntk_t*>(mapped_abc_network.get())),
      &abc::Abc_NtkDelete);

  current_network->pManFunc = library;

  // ---- Measure To: baseline delay (no GIA ops, all PIs at t=0) ----
  // Clone the logic network before any GIA-op restructuring.  Map the clone
  // with real_genlib to get the baseline cone delay using the same technology
  // model that Tr will use.  Both To and Tr must use the same library so the
  // comparison "Tr < To" is self-consistent.
  // (BuildMappedAbcNetwork uses Abc_SclDeriveGenlibSimple; that library has
  // systematically smaller delays than real_genlib → measuring To from the
  // original mapped_abc_network would always give To < Tr.)
  utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> baseline_net(
      abc::Abc_NtkDup(current_network.get()), &abc::Abc_NtkDelete);
  if (!baseline_net) {
    return -std::numeric_limits<double>::infinity();
  }
  if (!Abc_NtkIsStrash(baseline_net.get())) {
    baseline_net = WrapUnique(
        abc::Abc_NtkStrash(baseline_net.get(), false, true, false));
  }
  // Set timing constraints on baseline so the mapper uses the same timing
  // context as the optimized network — essential for a fair To vs Tr comparison.
  SetPerCiArrivals(baseline_net.get(), pi_arrivals);
  SetPerCoRequireds(baseline_net.get(), po_requireds);
  {
    utl::SuppressStdout nostdout(logger);
    baseline_net = WrapUnique(abc::Abc_NtkMap(
        baseline_net.get(),
        real_genlib,
        delay_budget_fallback,
        /*AreaMulti=*/0.0,
        /*DelayMulti=*/0.0,
        /*LogFan=*/4.0,
        /*Slew=*/10.0,
        /*Gain=*/0.0,
        /*nGatesMin=*/0,
        /*fRecovery=*/true,
        /*fSwitching=*/false,
        /*fSkipFanout=*/false,
        /*fUseProfile=*/false,
        /*fUseBuffs=*/true,
        /*fVerbose=*/false));
  }
  if (!baseline_net) {
    return -std::numeric_limits<double>::infinity();
  }
  // ---- To: baseline CO arrival using global pi_arrivals ----
  const double to = GetMaxCoArrival(baseline_net.get(), pi_arrivals);

  RunGiaPipeline(current_network, gia_ops, logger);

  if (!Abc_NtkIsStrash(current_network.get())) {
    current_network = WrapUnique(
        abc::Abc_NtkStrash(current_network.get(), false, true, false));
  }

  // Set per-CI/CO timing on the pre-map strashed network so the mapper uses
  // real arrival and required times during delay-aware cut selection.
  SetPerCiArrivals(current_network.get(), pi_arrivals);
  SetPerCoRequireds(current_network.get(), po_requireds);

  {
    utl::SuppressStdout nostdout(logger);
    current_network = WrapUnique(abc::Abc_NtkMap(
        current_network.get(),
        real_genlib,
        delay_budget_fallback,
        /*AreaMulti=*/0.0,
        /*DelayMulti=*/0.0,
        /*LogFan=*/4.0,
        /*Slew=*/10.0,
        /*Gain=*/0.0,
        /*nGatesMin=*/0,
        /*fRecovery=*/true,
        /*fSwitching=*/false,
        /*fSkipFanout=*/false,
        /*fUseProfile=*/false,
        /*fUseBuffs=*/true,
        /*fVerbose=*/false));
  }

  if (!current_network) {
    return -std::numeric_limits<double>::infinity();
  }

  // ---- Tr: remapped CO arrival using global pi_arrivals ----
  const double tr = GetMaxCoArrival(current_network.get(), pi_arrivals);

  // Return To - Tr:
  //   > 0  → remapped cone has lower worst CO arrival (Tr < To)  → ACCEPT
  //   ≤ 0  → no improvement or degradation                       → skip
  return to - tr;
}

// ---------------------------------------------------------------------------
// ApplyConeOps — apply gia_ops to a pre-extracted cut and commit to ODB.
//
// Same ABC pipeline as RunGiaConeSlack but:
//   — invalidates STA timing before ODB modification,
//   — calls InsertMappedAbcNetwork instead of evaluating slack.
// ---------------------------------------------------------------------------
void ApplyConeOps(
    sta::dbSta* sta,
    cut::LogicCut& cut,
    cut::AbcLibrary& abc_library,
    const std::vector<GiaOp>& gia_ops,
    const std::unordered_map<std::string, float>& pi_arrivals,
    const std::unordered_map<std::string, float>& po_requireds,
    float /*liberty_time_scale*/,
    double delay_budget_fallback,
    utl::UniqueName& name_generator,
    utl::Logger* logger)
{
  sta::dbNetwork* network = sta->getDbNetwork();

  // Invalidate STA state before touching ODB.
  sta->graphDelayCalc()->delaysInvalid();
  sta->search()->arrivalsInvalid();
  sta->search()->endpointsInvalid();

  utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> mapped_abc_network
      = cut.BuildMappedAbcNetwork(abc_library, network, logger);
  if (!mapped_abc_network) {
    return;
  }

  // Must register the Mio library BEFORE Abc_NtkToLogic (same reason as
  // RunGiaConeSlack: Abc_NtkLogicMakeSimpleCos needs a non-null pLibGen).
  auto* library
      = static_cast<abc::Mio_Library_t*>(mapped_abc_network->pManFunc);
  abc::Abc_FrameSetLibGen(library);

  utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> current_network(
      abc::Abc_NtkToLogic(
          const_cast<abc::Abc_Ntk_t*>(mapped_abc_network.get())),
      &abc::Abc_NtkDelete);

  current_network->pManFunc = library;

  RunGiaPipeline(current_network, gia_ops, logger);

  if (!Abc_NtkIsStrash(current_network.get())) {
    current_network = WrapUnique(
        abc::Abc_NtkStrash(current_network.get(), false, true, false));
  }

  SetPerCiArrivals(current_network.get(), pi_arrivals);
  SetPerCoRequireds(current_network.get(), po_requireds);

  abc::Mio_Library_t* real_genlib = abc::Abc_SclDeriveGenlib(
      abc_library.abc_library(), nullptr, 0.0f, 250.0f, 0, false);

  {
    utl::SuppressStdout nostdout(logger);
    current_network = WrapUnique(abc::Abc_NtkMap(
        current_network.get(),
        real_genlib,
        delay_budget_fallback,
        /*AreaMulti=*/0.0,
        /*DelayMulti=*/0.0,
        /*LogFan=*/4.0,
        /*Slew=*/10.0,
        /*Gain=*/0.0,
        /*nGatesMin=*/0,
        /*fRecovery=*/true,
        /*fSwitching=*/false,
        /*fSkipFanout=*/false,
        /*fUseProfile=*/false,
        /*fUseBuffs=*/true,
        /*fVerbose=*/false));
  }

  if (!current_network) {
    return;
  }

  abc::Abc_NtkCleanup(current_network.get(), /*fVerbose=*/false);
  current_network = WrapUnique(abc::Abc_NtkDupDfs(current_network.get()));
  current_network = WrapUnique(abc::Abc_NtkToNetlist(current_network.get()));

  cut.InsertMappedAbcNetwork(
      current_network.get(), abc_library, network, name_generator, logger);
}

void RunGia(sta::dbSta* sta,
            const std::vector<sta::Vertex*>& candidate_vertices,
            cut::AbcLibrary& abc_library,
            const std::vector<GiaOp>& gia_ops,
            size_t resize_iters,
            utl::UniqueName& name_generator,
            utl::Logger* logger)
{
  sta::dbNetwork* network = sta->getDbNetwork();

  // Step 1: extract the logic cone (pure graph traversal — no timing values
  // are read here, so this is safe to run before timing invalidation).
  cut::LogicExtractorFactory logic_extractor(sta, logger);
  for (sta::Vertex* negative_endpoint : candidate_vertices) {
    logic_extractor.AppendEndpoint(negative_endpoint);
  }

  cut::LogicCut cut = logic_extractor.BuildLogicCut(abc_library);

  // -----------------------------------------------------------------------
  // Compute the real per-path delay budget for Abc_NtkMap.
  //
  // PROBLEM WITH THE ORIGINAL CODE
  // ================================
  // The original code passed DelayTarget=1.0 to Abc_NtkMap together with a
  // GENLIB built by Abc_SclDeriveGenlibSimple, which assigns every gate —
  // inverter, NAND4, complex AOI — an identical hardcoded delay of 1.0
  // (see third-party/abc/src/map/scl/sclLibUtil.c:798).  With all gates at
  // 1.0, any logic cone with two or more gate levels has a critical path
  // > 1.0, so Abc_NtkMap's required-time check (mapperTime.c:385) fires
  // immediately: "Cannot meet target, continue anyway" — and the target is
  // silently discarded.  ABC maps for minimum depth with no timing motive.
  //
  // FIX — TWO PARTS
  // =================
  // Part 1 (delay budget, computed here):
  //   delay_budget = min(required_at_cone_output) - max(arrival_at_cone_input)
  //
  //   • min_required: tightest deadline across ALL cone output endpoints
  //     (candidate_vertices).  Using all outputs — not just the single
  //     worst-slack vertex — prevents ABC from relaxing any path whose
  //     required time is tighter than the worst-slack vertex's required.
  //
  //   • max_arrival: latest-arriving signal at any cone input boundary
  //     (drivers of cut.primary_inputs() nets).  The cone cannot start
  //     processing until all its inputs have arrived; the latest-arriving
  //     input is the binding constraint.
  //
  //   The result is the actual maximum delay the cone's logic may take
  //   while still meeting timing.  ABC receives a real, binding target.
  //
  // Part 2 (real GENLIB, applied during Abc_NtkMap below):
  //   Abc_SclDeriveGenlib is used instead of the default GENLIB so that
  //   gate delays in the mapper reflect actual Liberty NLDM tables.  With
  //   real delays, delay_budget (in Liberty time units = seconds) is
  //   numerically comparable to what the mapper computes internally.
  //
  // NOTE ON TIMING VALIDITY
  // ========================
  // Timing invalidation (delaysInvalid / arrivalsInvalid / endpointsInvalid)
  // is intentionally deferred until AFTER the delay budget computation below.
  //
  // Calling endpointsInvalid() tears down STA's endpoint search state.  If
  // sta->required() is called afterwards it triggers findRequireds(), which
  // walks the endpoint set and calls dbNetwork::id(pin) on pins that may no
  // longer be valid — causing a segfault (Signal 11 observed in practice).
  //
  // At this point in the function the ODB netlist is still unchanged
  // (BuildLogicCut only reads the graph), so sta->required() and
  // sta->arrival() return correct, up-to-date values.  Timing is invalidated
  // immediately after the budget is computed and before any netlist
  // modifications are made.
  // -----------------------------------------------------------------------

  // --- Step A: tightest required time across all cone output endpoints ---
  // sta::Delay is a class with operator float() but no std::numeric_limits
  // specialisation, so we track the running minimum as a plain float and use
  // sta::Delay's implicit float conversion (Delay.hh:69) for the comparison.
  float min_required_f = std::numeric_limits<float>::max();
  for (sta::Vertex* v : candidate_vertices) {
    // RiseFallBoth::riseFall() queries both rise and fall transitions and
    // returns the tighter (more constraining) required time.
    // MinMax::max() selects the max-delay (setup) analysis.
    const sta::Delay req   = sta->required(v,
                                           sta::RiseFallBoth::riseFall(),
                                           sta->scenes(),
                                           sta::MinMax::max());
    const float      req_f = static_cast<float>(req);  // operator float()
    if (req_f < min_required_f) {
      min_required_f = req_f;
    }
  }

  // --- Step B: latest arrival at all cone input boundary drivers ---
  // cut.primary_inputs() holds the nets that cross into the cone from outside.
  // Each such net is driven by a vertex outside the cone — that driver's
  // arrival time is the moment the signal becomes available at the cone
  // boundary.  We take the maximum across all inputs because the cone must
  // wait for the slowest input before all paths through it can complete.
  float max_arrival_f = std::numeric_limits<float>::lowest();
  for (const sta::Net* net : cut.primary_inputs()) {
    sta::PinSet* drivers = network->drivers(net);
    if (!drivers) {
      continue;
    }
    for (const sta::Pin* driver_pin : *drivers) {
      // Resolve pin → STA vertex so we can query the arrival time.
      // network->vertexId() maps a Liberty pin to its STA graph index;
      // sta->graph()->vertex() dereferences the index to the Vertex object.
      const sta::VertexId vid      = network->vertexId(driver_pin);
      sta::Vertex*        driver_v = sta->graph()->vertex(vid);
      if (!driver_v) {
        continue;  // power/ground or unconnected pin — no STA vertex
      }
      const sta::Arrival arr   = sta->arrival(driver_v,
                                              sta::RiseFallBoth::riseFall(),
                                              sta->scenes(),
                                              sta::MinMax::max());
      const float        arr_f = static_cast<float>(arr);  // operator float()
      if (arr_f > max_arrival_f) {
        max_arrival_f = arr_f;
      }
    }
  }

  // If the cone has no external inputs (e.g., driven entirely by constants
  // that have no STA vertex), max_arrival_f stays at lowest().  Clamp to 0
  // so the budget gracefully degrades to min_required_f alone rather than
  // producing a nonsensically large positive number.
  const double delay_budget_s
      = (min_required_f < std::numeric_limits<float>::max())
            ? static_cast<double>(min_required_f
                                  - std::max(max_arrival_f, 0.0f))
            : 0.0;

  // Convert the budget from STA internal seconds to GENLIB time units.
  //
  // sta->required() / sta->arrival() return values in STA's internal unit
  // (seconds). Abc_SclDeriveGenlib produces gate delays in the Liberty file's
  // time unit (e.g., picoseconds for ASAP7 "time_unit: 1ps").  Passing a
  // budget in seconds to Abc_NtkMap whose internal gate delays are ~10 ps
  // means budget ≈ 6.6e-11 << 10.25 — the mapper sees budget ≈ 0 and falls
  // back to minimum-depth mapping, defeating the entire point of the budget.
  //
  // sta->units()->timeUnit()->scale() is the multiplier from Liberty user
  // units to STA internal seconds (1e-12 for "1ps", 1e-9 for "1ns").
  // Dividing by it converts seconds back to Liberty user units, matching the
  // GENLIB delays that Abc_SclDeriveGenlib wrote in those same user units.
  const double liberty_time_scale
      = static_cast<double>(sta->units()->timeUnit()->scale());
  const double delay_budget
      = (liberty_time_scale > 0.0) ? (delay_budget_s / liberty_time_scale)
                                   : delay_budget_s;

  debugPrint(logger,
             RMP,
             "sa_eval",
             1,
             "Cone delay budget: min_required={:.6e}s  max_arrival={:.6e}s  "
             "budget_s={:.6e}  liberty_scale={:.6e}  budget_genlib={:.4f}  "
             "cone_inputs={}  cone_outputs={}  ({})",
             static_cast<double>(min_required_f),
             static_cast<double>(max_arrival_f),
             delay_budget_s,
             liberty_time_scale,
             delay_budget,
             cut.primary_inputs().size(),
             cut.primary_outputs().size(),
             delay_budget > 0.0
                 ? "positive — ABC has real timing headroom"
                 : (delay_budget == 0.0
                        ? "zero — cone boundary already at limit"
                        : "negative — cone inputs arrive after required; "
                          "ABC must compress"));

  // Step 3: NOW invalidate timing — the budget is computed, and we are about
  // to modify the ODB netlist.  Must happen before BuildMappedAbcNetwork /
  // InsertMappedAbcNetwork so that STA does not see a partially-updated netlist.
  sta->graphDelayCalc()->delaysInvalid();
  sta->search()->arrivalsInvalid();
  sta->search()->endpointsInvalid();

  utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> mapped_abc_network
      = cut.BuildMappedAbcNetwork(abc_library, network, logger);

  // Count original tech cells in the cone BEFORE any ABC transformation.
  // This is the cell count the new mapping must be compared against, not the
  // AIG node count (nodes_before in Abc_NtkMap is AIG nodes, not tech cells).
  const int original_cone_cells
      = abc::Abc_NtkNodeNum(mapped_abc_network.get());
  debugPrint(logger,
             RMP,
             "sa_eval",
             1,
             "Cone cell count: original_tech_cells={}  (before AIG conversion)",
             original_cone_cells);

  utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> current_network(
      abc::Abc_NtkToLogic(
          const_cast<abc::Abc_Ntk_t*>(mapped_abc_network.get())),
      &abc::Abc_NtkDelete);

  auto* library
      = static_cast<abc::Mio_Library_t*>(mapped_abc_network->pManFunc);

  // Install library for NtkMap
  abc::Abc_FrameSetLibGen(library);

  debugPrint(logger,
             RMP,
             "gia",
             1,
             "Mapped ABC network has {} nodes and {} POs.",
             abc::Abc_NtkNodeNum(current_network.get()),
             abc::Abc_NtkPoNum(current_network.get()));

  current_network->pManFunc = library;

  int cone_aig_depth = -1;  // captured from GIA for later GENLIB comparison
  int post_op_depth  = -1;  // depth after Gia_ManBalance + SA ops (ACHIEVABLE check)

  {
    auto gia = initGia(current_network.get(), logger);

    cone_aig_depth = abc::Gia_ManLevelNum(gia.get());
    debugPrint(logger,
               RMP,
               "sa_eval",
               1,
               "AIG cone: depth={}  nodes={}  inputs={}  outputs={}",
               cone_aig_depth,
               abc::Gia_ManAndNum(gia.get()),
               abc::Gia_ManCiNum(gia.get()),
               abc::Gia_ManCoNum(gia.get()));

    // Mandatory depth-minimization pass before any SA-chosen ops.
    //
    // Without this, area-focused ops (rewrite, syn3, dc2, etc.) consistently
    // INCREASE AIG depth (11→13-14) because they trade depth for node count.
    // Abc_NtkMap then receives a deeper AIG and maps it to a worse technology
    // implementation — every candidate fails the hard WNS guard.
    //
    // Gia_ManBalance minimizes depth by restructuring the AIG into a balanced
    // tree.  Running it first guarantees Abc_NtkMap always gets the shallowest
    // achievable AIG, regardless of which area-focused ops follow.
    replaceGia(gia,
               abc::Gia_ManBalance(gia.get(),
                                   /*fSimpleAnd=*/false,
                                   /*fStrict=*/false,
                                   /*fVerbose=*/false));

    const int depth_after_balance = abc::Gia_ManLevelNum(gia.get());
    debugPrint(logger,
               RMP,
               "sa_eval",
               1,
               "AIG after pre-balance: depth={}  nodes={}  (depth_delta={} vs cone)",
               depth_after_balance,
               abc::Gia_ManAndNum(gia.get()),
               depth_after_balance - cone_aig_depth);

    // Run all the given GIA ops (area / structural transforms)
    for (auto& op : gia_ops) {
      op.op(gia);
    }

    // If the SA-chosen ops regressed depth vs. the balanced AIG, re-run
    // Gia_ManBalance to recover.  Area-focused ops (rewrite, refactor, syn3,
    // dc2) frequently trade depth for node count — depth_delta seen at +1, +2
    // in production logs.  Re-balancing is cheap and gives Abc_NtkMap the
    // shallowest achievable AIG even when SA picks a depth-hostile op.
    {
      const int depth_after_ops = abc::Gia_ManLevelNum(gia.get());
      if (depth_after_ops > depth_after_balance) {
        replaceGia(gia,
                   abc::Gia_ManBalance(gia.get(),
                                       /*fSimpleAnd=*/false,
                                       /*fStrict=*/false,
                                       /*fVerbose=*/false));
        debugPrint(logger,
                   RMP,
                   "sa_eval",
                   1,
                   "AIG re-balance after ops: depth {} -> {} (recovered "
                   "from depth-increasing op)",
                   depth_after_ops,
                   abc::Gia_ManLevelNum(gia.get()));
      }
    }

    // Log post-op depth so we can see net effect of balance + SA ops.
    post_op_depth = abc::Gia_ManLevelNum(gia.get());
    debugPrint(logger,
               RMP,
               "sa_eval",
               1,
               "AIG post-op: depth={}  nodes={}  (depth_delta={} vs cone, {} vs balance)",
               post_op_depth,
               abc::Gia_ManAndNum(gia.get()),
               post_op_depth - cone_aig_depth,
               post_op_depth - depth_after_balance);

    debugPrint(logger, RMP, "gia", 1, "Converting GIA to network");
    abc::Extra_UtilGetoptReset();

    if (Gia_ManHasCellMapping(gia.get())) {
      current_network
          = WrapUnique(abc::Abc_NtkFromCellMappedGia(gia.get(), false));
    } else if (Gia_ManHasMapping(gia.get()) || gia->pMuxes) {
      current_network
          = WrapUnique(Abc_NtkFromMappedGia(gia.get(), false, false));
    } else {
      if (Gia_ManHasDangling(gia.get()) != 0) {
        debugPrint(logger, RMP, "gia", 1, "Rehashing before conversion");
        replaceGia(gia, Gia_ManRehash(gia.get(), false));
      }
      assert(Gia_ManHasDangling(gia.get()) == 0);
      auto aig = WrapUnique(abc::Gia_ManToAig(gia.get(), false));
      current_network = WrapUnique(Abc_NtkFromAigPhase(aig.get()));
      current_network->pName = abc::Extra_UtilStrsav(aig->pName);
    }

    assert(gia->vNamesIn);
    for (int i = 0; i < abc::Abc_NtkCiNum(current_network.get()); i++) {
      assert(i < Vec_PtrSize(gia->vNamesIn));
      abc::Abc_Obj_t* obj = abc::Abc_NtkCi(current_network.get(), i);
      assert(obj);
      Nm_ManDeleteIdName(current_network->pManName, obj->Id);
      Abc_ObjAssignName(
          obj, static_cast<char*>(Vec_PtrEntry(gia->vNamesIn, i)), nullptr);
    }
    assert(gia->vNamesOut);
    for (int i = 0; i < abc::Abc_NtkCoNum(current_network.get()); i++) {
      assert(i < Vec_PtrSize(gia->vNamesOut));
      abc::Abc_Obj_t* obj = Abc_NtkCo(current_network.get(), i);
      assert(obj);
      Nm_ManDeleteIdName(current_network->pManName, obj->Id);
      assert(Abc_ObjIsPo(obj));
      Abc_ObjAssignName(
          obj, static_cast<char*>(Vec_PtrEntry(gia->vNamesOut, i)), nullptr);
    }

    // decouple CI/CO with the same name
    if (!Abc_NtkIsStrash(current_network.get())
        && (gia->vNamesIn || gia->vNamesOut)) {
      abc::Abc_NtkRedirectCiCo(current_network.get());
    }
  }

  if (!Abc_NtkIsStrash(current_network.get())) {
    current_network = WrapUnique(
        abc::Abc_NtkStrash(current_network.get(), false, true, false));
  }

  // Build a real-timing GENLIB from the SC_Lib that AbcLibraryFactory
  // populated with actual Liberty NLDM tables.
  //
  // WHY NOT THE DEFAULT PATH (nullptr / Abc_FrameReadLibGen)?
  // When userLib=nullptr, Abc_NtkMap (abcMap.c:71) falls back to the global
  // Mio_Library_t set by Abc_FrameSetLibGen (line 268 above).  That global
  // library was created by Abc_SclDeriveGenlibSimple, which hard-codes
  // every gate's delay to 1.0 (sclLibUtil.c:798) — a unit-delay fiction
  // that discards all Liberty NLDM data.  With every gate at 1.0, any cone
  // with two or more gate levels violates DelayTarget=1.0 immediately, so
  // the mapper silently abandons the target and maps for depth only.
  //
  // WHY Abc_SclDeriveGenlib INSTEAD?
  // Abc_SclDeriveGenlib (sclLibUtil.c:1078) reads the SC_Lib's timing
  // tables and generates a GENLIB where gate delays reflect actual NLDM
  // data scaled by the given Slew and Gain.  Passing this as userLib
  // causes Abc_NtkMap to override its internal pLib with our real-timing
  // library (abcMap.c:92-94), so delay_budget (computed above in Liberty
  // time units = seconds) is numerically meaningful to the mapper.
  //
  // PARAMETER CHOICES
  // SlewInit=0.0: use the auto-computed average slew from the SC_Lib.
  // Gain=250.0:   same value used previously (controls load-capacitance
  //               contribution to gate delay in the GENLIB string).
  // Gain in Abc_NtkMap=0.0: Abc_NtkMap's Gain is only used when it calls
  //               Abc_SclDeriveGenlib internally (the path we are now
  //               bypassing with userLib).  Passing it again would have
  //               no effect since that internal call never happens, but
  //               0.0 documents the intent explicitly.
  // DelayMulti=0.0: do NOT call Mio_LibraryMultiDelay.  With fake 1.0 delays
  //               the 2.5 exponent was a harmless heuristic; with real Liberty
  //               delays it inflates a NAND2 from 12 ps to 67.9 ps, making
  //               every multi-input gate exceed the real budget (~50-66 ps) so
  //               ABC degrades to inverter-only mapping.  Real Liberty delays
  //               already encode the input-count vs. delay trade-off correctly.
  //
  // NOTE: Abc_SclDeriveGenlib is called OUTSIDE the SuppressStdout block
  // because SuppressStdout redirects STDOUT_FILENO (fd 1) to /dev/null, and
  // the OpenROAD logger's spdlog stdout sink writes to fd 1.  Any debugPrint
  // call inside that block is silently discarded.  Only Abc_NtkMap (which
  // always prints to stdout even with fVerbose=false) stays inside the block.
  abc::Mio_Library_t* real_genlib = abc::Abc_SclDeriveGenlib(
      abc_library.abc_library(),
      /*pMio=*/nullptr,
      /*SlewInit=*/0.0f,
      /*Gain=*/250.0f,
      /*nGatesMin=*/0,
      /*fVerbose=*/false);

  // Verify the real GENLIB was created and carries meaningful timing data.
  // gate_count > 0 confirms Liberty tables were read (not the fake 1.0 genlib).
  // inv_delay_max > 0 and << 1.0 confirms delays are real Liberty values
  // (Abc_SclDeriveGenlibSimple always sets every gate's delay to exactly 1.0).
  if (real_genlib) {
    debugPrint(logger,
               RMP,
               "sa_eval",
               1,
               "Real-timing GENLIB: name=\"{}\" gates={} "
               "inv_delay_max={:.4f} (GENLIB units)  budget_genlib={:.4f} (GENLIB units)",
               abc::Mio_LibraryReadName(real_genlib)
                   ? abc::Mio_LibraryReadName(real_genlib)
                   : "<null>",
               abc::Mio_LibraryReadGateNum(real_genlib),
               static_cast<double>(abc::Mio_LibraryReadDelayInvMax(real_genlib)),
               delay_budget);
  } else {
    logger->warn(RMP,
                 238,
                 "Abc_SclDeriveGenlib returned nullptr; "
                 "Abc_NtkMap will fall back to the global unit-delay GENLIB");
  }

  // Summarize GENLIB timing to diagnose budget achievability.
  // Key metrics: inv delay (single-level floor), min non-inv 2-input gate
  // delay, and the maximum levels that fit within delay_budget.
  // When cone_aig_depth > max_levels_in_budget, ABC cannot meet the target.
  //
  // effective_budget is what we actually pass to Abc_NtkMap.  When the real
  // budget is unachievable, ABC's required-time check fires inside its
  // mapper and silently degrades to minimum-area depth-only mapping — that
  // is the -500 ps WNS hit we see at rsz iter 0 every SA candidate.  We
  // override the budget to "just enough" for the actual AIG depth (+50%
  // margin), which keeps ABC in delay-aware mode using higher-drive cells
  // even on cones whose real budget is too tight.  ABC's resulting netlist
  // will still miss the original required time, but the cells will be sized
  // for delay — making rsz's downstream repair vastly more effective.
  double effective_budget = delay_budget;
  if (real_genlib) {
    double min_multi_input_delay = std::numeric_limits<double>::max();
    for (abc::Mio_Gate_t* pGate = abc::Mio_LibraryReadGates(real_genlib);
         pGate;
         pGate = abc::Mio_GateReadNext(pGate)) {
      const int    fanin = abc::Mio_GateReadPinNum(pGate);
      const double dly   = abc::Mio_GateReadDelayMax(pGate);
      if (fanin >= 2 && dly < min_multi_input_delay) {
        min_multi_input_delay = dly;
      }
    }
    const double inv_delay = abc::Mio_LibraryReadDelayInvMax(real_genlib);
    const int    max_levels
        = (min_multi_input_delay > 0.0 && min_multi_input_delay
               < std::numeric_limits<double>::max())
              ? static_cast<int>(std::floor(delay_budget / min_multi_input_delay))
              : -1;
    const bool budget_achievable
        = (max_levels >= 0 && post_op_depth >= 0
           && post_op_depth <= max_levels);

    // UNACHIEVABLE-budget override: REVERTED.
    // Tested with scale=1.0 (1.8x effective) and scale=1.5 (2.7x effective).
    // In both cases iter-0 WNS improved (cone less destroyed) but rsz repair
    // recovered far less because ABC's pre-sized cells removed the easy
    // upsize headroom rsz relies on.  Net: candidate quality no better than
    // no-override case.  Leaving effective_budget == delay_budget so rsz has
    // its full size-up move budget to work with.

    debugPrint(
        logger,
        RMP,
        "sa_eval",
        1,
        "GENLIB timing: inv_delay={:.4f}  min_2input_delay={:.4f}  "
        "budget={:.4f}  max_gate_levels_in_budget={}  aig_depth={}  {}",
        inv_delay,
        min_multi_input_delay < std::numeric_limits<double>::max()
            ? min_multi_input_delay
            : -1.0,
        delay_budget,
        max_levels,
        post_op_depth,
        budget_achievable
            ? "ACHIEVABLE"
            : fmt::format("UNACHIEVABLE (AIG needs {}x more levels than budget "
                          "allows — ABC will fall back to depth-only mapping "
                          "and likely regress WNS)",
                          max_levels > 0 ? (post_op_depth + max_levels - 1)
                                               / max_levels
                                         : post_op_depth));
    (void)inv_delay;  // used in format string; suppress release-mode warning
  }

  const int nodes_before = abc::Abc_NtkNodeNum(current_network.get());

  {
    // SuppressStdout only to silence Abc_NtkMap's unconditional stdout prints.
    utl::SuppressStdout nostdout(logger);

    current_network = WrapUnique(abc::Abc_NtkMap(
        current_network.get(),
        real_genlib,       // real Liberty-timing GENLIB — replaces fake 1.0 delays
        effective_budget,  // = delay_budget when achievable; scaled up when not
                           // (prevents ABC's depth-only min-area fallback that
                           // costs ~500 ps of WNS every UNACHIEVABLE candidate)
        /*AreaMulti=*/0.0,
        /*DelayMulti=*/0.0,  // must be 0.0 with real GENLIB: Mio_LibraryMultiDelay
                             // multiplies each gate's delay by nInputs^Multi, inflating
                             // a NAND2 (12 ps) to 67.9 ps and a NAND4 (15 ps) to 480 ps.
                             // delay_budget is in raw Liberty ps (~50-66 ps), so after
                             // inflation even a single NAND2 exceeds the budget — ABC
                             // can only fit inverters, falls back to depth-mode, and
                             // produces worse paths.  With DelayMulti=0.0, abcMap.c:111
                             // skips Mio_LibraryMultiDelay entirely; GENLIB delays remain
                             // as real Liberty values, numerically consistent with budget.
        /*LogFan=*/4.0,     // enable ABC's fanout-delay penalty so min-area
                             // cells aren't picked for high-fanout nets — that
                             // pick looks free in unit-delay land but explodes
                             // under real STA wire RC.
        /*Slew=*/10.0,       // assumed input slew (Liberty ps) for delay model;
                             // 0.0 disables slew-aware delay entirely.
        /*Gain=*/0.0,        // Gain already baked into real_genlib above
        /*nGatesMin=*/0,
        /*fRecovery=*/true,
        /*fSwitching=*/false,
        /*fSkipFanout=*/false,
        /*fUseProfile=*/false,
        /*fUseBuffs=*/true,  // let ABC insert buffer trees during mapping —
                             // partially restores the rsz-inserted buffer relay
                             // that the cone extraction strips away.
        /*fVerbose=*/false));

    // DO NOT call Mio_LibraryDelete(real_genlib) here.
    //
    // Map_SuperLibCreate (mapperTree.c:416) stores the raw pointer directly:
    //   pLib->pGenlib = pGenlib;
    // Map_SuperLibFree (mapperLib.c:173-176) then calls Mio_LibraryDelete on
    // it (when pGenlib != Abc_FrameReadLibGen()).  That free happens on the
    // NEXT call to Abc_NtkMap when it replaces the previous super library.
    // Freeing real_genlib here would produce a double-free on iteration 2
    // of SA (Signal 11 in Map_SuperLibDeriveFromGenlib → Map_SuperLibFree
    // → Mio_LibraryDelete on an already-freed pointer).
  }

  const int nodes_after
      = current_network ? abc::Abc_NtkNodeNum(current_network.get()) : -1;
  // nodes_before = AIG nodes going INTO the tech mapper (NOT original tech cells)
  // nodes_after  = tech cells coming OUT of Abc_NtkMap
  // original_cone_cells = tech cells from ODB before any ABC transformation
  debugPrint(logger,
             RMP,
             "sa_eval",
             1,
             "Abc_NtkMap: aig_nodes_in={}  new_tech_cells={}  "
             "original_tech_cells={}  expansion_delta={:+d}  ({} cells in design)",
             nodes_before,
             nodes_after,
             original_cone_cells,
             nodes_after - original_cone_cells,
             nodes_after > original_cone_cells ? "EXPANDING" : "SHRINKING");

  abc::Abc_NtkCleanup(current_network.get(), /*fVerbose=*/false);

  current_network = WrapUnique(abc::Abc_NtkDupDfs(current_network.get()));

  if (resize_iters > 0) {
    // All the magic numbers are defaults from abc/src/base/abci/abc.c
    utl::SuppressStdout nostdout(logger);
    abc::SC_SizePars pars = {};
    pars.nIters = resize_iters;
    pars.nIterNoChange = 50;
    pars.Window = 1;
    pars.Ratio = 10;
    pars.Notches = 1000;
    pars.DelayUser = 0;
    pars.DelayGap = 0;
    pars.TimeOut = 0;
    pars.BuffTreeEst = 0;
    pars.BypassFreq = 0;
    pars.fUseDept = true;
    abc::Abc_SclUpsizePerform(
        abc_library.abc_library(), current_network.get(), &pars, nullptr);
    // Abc_SclDnsizePerform intentionally omitted: it sees the 150 primary
    // output nets as having zero external load (external fanout is outside
    // ABC's network), so it reports infinite positive slack on every output
    // driver and downsizes them all to minimum strength.  STA then measures
    // those minimum-drive cells against real fanout capacitance and sees
    // 3-5× higher delay than ABC predicted — causing WNS regression of
    // 280+ ps.  UpsizePrimaryOutputDrivers (called below) fixes output
    // driver sizing correctly using the real Liberty drive-resistance data.
  }

  current_network = WrapUnique(abc::Abc_NtkToNetlist(current_network.get()));

  cut.InsertMappedAbcNetwork(
      current_network.get(), abc_library, network, name_generator, logger);

  // PO-driver and internal-cell upsizing are now handled by the in-situ
  // rsz::repairSetup pass that runs inside SolutionSlack::Evaluate (see
  // slack_tuning_strategy.cpp, step 2b).  rsz sizes based on real STA
  // slack/slew rather than the "always pick strongest equiv" heuristic the
  // old UpsizePrimaryOutputDrivers used — strictly more accurate, and
  // avoids upsizing cells that don't need it.
}

// ---------------------------------------------------------------------------
// GiaOpAbcCmd — return the ABC &-command string for a GIA op ID.
// ---------------------------------------------------------------------------
const char* GiaOpAbcCmd(size_t op_id)
{
  // Maps our GiaOp IDs (defined in GiaOps()) to ABC interpreter commands.
  // Op 0: Gia_ManPerformDch + Gia_ManEquivReduce       → &dch; &reduce
  // Op 1: Gia_ManAigSyn4                               → &syn4
  // Op 2: Gia_ManCompress2 (dc2)                       → &dc2
  // Op 3: Gia_ManBalance -d (depth-first)              → &b -d
  // Op 4: Gia_ManPerformDch + Gia_ManEquivReduceAndRemap → &dch; &reduce
  // Op 5: Gia_ManPerformMapping -g -K 6                → &if -g -K 6
  // Op 6: Gia_ManAigSynch2 (SAT sweep + LUT synth)    → &synch2
  // Op 7: Gia_ManPerformSopBalance                     → &sopb
  static const char* kCmds[] = {
      "&dch; &reduce",   // 0
      "&syn4",           // 1
      "&dc2",            // 2
      "&b -d",           // 3
      "&dch; &reduce",   // 4
      "&if -g -K 6",     // 5
      "&synch2",         // 6
      "&sopb",           // 7
  };
  if (op_id < sizeof(kCmds) / sizeof(kCmds[0])) {
    return kCmds[op_id];
  }
  return "";
}

// ---------------------------------------------------------------------------
// WriteConeBlif — write ABC network to a BLIF file via Io_WriteBlif.
// ---------------------------------------------------------------------------
void WriteConeBlif(abc::Abc_Ntk_t* ntk,
                   const std::string& blif_path,
                   utl::Logger* logger)
{
  utl::SuppressStdout suppress(logger);
  abc::Io_WriteBlif(ntk,
                    const_cast<char*>(blif_path.c_str()),
                    /*fWriteLatches=*/1,
                    /*fBb2Wb=*/0,
                    /*fSeq=*/0);
}

// ---------------------------------------------------------------------------
// WriteConeConstr — write per-PI arrival / per-PO required file in ABC
// .constr format (parsed by Scl_ConParse / "read_constr").
//
// Format:
//   .model  {model_name}
//   .input_arrival   {pi_net_name}  {float_time}
//   .output_required {po_net_name}  {float_time}
// ---------------------------------------------------------------------------
void WriteConeConstr(
    const std::string& model_name,
    const std::string& constr_path,
    const std::unordered_map<std::string, float>& pi_arrivals,
    const std::unordered_map<std::string, float>& po_requireds,
    utl::Logger* logger)
{
  std::ofstream f(constr_path);
  if (!f.is_open()) {
    logger->warn(RMP, 330, "Cannot write constraint file: {}", constr_path);
    return;
  }
  f << ".model " << model_name << "\n";
  // .input_arrival lines — one per PI net
  for (auto& [name, arr] : pi_arrivals) {
    f << ".input_arrival " << name << " "
      << std::fixed << std::setprecision(4) << arr << "\n";
  }
  // .output_required lines — one per PO net
  for (auto& [name, req] : po_requireds) {
    f << ".output_required " << name << " "
      << std::fixed << std::setprecision(4) << req << "\n";
  }
}

// ---------------------------------------------------------------------------
// WriteGenlibToFile — derive GENLIB from the SCL library and write to file.
// Must be called once per OptimizeDesign; the file is reused by all cones.
// ---------------------------------------------------------------------------
bool WriteGenlibToFile(cut::AbcLibrary& abc_library,
                       const std::string& genlib_path,
                       utl::Logger* logger)
{
  // Use Abc_SclDeriveGenlibSimple which includes ALL cells from the Liberty
  // file (the same set used by BuildMappedAbcNetwork / CreateStandardCells).
  // Abc_SclDeriveGenlib produces only a representative subset (77 cells),
  // which would miss many design cells and cause read_blif to fail.
  abc::Mio_Library_t* genlib = abc_library.mio_library();
  if (!genlib) {
    logger->warn(RMP, 331, "WriteGenlibToFile: mio_library() returned null.");
    return false;
  }
  FILE* f = fopen(genlib_path.c_str(), "w");
  if (!f) {
    abc::Mio_LibraryDelete(genlib);
    logger->warn(RMP, 332, "WriteGenlibToFile: cannot open {}", genlib_path);
    return false;
  }
  {
    utl::SuppressStdout suppress(logger);
    abc::Mio_WriteLibrary(f, genlib, /*fPrintSops=*/0, /*fShort=*/0,
                          /*fSelected=*/0);
  }
  fclose(f);
  // Set as the global ABC frame library so Abc_NtkMap uses the correct lib.
  abc::Abc_FrameSetLibGen(genlib);
  return true;
}

// ---------------------------------------------------------------------------
// ApplyConeOpsViaFile — CPA-Remap paper-exact file I/O apply.
//
// Writes per-cone BLIF + .constr + ABC script, executes the script via
// Cmd_CommandExecute, then commits the result to ODB.
//
// Files produced in work_dir:
//   cone_{cone_idx}.blif       — input netlist (current cone structure)
//   cone_{cone_idx}.constr     — PI arrivals / PO requireds (ABC format)
//   cone_{cone_idx}.tcl        — ABC optimization script
//   cone_{cone_idx}_opt.blif   — ABC output (optimized mapped netlist)
// ---------------------------------------------------------------------------
void ApplyConeOpsViaFile(
    sta::dbSta* sta,
    cut::LogicCut& cut,
    cut::AbcLibrary& abc_library,
    const std::vector<GiaOp>& gia_ops,
    const std::unordered_map<std::string, float>& pi_arrivals,
    const std::unordered_map<std::string, float>& po_requireds,
    float /*liberty_time_scale*/,
    double delay_budget_fallback,
    const std::string& work_dir,
    int cone_idx,
    utl::UniqueName& name_generator,
    utl::Logger* logger)
{
  sta::dbNetwork* network = sta->getDbNetwork();

  const std::string prefix      = work_dir + "/cone_" + std::to_string(cone_idx);
  const std::string blif_path   = prefix + ".blif";
  const std::string constr_path = prefix + ".constr";
  const std::string script_path = prefix + ".tcl";
  const std::string opt_path    = prefix + "_opt.blif";
  const std::string genlib_path = work_dir + "/tech.genlib";

  // ---- 1. Build the mapped ABC network from the cut ----
  utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> mapped_abc_network
      = cut.BuildMappedAbcNetwork(abc_library, network, logger);
  if (!mapped_abc_network) {
    logger->warn(RMP, 333,
                 "ApplyConeOpsViaFile: cone {}: BuildMappedAbcNetwork failed.",
                 cone_idx);
    return;
  }

  const std::string model_name = "cone_" + std::to_string(cone_idx);

  // Capture PI/PO net names from the gate-level NETLIST before converting.
  // In ABC_NTK_NETLIST the signal name lives on the NET object:
  //   PI → (fanout0) → NET  (STA net name = .inputs in BLIF)
  //   PO ← (fanin0)  ← NET  (STA net name = .outputs in BLIF)
  // All subsequent conversions (LOGIC+MAP → STRASH) lose these names because
  // Abc_NtkTrasferNames is commented out in ABC's Abc_NtkStartFrom.
  std::vector<std::string> pi_net_names;
  pi_net_names.reserve(abc::Abc_NtkCiNum(mapped_abc_network.get()));
  for (int i = 0; i < abc::Abc_NtkCiNum(mapped_abc_network.get()); i++) {
    abc::Abc_Obj_t* ci  = abc::Abc_NtkCi(mapped_abc_network.get(), i);
    abc::Abc_Obj_t* net = abc::Abc_ObjFanout0(ci);
    pi_net_names.push_back(net ? std::string(abc::Abc_ObjName(net)) : "");
  }
  std::vector<std::string> po_net_names;
  po_net_names.reserve(abc::Abc_NtkCoNum(mapped_abc_network.get()));
  for (int i = 0; i < abc::Abc_NtkCoNum(mapped_abc_network.get()); i++) {
    abc::Abc_Obj_t* co  = Abc_NtkCo(mapped_abc_network.get(), i);
    abc::Abc_Obj_t* net = abc::Abc_ObjFanin0(co);
    po_net_names.push_back(net ? std::string(abc::Abc_ObjName(net)) : "");
  }

  // Convert NETLIST+MAP → LOGIC+MAP so Abc_NtkStrash (C API) can decompose
  // each cell into its boolean SOP and build an AIG.
  // NOTE: Abc_NtkToLogic for ABC_FUNC_MAP produces ABC_NTK_LOGIC+ABC_FUNC_MAP
  //       (not SOP), so the gate-level representation is still present here.
  //       The key step is Abc_NtkStrash below.
  utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> logic_network(
      abc::Abc_NtkToLogic(mapped_abc_network.get()),
      &abc::Abc_NtkDelete);
  if (!logic_network) {
    logger->warn(RMP, 337,
                 "ApplyConeOpsViaFile: cone {}: Abc_NtkToLogic failed.",
                 cone_idx);
    return;
  }

  // Provide the MIO library so Abc_NtkStrash can look up each gate's SOP via
  // Mio_GateReadSop(pNode->pData).  Required for multi-output cell handling.
  auto* library = static_cast<abc::Mio_Library_t*>(mapped_abc_network->pManFunc);
  abc::Abc_FrameSetLibGen(library);
  logic_network->pManFunc = library;

  // Convert LOGIC+MAP → STRASH via the C API.
  //
  // The ABC "strash" COMMAND rejects multi-output cells (FAx1_ASAP7_75t_R,
  // etc.) with "Warning: Detected N multi-output cells" and aborts.  The C
  // function Abc_NtkStrash has no such guard: it handles multi-output cells
  // by decomposing each gate output into its SOP via Mio_GateReadSop and
  // building independent AIG sub-graphs for each output.  The in-memory path
  // (ApplyConeOps / initGia) already relies on this behaviour.
  utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> strash_network(
      abc::Abc_NtkStrash(logic_network.get(), /*fAllowConsts=*/false,
                         /*fCleanup=*/true, /*fRecord=*/false),
      &abc::Abc_NtkDelete);
  if (!strash_network) {
    logger->warn(RMP, 339,
                 "ApplyConeOpsViaFile: cone {}: Abc_NtkStrash failed.",
                 cone_idx);
    return;
  }

  // Set model name on the STRASH network.
  if (strash_network->pName) {
    free(strash_network->pName);
  }
  strash_network->pName = strdup(model_name.c_str());

  // Restore CI/CO names on the STRASH network (lost because Abc_NtkStartFrom
  // sets fCopyNames=false when the target type is ABC_NTK_STRASH).
  // When Io_WriteBlif(strash) calls Abc_NtkToLogic(strash) internally, it
  // uses Abc_NtkStartFrom(strash, LOGIC, SOP) with fCopyNames=true — so the
  // names we set here ARE propagated to the BLIF .inputs / .outputs lines.
  for (int i = 0; i < abc::Abc_NtkCiNum(strash_network.get())
       && i < static_cast<int>(pi_net_names.size()); i++) {
    if (pi_net_names[i].empty()) {
      continue;
    }
    abc::Abc_Obj_t* ci = abc::Abc_NtkCi(strash_network.get(), i);
    Nm_ManDeleteIdName(strash_network->pManName, ci->Id);
    Abc_ObjAssignName(ci, const_cast<char*>(pi_net_names[i].c_str()), nullptr);
  }
  for (int i = 0; i < abc::Abc_NtkCoNum(strash_network.get())
       && i < static_cast<int>(po_net_names.size()); i++) {
    if (po_net_names[i].empty()) {
      continue;
    }
    abc::Abc_Obj_t* co = Abc_NtkCo(strash_network.get(), i);
    Nm_ManDeleteIdName(strash_network->pManName, co->Id);
    Abc_ObjAssignName(co, const_cast<char*>(po_net_names[i].c_str()), nullptr);
  }

  // ---- 2. Write SOP BLIF ----
  // Io_WriteBlif requires ABC_NTK_NETLIST (has assert).  Passing STRASH
  // directly writes garbage (unnamed internal AIG nodes produce "(null)"
  // strings in the BLIF, making it unparseable by "read_blif").
  //
  // Abc_NtkToNetlist(strash):
  //   → Abc_NtkAigToLogicSop   (STRASH → LOGIC+SOP, fCopyNames=true so
  //                              CI/CO names we set above are preserved)
  //   → Abc_NtkLogicToNetlist  (LOGIC+SOP → NETLIST+SOP, PI/PO nets named
  //                              from CI/CO names in the LOGIC network)
  //
  // Io_WriteBlif(NETLIST+SOP) writes .names (SOP) format: no .gate entries,
  // no multi-output cells, no (null) names.  The ABC "strash" command in the
  // optimization script converts SOP BLIF → AIG cleanly.
  utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> netlist_sop(
      abc::Abc_NtkToNetlist(strash_network.get()),
      &abc::Abc_NtkDelete);
  if (!netlist_sop) {
    logger->warn(RMP, 340,
                 "ApplyConeOpsViaFile: cone {}: Abc_NtkToNetlist(strash) failed.",
                 cone_idx);
    return;
  }
  WriteConeBlif(netlist_sop.get(), blif_path, logger);

  // ---- 3. Write .constr ----
  WriteConeConstr(model_name, constr_path,
                  pi_arrivals, po_requireds, logger);

  // ---- 4. Write ABC script ----
  {
    std::ofstream script(script_path);
    if (!script.is_open()) {
      logger->warn(RMP, 334,
                   "ApplyConeOpsViaFile: cannot write script {}",
                   script_path);
      return;
    }

    // read_genlib — technology library for timing-driven map.
    script << "read_genlib " << genlib_path << "\n";
    // read_blif — loads the cone gate-level netlist.
    script << "read_blif " << blif_path << "\n";
    // strash — convert gate-level netlist to strashed AIG (required before &get).
    script << "strash\n";
    // &get — bring strashed AIG into GIA (And-Inverter Graph) manager.
    script << "&get\n";
    // Initial depth balance — same step as RunGiaPipeline.
    script << "&b -d\n";
    // MCTS-selected GIA ops.
    for (const GiaOp& op : gia_ops) {
      const char* cmd = GiaOpAbcCmd(op.id);
      if (cmd && *cmd) {
        script << cmd << "\n";
      }
    }
    // &put — convert GIA back to logic network.
    script << "&put\n";
    // map — technology mapping with real GENLIB; delay_budget_fallback is in
    // Liberty time units (same scale as pi_arrivals/po_requireds).
    script << "map -D " << std::fixed << std::setprecision(4)
           << delay_budget_fallback << "\n";
    // write_blif — save the optimised mapped netlist to file.
    script << "write_blif " << opt_path << "\n";
  }

  // ---- 5. Execute the script via the embedded ABC interpreter ----
  abc::Abc_Frame_t* abc_frame = abc::Abc_FrameGetGlobalFrame();
  const std::string source_cmd = "source " + script_path;

  debugPrint(logger, RMP, "cone_mcts", 1,
             "cone {}: running ABC script {}", cone_idx, script_path);

  {
    const int rc = abc::Cmd_CommandExecute(abc_frame, source_cmd.c_str());
    if (rc) {
      logger->warn(RMP, 335,
                   "ApplyConeOpsViaFile: cone {}: ABC script returned error {}.",
                   cone_idx, rc);
      return;
    }
  }

  // ---- 6. Get the optimised network from the ABC frame ----
  abc::Abc_Ntk_t* opt_ntk = abc::Abc_FrameReadNtk(abc_frame);
  if (!opt_ntk) {
    logger->warn(RMP, 336,
                 "ApplyConeOpsViaFile: cone {}: ABC produced no network.",
                 cone_idx);
    return;
  }

  // ---- 7. Invalidate STA and commit to ODB ----
  sta->graphDelayCalc()->delaysInvalid();
  sta->search()->arrivalsInvalid();
  sta->search()->endpointsInvalid();

  // Ensure the network is in netlist form before InsertMappedAbcNetwork.
  if (!abc::Abc_NtkIsNetlist(opt_ntk)) {
    utl::UniquePtrWithDeleter<abc::Abc_Ntk_t> nl(
        abc::Abc_NtkToNetlist(opt_ntk), &abc::Abc_NtkDelete);
    cut.InsertMappedAbcNetwork(nl.get(), abc_library, network,
                               name_generator, logger);
  } else {
    cut.InsertMappedAbcNetwork(opt_ntk, abc_library, network,
                               name_generator, logger);
  }

  debugPrint(logger, RMP, "cone_mcts", 1,
             "cone {}: file-I/O apply done (blif={} constr={} opt={})",
             cone_idx, blif_path, constr_path, opt_path);
}
}  // namespace rmp
