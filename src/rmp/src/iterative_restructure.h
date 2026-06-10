// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <optional>
#include <random>
#include <string>
#include <vector>

#include "cut/abc_library_factory.h"
#include "cut/logic_cut.h"
#include "cut/logic_extractor.h"
#include "sta/Graph.hh"

namespace rmp {

class Restructure;

class IterativeRestructure
{
 public:
  explicit IterativeRestructure(Restructure* restructure);

  void setIters(unsigned n) { iters_ = n; }
  void setSeed(int s) { seed_ = s; }
  void setTemperature(float t) { temperature_ = t; }
  void setInitialOps(unsigned n) { initial_ops_ = n; }
  void setRevertAfter(unsigned n) { revert_after_ = n; }

  // Entry point — replaces Restructure::run() for the iterative flow.
  // `max_depth` is retained for API compatibility but is no longer used:
  // LogicExtractorFactory performs its own bounded BFS over the full set of
  // violating endpoints. `liberty_file_name` and `abc_logfile` are likewise
  // kept for compatibility with the existing Tcl wrapper.
  void run(const std::string& liberty_file_name,
           float slack_threshold,
           unsigned max_depth,
           const std::string& workdir_name,
           const std::string& abc_logfile);

 private:
  using Script = std::vector<std::string>;

  Script randomScript(std::mt19937& rng) const;
  Script mutate(const Script& s, std::mt19937& rng) const;

  // Build the inline ABC command string for a candidate.
  // Format: "strash; <op1>; <op2>; ...; map -B 0.2 -F 20".
  std::string buildCommandString(const Script& script) const;

  // Run a candidate inside an ECO scope, measure worstSlack, then undoEco.
  // Higher score = better (positive worstSlack means no violations).
  // Returns -infinity on ABC failure.
  float evaluate(const Script& script);

  // Apply the best candidate permanently (no undoEco).
  void commit(const Script& best_script);

  Restructure* r_;  // borrowed
  unsigned iters_ = 50;
  int seed_ = 0;
  float temperature_ = 1.0;
  unsigned initial_ops_ = 6;
  unsigned revert_after_ = 0;  // 0 = disabled

  std::optional<cut::AbcLibrary> abc_library_;
  std::vector<sta::Vertex*> candidate_vertices_;

  std::vector<std::string> alphabet_;  // ABC commands available for mutation
};

}  // namespace rmp
