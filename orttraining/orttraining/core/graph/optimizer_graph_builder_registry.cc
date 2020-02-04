// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "orttraining/core/graph/optimizer_graph_builder_registry.h"

// optimizer graph builders to register
#include "orttraining/core/graph/optimizer_graph_builder.h"
#include "orttraining/core/graph/allreduce_optimizer_graph_builder.h"
#include "orttraining/core/graph/zero_optimizer_graph_builder.h"

namespace onnxruntime {
namespace training {

// Register all optimizer graph builders here.
void OptimizerGraphBuilderRegistry::RegisterGraphBuilders() {
  GetInstance().Register<OptimizerGraphBuilder>("Default");
  GetInstance().Register<AllreduceOptimizerGraphBuilder>("Allreduce");
  GetInstance().Register<ZeROOptimizerGraphBuilder>("ZeRO");
}

std::string OptimizerGraphBuilderRegistry::GetNameFromConfig(const OptimizerGraphConfig& config) const {
  if (config.world_size > 1) {
    if (config.partition_optimizer) {
      return "ZeRO";
    } else {
      return "Allreduce";
    }
  }

  return "Default";
}

}  // namespace training
}  // namespace onnxruntime