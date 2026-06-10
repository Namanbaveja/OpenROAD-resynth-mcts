// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025-2025, The OpenROAD Authors
//
// Original upstream Antmicro SA strategy — WNS-only objective, no tiered eval,
// no top-K, no pre-repair.  Preserved verbatim from commit 01e674a3fc for
// baseline comparison in the resynth_annealing command.

#pragma once

#include <functional>
#include <optional>
#include <random>
#include <vector>

#include "aig/gia/gia.h"
#include "cut/abc_library_factory.h"
#include "cut/logic_cut.h"
#include "db_sta/dbSta.hh"
#include "resynthesis_strategy.h"
#include "sta/Scene.hh"
#include "utl/Logger.h"
#include "utl/unique_name.h"

namespace rmp {

using OriginalGiaOp = std::function<void(abc::Gia_Man_t*&)>;

class OriginalAnnealingStrategy : public ResynthesisStrategy
{
 public:
  explicit OriginalAnnealingStrategy(sta::Scene* corner,
                                     sta::Slack slack_threshold,
                                     std::optional<uint64_t> seed,
                                     std::optional<float> temperature,
                                     unsigned iterations,
                                     unsigned initial_ops,
                                     float percentage = 100.0f)
      : corner_(corner),
        slack_threshold_(slack_threshold),
        temperature_(temperature),
        iterations_(iterations),
        initial_ops_(initial_ops),
        percentage_(percentage)
  {
    if (seed) {
      random_ = decltype(random_){*seed};
    }
  }

  void OptimizeDesign(sta::dbSta* sta,
                      utl::UniqueName& name_generator,
                      rsz::Resizer* resizer,
                      utl::Logger* logger) override;

  void RunGia(sta::dbSta* sta,
              const std::vector<sta::Vertex*>& candidate_vertices,
              cut::AbcLibrary& abc_library,
              const std::vector<OriginalGiaOp>& gia_ops,
              size_t resize_iters,
              utl::UniqueName& name_generator,
              utl::Logger* logger);

 private:
  sta::Scene* corner_;
  sta::Slack slack_threshold_;
  std::optional<float> temperature_;
  unsigned iterations_;
  unsigned initial_ops_;
  float percentage_ = 100.0f;
  std::mt19937 random_;
};

}  // namespace rmp
