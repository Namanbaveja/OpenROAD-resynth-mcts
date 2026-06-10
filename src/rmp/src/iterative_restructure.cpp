// SPDX-License-Identifier: BSD-3-Clause
#include "iterative_restructure.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "absl/random/distributions.h"
#include "base/abc/abc.h"
#include "cut/abc_library_factory.h"
#include "cut/logic_cut.h"
#include "cut/logic_extractor.h"
#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "est/EstimateParasitics.h"
#include "map/mio/mio.h"
#include "misc/util/abc_global.h"
#include "odb/db.h"
#include "rmp/Restructure.h"
#include "sta/Delay.hh"
#include "sta/Graph.hh"
#include "sta/GraphDelayCalc.hh"
#include "sta/MinMax.hh"
#include "sta/Search.hh"
#include "utils.h"
#include "utl/Logger.h"

#include "base/main/abcapis.h"

namespace abc {
// NOLINTBEGIN(readability-identifier-naming)
extern void Abc_FrameSetLibGen(void* pLib);
extern void Abc_FrameReplaceCurrentNetwork(Abc_Frame_t* p, Abc_Ntk_t* pNet);
extern Abc_Ntk_t* Abc_FrameReadNtk(Abc_Frame_t* p);
// NOLINTEND(readability-identifier-naming)
}  // namespace abc

using abc::Abc_Frame_t;
using abc::Abc_FrameGetGlobalFrame;
using abc::Abc_Start;
using abc::Abc_Stop;
using abc::Cmd_CommandExecute;

namespace rmp {

using utl::RMP;

IterativeRestructure::IterativeRestructure(Restructure* r) : r_(r)
{
  // AIG-level operations only; technology mapping is appended automatically.
  alphabet_ = {
      "balance",
      "rewrite",
      "rewrite -z",
      "refactor",
      "refactor -z",
      "resub",
      "resub -K 6",
      "fraig",
      "dch",
  };
}

void IterativeRestructure::run(const std::string& liberty_file_name,
                               float slack_threshold,
                               unsigned /*max_depth*/,
                               const std::string& workdir_name,
                               const std::string& abc_logfile)
{
  r_->block_ = r_->db_->getChip()->getBlock();
  if (!r_->block_) {
    return;
  }

  r_->logfile_ = abc_logfile;
  r_->lib_file_names_.emplace_back(liberty_file_name);
  r_->work_dir_name_ = workdir_name + "/";

  // STA preamble — same pattern as SlackTuningStrategy::OptimizeDesign.
  r_->open_sta_->ensureGraph();
  r_->open_sta_->ensureLevelized();
  r_->open_sta_->searchPreamble();
  for (auto* mode : r_->open_sta_->modes()) {
    r_->open_sta_->ensureClkNetwork(mode);
  }

  // Step 1: collect ALL violating endpoints (not just one). These
  // sta::Vertex* pointers are stable across ECO begin/end/undo cycles, so we
  // store them once and reuse them each iteration.
  candidate_vertices_
      = GetEndpoints(r_->open_sta_, r_->resizer_, slack_threshold);
  if (candidate_vertices_.empty()) {
    r_->logger_->info(
        RMP, 200, "Iterative: no endpoints below slack threshold.");
    return;
  }
  r_->logger_->info(RMP,
                    204,
                    "Iterative: {} violating endpoints.",
                    candidate_vertices_.size());

  // Step 2: build AbcLibrary from the loaded OpenSTA database.
  cut::AbcLibraryFactory factory(r_->logger_);
  factory.AddDbSta(r_->open_sta_)
      .AddResizer(r_->resizer_)
      .SetCorner(r_->open_sta_->cmdScene());
  abc_library_.emplace(factory.Build());

  // Persistent ABC frame across all evaluate()/commit() calls in this run.
  // Cmd_CommandExecute uses the global frame; Abc_FrameReplaceCurrentNetwork
  // resets per-iteration network ownership.
  Abc_Start();

  // Step 4: simulated-annealing loop over ABC operator scripts.
  std::mt19937 rng(seed_);
  Script current = randomScript(rng);
  float current_score = evaluate(current);
  Script best = current;
  float best_score = current_score;
  unsigned worse_iters = 0;

  r_->logger_->info(
      RMP, 201, "Iterative: start. initial worst_slack = {}", current_score);

  for (unsigned i = 0; i < iters_; ++i) {
    const float T = temperature_ * float(iters_ - i) / float(iters_);

    if (revert_after_ && worse_iters >= revert_after_) {
      current = best;
      current_score = best_score;
      worse_iters = 0;
    }

    Script candidate = mutate(current, rng);
    const float cand_score = evaluate(candidate);

    bool accept = false;
    if (cand_score >= current_score) {
      accept = true;
    } else {
      const float p
          = (T == 0) ? 0.0f : std::exp((cand_score - current_score) / T);
      accept = absl::Uniform<float>(rng, 0, 1) < p;
    }

    if (accept) {
      current = std::move(candidate);
      current_score = cand_score;
    }

    if (current_score > best_score) {
      best_score = current_score;
      best = current;
      worse_iters = 0;
    } else {
      ++worse_iters;
    }

    if ((i + 1) % 10 == 0) {
      r_->logger_->info(
          RMP, 202, "Iter {}: T={}, best worst_slack={}", i + 1, T, best_score);
    }
  }

  // Step 5: commit the best script permanently and refresh parasitics.
  commit(best);
  Abc_Stop();
  r_->postABC(slack_threshold);

  r_->logger_->info(
      RMP, 203, "Iterative: done. final worst_slack = {}", best_score);
}

IterativeRestructure::Script IterativeRestructure::randomScript(
    std::mt19937& rng) const
{
  Script s;
  s.reserve(initial_ops_);
  for (unsigned i = 0; i < initial_ops_; ++i) {
    s.push_back(alphabet_[absl::Uniform<size_t>(rng, 0, alphabet_.size())]);
  }
  return s;
}

IterativeRestructure::Script IterativeRestructure::mutate(
    const Script& s,
    std::mt19937& rng) const
{
  Script out = s;
  const int op = absl::Uniform<int>(rng, 0, 3);  // 0=swap, 1=insert, 2=delete
  if (out.empty() || op == 1) {
    out.insert(out.begin() + absl::Uniform<size_t>(rng, 0, out.size() + 1),
               alphabet_[absl::Uniform<size_t>(rng, 0, alphabet_.size())]);
  } else if (op == 2 && out.size() > 1) {
    out.erase(out.begin() + absl::Uniform<size_t>(rng, 0, out.size()));
  } else {
    out[absl::Uniform<size_t>(rng, 0, out.size())]
        = alphabet_[absl::Uniform<size_t>(rng, 0, alphabet_.size())];
  }
  return out;
}

std::string IterativeRestructure::buildCommandString(const Script& script) const
{
  std::string cmd = "strash";
  for (const auto& op : script) {
    cmd += "; ";
    cmd += op;
  }
  cmd += "; map -B 0.2 -F 20";
  return cmd;
}

float IterativeRestructure::evaluate(const Script& script)
{
  if (!abc_library_ || candidate_vertices_.empty()) {
    return -std::numeric_limits<float>::infinity();
  }

  sta::dbNetwork* network = r_->open_sta_->getDbNetwork();

  // Disable incremental timing (matches RunGia's prelude).
  r_->open_sta_->graphDelayCalc()->delaysInvalid();
  r_->open_sta_->search()->arrivalsInvalid();
  r_->open_sta_->search()->endpointsInvalid();

  // Rebuild the cut from the persistent endpoint list each iteration.
  // The internal raw pointers (sta::Net*, sta::Instance*) are only valid for
  // the current ODB state, so the cut cannot be cached across undoEco cycles.
  cut::LogicExtractorFactory extractor(r_->open_sta_, r_->logger_);
  for (sta::Vertex* v : candidate_vertices_) {
    extractor.AppendEndpoint(v);
  }
  cut::LogicCut cut = extractor.BuildLogicCut(*abc_library_);
  if (cut.IsEmpty()) {
    return -std::numeric_limits<float>::infinity();
  }

  // Build the start network in-memory (no BLIF file I/O).
  auto mapped = cut.BuildMappedAbcNetwork(*abc_library_, network, r_->logger_);
  if (!mapped) {
    return -std::numeric_limits<float>::infinity();
  }

  // Install the mapping library so `map` does not need read_lib.
  auto* mio_library = static_cast<abc::Mio_Library_t*>(mapped->pManFunc);
  abc::Abc_FrameSetLibGen(mio_library);

  // Convert to a logic network and hand ownership to the ABC frame.
  abc::Abc_Ntk_t* logic_ntk = abc::Abc_NtkToLogic(mapped.get());
  if (!logic_ntk) {
    return -std::numeric_limits<float>::infinity();
  }
  logic_ntk->pManFunc = mio_library;

  Abc_Frame_t* frame = Abc_FrameGetGlobalFrame();
  abc::Abc_FrameReplaceCurrentNetwork(frame, logic_ntk);  // frame owns ntk

  const std::string cmds = buildCommandString(script);
  const int rc = Cmd_CommandExecute(frame, cmds.c_str());

  abc::Abc_Ntk_t* mapped_logic
      = (rc == 0) ? abc::Abc_FrameReadNtk(frame) : nullptr;
  if (!mapped_logic) {
    return -std::numeric_limits<float>::infinity();
  }

  // Convert mapped logic network → netlist (required by InsertMappedAbcNetwork).
  auto netlist = WrapUnique(abc::Abc_NtkToNetlist(mapped_logic));
  if (!netlist) {
    return -std::numeric_limits<float>::infinity();
  }

  // Stitch the optimized network back into ODB inside an ECO scope, measure
  // worstSlack, then roll back.
  odb::dbBlock* block = r_->block_;
  odb::dbDatabase::beginEco(block);
  cut.InsertMappedAbcNetwork(
      netlist.get(), *abc_library_, network, r_->name_generator_, r_->logger_);
  odb::dbDatabase::endEco(block);

  netlist.reset();

  // Refresh parasitics for the changed nets so STA sees the new netlist.
  r_->estimate_parasitics_->estimateWireParasitics();

  sta::Slack worst_slack;
  sta::Vertex* worst_vertex = nullptr;
  r_->open_sta_->worstSlack(r_->open_sta_->cmdScene(),
                            sta::MinMax::max(),
                            worst_slack,
                            worst_vertex);

  odb::dbDatabase::undoEco(block);

  return static_cast<float>(worst_slack);
}

void IterativeRestructure::commit(const Script& script)
{
  if (!abc_library_ || candidate_vertices_.empty()) {
    r_->logger_->warn(
        RMP, 211, "Iterative: final commit skipped, no cut available.");
    return;
  }

  sta::dbNetwork* network = r_->open_sta_->getDbNetwork();

  r_->open_sta_->graphDelayCalc()->delaysInvalid();
  r_->open_sta_->search()->arrivalsInvalid();
  r_->open_sta_->search()->endpointsInvalid();

  cut::LogicExtractorFactory extractor(r_->open_sta_, r_->logger_);
  for (sta::Vertex* v : candidate_vertices_) {
    extractor.AppendEndpoint(v);
  }
  cut::LogicCut cut = extractor.BuildLogicCut(*abc_library_);
  if (cut.IsEmpty()) {
    r_->logger_->warn(
        RMP, 212, "Iterative: final cut is empty, netlist unchanged.");
    return;
  }

  auto mapped = cut.BuildMappedAbcNetwork(*abc_library_, network, r_->logger_);
  if (!mapped) {
    r_->logger_->warn(
        RMP, 213, "Iterative: final mapped network build failed.");
    return;
  }

  auto* mio_library = static_cast<abc::Mio_Library_t*>(mapped->pManFunc);
  abc::Abc_FrameSetLibGen(mio_library);

  abc::Abc_Ntk_t* logic_ntk = abc::Abc_NtkToLogic(mapped.get());
  if (!logic_ntk) {
    r_->logger_->warn(RMP, 214, "Iterative: final logic conversion failed.");
    return;
  }
  logic_ntk->pManFunc = mio_library;

  Abc_Frame_t* frame = Abc_FrameGetGlobalFrame();
  abc::Abc_FrameReplaceCurrentNetwork(frame, logic_ntk);

  const std::string cmds = buildCommandString(script);
  const int rc = Cmd_CommandExecute(frame, cmds.c_str());
  abc::Abc_Ntk_t* mapped_logic
      = (rc == 0) ? abc::Abc_FrameReadNtk(frame) : nullptr;
  if (!mapped_logic) {
    r_->logger_->warn(
        RMP, 215, "Iterative: final ABC run failed, netlist unchanged.");
    return;
  }

  auto netlist = WrapUnique(abc::Abc_NtkToNetlist(mapped_logic));
  if (!netlist) {
    r_->logger_->warn(
        RMP, 216, "Iterative: final NtkToNetlist failed, netlist unchanged.");
    return;
  }

  cut.InsertMappedAbcNetwork(
      netlist.get(), *abc_library_, network, r_->name_generator_, r_->logger_);

  netlist.reset();
}

}  // namespace rmp
