// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <utility>
#include <vector>

#include "core/framework/ml_value.h"
#include "core/training/optimizer_config.h"
#include "core/training/mpi_setup.h"
#include "core/training/training_session.h"
#include "test/training/runner/data_loader.h"

namespace onnxruntime {
namespace training {

class TrainingRunner {
 public:
  struct AdamOptimizerParams {
    float alpha;
    float beta;
    float lambda;
    float epsilon;
  };

  struct Parameters {
    Parameters() {}

    std::string model_name;
    std::string model_path;
    std::string model_with_loss_func_path;          // To save the model after adding loss func.
    std::string model_with_training_graph_path;     // To save the model after adding loss func and backward graph.
    std::string model_actual_running_graph_path;    // To save the model with the actual running graph after transformations.
    std::string model_trained_path;                 // To save the model after training.
    std::string model_trained_with_loss_func_path;  // To save the model with loss func after training.
    std::string model_gist_encode;                  // To save the model with gist encoding.

    PATH_STRING_TYPE train_data_dir;
    PATH_STRING_TYPE test_data_dir;

    bool is_perf_test;
    size_t perf_warm_up_iters;
    LossFunctionInfo loss_func_info;

    // The training optimizer name
    // Every weight's gradient will be connected to an optimizer node
    // For now all to-be-trained weights use the same optimizer type.
    std::string training_optimizer_name = "SGDOptimizer";
    std::unordered_map<std::string, float> optimizer_attributes;
    LearningRateParameters lr_params;
    int gradient_accumulation_steps = 1;

    // The weights to train, exclusive with weights_not_to_train_.
    std::unordered_set<std::string> weights_to_train;

    // The weights not to train. If not empty, all the initializers not in the vector will be trained.
    // exclusive with weights_to_train_.
    std::unordered_set<std::string> weights_not_to_train;

    TrainingSession::ImmutableWeights immutable_weights;

    MapStringToString input_name_map;

    bool shuffle_data;
    size_t batch_size;
    size_t eval_batch_size;
    size_t num_train_steps;
    size_t evaluation_period;
    bool do_eval = false;
    size_t display_loss_steps;

    // error_function_ is called when evaluating the error for a single sample.
    std::function<void(const std::vector<std::string>& feed_names,
                       const std::vector<OrtValue>& feeds,
                       const std::vector<std::string>& fetch_names,
                       const std::vector<OrtValue>& fetches,
                       size_t step)>
        error_function;

    // post_evaluation_callback_ is called when a batch of evaluation is done.
    std::function<void(size_t /*eval_batch_size*/,
                       size_t /*step*/,
                       const std::string& /*tag*/)>
        post_evaluation_callback;

    // Use CUDA providers or not.
    // TODO: support a list of providers.
    bool use_cuda = false;
    // Use Gist on CPU.
    bool use_gist = false;
    // Whether we collect execution profile trace during this run.
    bool use_profiler = false;
    // Maximum number of profile records to collect.
    size_t max_profile_records = profiling::Profiler::DEFAULT_MAX_PROFILER_EVENTS;
    MPIContext mpi_context;
    bool skip_evaluation = false;
    bool dump_fetches = false;

    VectorString fetch_names;

    bool use_mixed_precision = false;
    float loss_scale = 1.0f;
    bool use_fp16_moments = false;
    bool use_fp16_initializer = true;

    // Tensorboard configuration.
    PATH_STRING_TYPE log_dir;  // Path to write Tensorboard events to.
    std::string summary_name = "summary";
    VectorString scalar_names;
    VectorString histogram_names;

    bool EnableTensorboard() const {
      return !is_perf_test && !log_dir.empty() && mpi_context.world_rank == 0;
    }
  };

  TrainingRunner(Parameters params);

  common::Status Initialize();

  common::Status Run(std::shared_ptr<IDataLoader> training_data_loader, std::shared_ptr<IDataLoader> test_data_loader);

  common::Status EndTraining(std::shared_ptr<IDataLoader> data_loader);

  common::Status UpdateParams(Parameters params); 
 private:
  Status TrainingLoop(std::shared_ptr<IDataLoader> training_data_loader, std::shared_ptr<IDataLoader> test_data_loader);
  Status Evaluate(InferenceSession& session, std::shared_ptr<IDataLoader> data_loader);
  Status LoadAndEvaluate(const std::string& model_path, std::shared_ptr<IDataLoader> data_loader);
  Status SetupOptimizerParams(const std::unordered_set<std::string>& weights_to_train,
                              const std::unordered_map<std::string, NodeArg*>& fp16_weights_map,
                              const std::string& loss_scale_input_name,
                              OptimizerGraphConfig& opt_graph_config,
                              std::unordered_map<std::string, OptimizerNodeConfig>& opt_configs);

  size_t step_;
  size_t round_;
  size_t weight_update_step_count_;
  std::unordered_map<std::string, std::string> opt_graph_outputs_;

  std::unique_ptr<LossScaler> loss_scaler_ = nullptr;

  Parameters params_;
  TrainingSession session_;
};

}  // namespace training
}  // namespace onnxruntime