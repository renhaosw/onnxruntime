// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/graph/graph.h"

namespace onnxruntime {
namespace training {

common::Status TransformGraphForTensorboard(Graph& graph,
                                            const std::string& summary_name,
                                            const std::vector<std::string>& scalar_nodes = {},
                                            const std::vector<std::string>& histogram_nodes = {});

}  // namespace training
}  // namespace onnxruntime
