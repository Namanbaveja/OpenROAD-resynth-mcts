// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2026, The OpenROAD Authors

#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>


#include "db_sta/dbSta.hh"
#include "rsz/Resizer.hh"
#include "sta/Delay.hh"
#include "sta/Liberty.hh"
#include "sta/NetworkClass.hh"
#include "sta/Scene.hh"
#include "utl/Logger.h"
#include "utl/unique_name.h"

namespace abc {
}  // namespace abc

namespace odb {
class dbDatabase;
class dbBlock;
class dbInst;
class dbNet;
class dbITerm;
}  // namespace odb

namespace est {
class EstimateParasitics;
}

namespace sta {
class dbSta;
}  // namespace sta

namespace rmp {

enum class Mode
{
  kArea1 = 0,
  kArea2,
  kArea3,
  kDelay1,
  kDelay2,
  kDelay3,
  kDelay4
};

class Restructure
{
  friend class IterativeRestructure;
 public:
  Restructure(utl::Logger* logger,
              sta::dbSta* open_sta,
              odb::dbDatabase* db,
              rsz::Resizer* resizer,
              est::EstimateParasitics* estimate_parasitics);
  ~Restructure();

  void reset();
  void resynth(sta::Scene* corner);
  void resynthOriginalAnnealing(sta::Scene* corner);  // upstream WNS-only SA
  void resynthAnnealing(sta::Scene* corner);          // our TNS+top-K SA
  void resynthImprovedAnnealing(sta::Scene* corner);
  void resynthImprovedAnnealingv2(sta::Scene* corner);
  void resynthGenetic(sta::Scene* corner);
  void resynthMcts(sta::Scene* corner);
  void resynthMctsCones(sta::Scene* corner);
  void resynthCpaRemap(sta::Scene* corner);
  void run(char* liberty_file_name,
           float slack_threshold,
           unsigned max_depth,
           char* workdir_name,
           char* abc_logfile);

  void setAnnealingSeed(int seed) { annealing_seed_ = seed; }
  void setAnnealingTemp(float temp) { annealing_temp_ = temp; }
  void setAnnealingIters(unsigned iters) { annealing_iters_ = iters; }
  void setAnnealingRevertAfter(unsigned revert_after)
  {
    annealing_revert_after_ = revert_after;
  }
  void setAnnealingInitialOps(unsigned ops) { annealing_init_ops_ = ops; }
  void setAnnealingPercentage(float percentage)
  {
    annealing_percentage_ = percentage;
  }
  void setAnnealingFinalTopK(unsigned k) { annealing_final_topk_ = k; }
  void setOriginalAnnealingPercentage(float p) { original_annealing_percentage_ = p; }

  // v2
  void setImprovedAnnealingSeed(int seed) { improved_annealing_seed_ = seed; }
  void setImprovedAnnealingTemp(float temp) { improved_annealing_temp_ = temp; }
  void setImprovedAnnealingIters(unsigned iters)
  {
    improved_annealing_iters_ = iters;
  }
  void setImprovedAnnealingRevertAfter(unsigned revert_after)
  {
    improved_annealing_revert_after_ = revert_after;
  }
  void setImprovedAnnealingInitialOps(unsigned ops)
  {
    improved_annealing_init_ops_ = ops;
  }


  void setImprovedAnnealingv2Seed(int seed) { improved_annealingv2_seed_ = seed; }
  void setImprovedAnnealingv2Temp(float temp) { improved_annealingv2_temp_ = temp; }
  void setImprovedAnnealingv2Iters(unsigned iters)
  {
    improved_annealingv2_iters_ = iters;
  }
  void setImprovedAnnealingv2RevertAfter(unsigned revert_after)
  {
    improved_annealingv2_revert_after_ = revert_after;
  }
  void setImprovedAnnealingv2InitialOps(unsigned ops)
  {
    improved_annealingv2_init_ops_ = ops;
  }


  void setMctsSeed(int seed) { mcts_seed_ = seed; }
  void setMctsUcbConstant(float c) { mcts_ucb_constant_ = c; }
  void setMctsIters(unsigned iters) { mcts_iters_ = iters; }
  void setMctsMaxDepth(int d) { mcts_max_depth_ = d; }
  void setMctsInitialOps(unsigned ops) { mcts_init_ops_ = ops; }
  void setMctsPercentage(float pct) { mcts_percentage_ = pct; }
  void setMctsWnsPct(float pct)          { mcts_wns_pct_    = pct; }
  void setMctsEvalMode(const char* mode) { mcts_eval_mode_ = mode; }
  void setSaEvalMode(const char* mode)   { sa_eval_mode_   = mode; }

  // Multi-cone MCTS (resynth_mcts_cones) setters.
  void setMctsConesSeed(unsigned seed) { mcts_cones_seed_ = seed; }
  void setMctsConesIters(unsigned iters) { mcts_cones_iters_ = iters; }
  void setMctsConesMaxDepth(int d) { mcts_cones_max_depth_ = d; }
  void setMctsConesUcbConstant(float c) { mcts_cones_ucb_constant_ = c; }
  void setMctsConesBfsHops(int hops) { mcts_cones_bfs_hops_ = hops; }
  void setMctsConesMaxInstances(int m) { mcts_cones_max_instances_ = m; }

  // CPA-remap (resynth_cpa_remap) setters.
  void setCpaRemapBfsHops(int hops) { cpa_remap_bfs_hops_ = hops; }
  void setCpaRemapMaxInstances(int m) { cpa_remap_max_instances_ = m; }
  // Proximity threshold in microns.  When > 0, proximity-based cone growth
  // is used instead of K-hop BFS.  0.0 disables proximity growth (K-hop only).
  void setCpaRemapProximityUm(float um) { cpa_remap_proximity_um_ = um; }
  // Long-wire break threshold in picoseconds.
  // 0 = disabled (include full path); >0 = break path at net arcs
  //   with step-delay > threshold, keeping only the endpoint-local segment.
  void setCpaRemapLongWirePs(float ps) { cpa_remap_long_wire_ps_ = ps; }
  // MCTS iterations within CPA-remap.  0 = pure balance+map (no MCTS).
  void setCpaRemapMctsIters(unsigned n) { cpa_remap_mcts_iters_ = n; }
  void setCpaRemapMctsMaxDepth(int d) { cpa_remap_mcts_max_depth_ = d; }
  void setCpaRemapMctsUcbConstant(float c) { cpa_remap_mcts_ucb_constant_ = c; }
  // Max cones to apply per round.  0 = unlimited.
  void setCpaRemapMaxConesPerRound(int n) { cpa_remap_max_cones_per_round_ = n; }

  void setGeneticSeed(int seed) { genetic_seed_ = seed; }
  void setGeneticPopulationSize(unsigned population_size)
  {
    genetic_population_size_ = population_size;
  }
  void setGeneticMutationProbability(float mutation_probability)
  {
    genetic_mutation_probability_ = mutation_probability;
  }
  void setGeneticCrossoverProbability(float crossover_probability)
  {
    genetic_crossover_probability_ = crossover_probability;
  }
  void setGeneticTournamentSize(unsigned tournament_size)
  {
    genetic_tournament_size_ = tournament_size;
  }
  void setGeneticTournamentProbability(float tournament_probability)
  {
    genetic_tournament_probability_ = tournament_probability;
  }
  void setGeneticIters(unsigned iters) { genetic_iters_ = iters; }
  void setGeneticInitialOps(unsigned ops) { genetic_init_ops_ = ops; }
  void setSlackThreshold(sta::Slack thresh) { slack_threshold_ = thresh; }
  void setMode(const char* mode_name);
  void setTieLoPort(sta::LibertyPort* loport);
  void setTieHiPort(sta::LibertyPort* hiport);

 private:
  void deleteComponents();
  void getBlob(unsigned max_depth);
  void runABC();
  void postABC(float worst_slack);
  bool writeAbcScript(const std::string& file_name);
  void writeOptCommands(std::ofstream& script);
  void initDB();
  void getEndPoints(sta::PinSet& ends, bool area_mode, unsigned max_depth);
  int countConsts(odb::dbBlock* top_block);
  void removeConstCells();
  void removeConstCell(odb::dbInst* inst);
  bool readAbcLog(const std::string& abc_file_name,
                  int& level_gain,
                  float& delay_val);

  utl::Logger* logger_;
  utl::UniqueName name_generator_;
  std::string logfile_;
  std::string locell_;
  std::string loport_;
  std::string hicell_;
  std::string hiport_;
  std::string work_dir_name_;

  // db vars
  sta::dbSta* open_sta_;
  odb::dbDatabase* db_;
  rsz::Resizer* resizer_;
  est::EstimateParasitics* estimate_parasitics_;
  odb::dbBlock* block_ = nullptr;

  // Annealing
  int annealing_seed_ = 0;
  std::optional<float> annealing_temp_;
  unsigned annealing_iters_ = 100;
  std::optional<unsigned> annealing_revert_after_;
  unsigned annealing_init_ops_ = 10;
  // 100.0 = no filter (every violator is a candidate); see
  // SlackTuningStrategy::OptimizeDesign for the top-N% filter.
  float annealing_percentage_ = 100.0f;
  // Distinct best-by-kCheap recipes re-scored under real repair at the end so
  // the measured-best post-repair TNS wins.  1 = legacy single-best apply.
  unsigned annealing_final_topk_ = 5;
  float original_annealing_percentage_ = 100.0f;

  // Improved Annealing
  int improved_annealing_seed_ = 0;
  std::optional<float> improved_annealing_temp_;
  unsigned improved_annealing_iters_ = 100;
  std::optional<unsigned> improved_annealing_revert_after_;
  unsigned improved_annealing_init_ops_ = 10;

  // Improved Annealing V2
  int improved_annealingv2_seed_ = 0;
  std::optional<float> improved_annealingv2_temp_;
  unsigned improved_annealingv2_iters_ = 100;
  std::optional<unsigned> improved_annealingv2_revert_after_;
  unsigned improved_annealingv2_init_ops_ = 10;

  // MCTS
  int mcts_seed_ = 0;
  std::optional<float>    mcts_ucb_constant_;
  unsigned                mcts_iters_    = 50;
  std::optional<unsigned> mcts_max_depth_;
  unsigned                mcts_init_ops_ = 6;
  float                   mcts_percentage_ = 100.0f;
  float                   mcts_wns_pct_   = 10.0f;
  std::string             mcts_eval_mode_ = "full";
  std::string             sa_eval_mode_   = "full";

  // Multi-cone MCTS (resynth_mcts_cones)
  unsigned mcts_cones_seed_          = 0;
  unsigned mcts_cones_iters_         = 50;
  int      mcts_cones_max_depth_     = 7;
  float    mcts_cones_ucb_constant_  = 1.414f;
  int      mcts_cones_bfs_hops_      = 3;
  // Skip cones with more instances than this (they are too large for ABC to
  // improve reliably and tend to regress timing).
  int      mcts_cones_max_instances_ = 100;

  // CPA-remap (resynth_cpa_remap)
  int      cpa_remap_bfs_hops_             = 1;
  int      cpa_remap_max_instances_        = 0;      // 0 = unlimited
  float    cpa_remap_proximity_um_         = 0.0f;   // 0.0 = K-hop BFS mode
  float    cpa_remap_long_wire_ps_         = 0.0f;   // 0 = disabled
  unsigned cpa_remap_mcts_iters_           = 0;      // 0 = balance+map only
  int      cpa_remap_mcts_max_depth_       = 7;
  float    cpa_remap_mcts_ucb_constant_    = 1.414f;
  int      cpa_remap_max_cones_per_round_  = 0;      // 0 = unlimited

  // Genetic
  int genetic_seed_ = 0;
  unsigned genetic_population_size_ = 4;
  float genetic_mutation_probability_ = 0.5;
  float genetic_crossover_probability_ = 0.5;
  unsigned genetic_tournament_size_ = 4;
  float genetic_tournament_probability_ = 0.8;
  unsigned genetic_iters_ = 10;
  unsigned genetic_init_ops_ = 10;

  sta::Slack slack_threshold_ = 0;

  std::string input_blif_file_name_;
  std::string output_blif_file_name_;
  std::vector<std::string> lib_file_names_;
  std::set<odb::dbInst*> path_insts_;

  Mode opt_mode_{Mode::kDelay1};
  bool is_area_mode_{false};
  int blif_call_id_{0};
};

}  // namespace rmp
