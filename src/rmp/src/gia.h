// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2026, The OpenROAD Authors

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aig/gia/gia.h"
#include "cut/abc_library_factory.h"
#include "cut/logic_cut.h"
#include "utl/unique_name.h"

namespace sta {
class dbSta;
class Vertex;
}  // namespace sta

namespace utl {
class Logger;
}  // namespace utl

namespace rmp {

struct GiaOp final
{
  using AigManager = utl::UniquePtrWithDeleter<abc::Gia_Man_t>;
  using OpExecutor = std::function<void(AigManager&)>;

  size_t id;
  OpExecutor op;
  bool operator==(const GiaOp& other) const { return id == other.id; }
  bool operator!=(const GiaOp& other) const { return !(*this == other); }

  template <typename H>
  friend H AbslHashValue(H h, const GiaOp& gia_op)
  {
    return H::combine(std::move(h), gia_op.id);
  }
};

std::vector<GiaOp> GiaOps(utl::Logger* logger);

// Original single-cone RunGia — unchanged.  Extracts cone from STA,
// applies gia_ops, maps with Abc_NtkMap, commits to ODB.
void RunGia(sta::dbSta* sta,
            const std::vector<sta::Vertex*>& candidate_vertices,
            cut::AbcLibrary& abc_library,
            const std::vector<GiaOp>& gia_ops,
            size_t resize_iters,
            utl::UniqueName& name_generator,
            utl::Logger* logger);

// Pure-ABC cone evaluation — no ODB reads or writes after cut is given.
//
// Implements the CPA-remap paper's accept criterion "Tr < To":
//   1. Build the ABC network from `cut` (original mapped cone = To).
//   2. Measure To = max cone-path-delay with all PIs at t=0.
//   3. Apply `gia_ops` in GIA, re-balance, then call Abc_NtkMap.
//   4. Measure Tr = max cone-path-delay after remapping, with all PIs at t=0.
//   5. Return To - Tr.
//
// Return value:
//   > 0  → cone delay improved (Tr < To)  → ACCEPT the remapped cone.
//   ≤ 0  → no improvement or degradation  → skip.
//   -inf → ABC failed to build / remap the cone.
//
// MCTS uses the return value as a reward and maximises it across op sequences.
double RunGiaConeSlack(
    sta::dbSta* sta,
    cut::LogicCut& cut,
    cut::AbcLibrary& abc_library,
    const std::vector<GiaOp>& gia_ops,
    const std::unordered_map<std::string, float>& pi_arrivals,
    const std::unordered_map<std::string, float>& po_requireds,
    float liberty_time_scale,
    double delay_budget_fallback,
    utl::Logger* logger);

// Apply gia_ops using a pre-extracted cut (in-memory, skips ODB/STA re-extraction).
// Commits the resulting mapped network to ODB via InsertMappedAbcNetwork.
// Called once per cone after the MCTS search has identified the best ops.
void ApplyConeOps(
    sta::dbSta* sta,
    cut::LogicCut& cut,
    cut::AbcLibrary& abc_library,
    const std::vector<GiaOp>& gia_ops,
    const std::unordered_map<std::string, float>& pi_arrivals,
    const std::unordered_map<std::string, float>& po_requireds,
    float liberty_time_scale,
    double delay_budget_fallback,
    utl::UniqueName& name_generator,
    utl::Logger* logger);

// Return the ABC &-command string for a GIA op ID, e.g. "&syn4", "&dch; &reduce".
// Used when generating per-cone ABC scripts for the file-I/O apply path.
const char* GiaOpAbcCmd(size_t op_id);

// Write the ABC mapped network (from BuildMappedAbcNetwork) to a BLIF file.
void WriteConeBlif(abc::Abc_Ntk_t* ntk,
                   const std::string& blif_path,
                   utl::Logger* logger);

// Write an ABC .constr file with per-PI arrival and per-PO required times.
//
// File format (parsed by Scl_ConParse / "read_constr"):
//   .model  {model_name}
//   .input_arrival   {pi_net_name}  {time_in_liberty_units}
//   .output_required {po_net_name}  {time_in_liberty_units}
void WriteConeConstr(const std::string& model_name,
                     const std::string& constr_path,
                     const std::unordered_map<std::string, float>& pi_arrivals,
                     const std::unordered_map<std::string, float>& po_requireds,
                     utl::Logger* logger);

// Write the GENLIB library currently loaded in the ABC frame to a file.
// Returns true on success.  Required before the per-cone ABC scripts can
// reference the library via "read_genlib {genlib_path}".
bool WriteGenlibToFile(cut::AbcLibrary& abc_library,
                       const std::string& genlib_path,
                       utl::Logger* logger);

// Apply gia_ops via file I/O — the exact CPA-Remap paper flow.
//
// Per-cone files written to work_dir:
//   cone_{cone_idx}.blif      — netlist extracted from the cut
//   cone_{cone_idx}.constr    — per-PI/PO timing constraints
//   cone_{cone_idx}.tcl       — ABC script:
//       read_genlib {genlib}
//       read_blif   {blif}
//       read_constr {constr}
//       strash; &get; &b -d; {ops}; &put
//       map -D {budget}
//       write_blif {opt_blif}
//
// After Cmd_CommandExecute runs the script, the optimized network is read
// from the ABC frame and committed to ODB via InsertMappedAbcNetwork.
void ApplyConeOpsViaFile(
    sta::dbSta* sta,
    cut::LogicCut& cut,
    cut::AbcLibrary& abc_library,
    const std::vector<GiaOp>& gia_ops,
    const std::unordered_map<std::string, float>& pi_arrivals,
    const std::unordered_map<std::string, float>& po_requireds,
    float liberty_time_scale,
    double delay_budget_fallback,
    const std::string& work_dir,
    int cone_idx,
    utl::UniqueName& name_generator,
    utl::Logger* logger);

}  // namespace rmp
