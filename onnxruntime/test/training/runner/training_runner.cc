// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <algorithm>
#include <memory>
#include "core/common/logging/logging.h"
#include "core/common/logging/sinks/clog_sink.h"
#include "core/session/environment.h"
#include "core/training/training_optimizer.h"
#include "core/training/weight_updater.h"
#include "test/training/runner/training_runner.h"
#include "test/training/runner/training_util.h"

#ifdef USE_CUDA
#include "core/providers/cuda/cuda_execution_provider.h"
#endif

using namespace std;

namespace onnxruntime {
namespace training {

const static string SGD_OP_NAME = "SGDOptimizer";
const static string SGD_LEARNING_RATE_STRING = "learning_rate";
static SessionOptions SESSION_OPTION = {
    true,                              //enable_sequential_execution
    false,                             //enable_profiling
    true,                              //enable_mem_pattern
    true,                              //enable_cpu_mem_arena
    ORT_TSTR("onnxruntime_profile_"),  //profile_file_prefix
    "",                                //session_logid
    -1,                                //session_log_severity_level
    0,                                 //session_log_verbosity_level
    5,                                 //max_num_graph_transformation_steps
    TransformerLevel::Level1,          //graph_optimization_level
    0,                                 //session_thread_pool_size
    true                               //only_execute_path_to_fetches
};

TrainingRunner::TrainingRunner(DataSet* training_data, DataSet* test_data, const Parameters& params)
    : training_data_(training_data),
      test_data_(test_data),
      params_(params),
      session_(SESSION_OPTION) {
  ORT_ENFORCE(!params_.model_path_.empty());
  ORT_ENFORCE((!params_.weights_to_train_.empty() && params_.weights_not_to_train_.empty()) ||
              (params_.weights_to_train_.empty() && !params_.weights_not_to_train_.empty()));
  ORT_ENFORCE(!params_.model_trained_path_.empty() || !params_.model_trained_with_loss_func_path_.empty());
  ORT_ENFORCE(!params_.model_prediction_name_.empty());
#ifdef USE_CUDA
  ORT_ENFORCE(!params_.use_cuda_ || !params_.in_graph_optimizer_name_.empty());
#else
  ORT_ENFORCE(params_.in_graph_optimizer_name_.empty());
#endif
}

Status TrainingRunner::Initialize() {
  ORT_RETURN_IF_ERROR(session_.Load(params_.model_path_));

  // Add loss func
  ORT_RETURN_IF_ERROR(session_.BuildLossFunction(params_.loss_func_info_));
  if (params_.world_rank_ == 0 && !params_.model_with_loss_func_path_.empty()) {
    ORT_RETURN_IF_ERROR(session_.Save(params_.model_with_loss_func_path_,
                                      TrainingSession::SaveOption::NO_RELOAD));
  }

  // Get the weights-to-train list if user specify it.
  // Otherweise, generate the list by removing not-to-train ones from all initializers.
  auto weights_to_train = params_.weights_to_train_;
  if (weights_to_train.empty()) {
    weights_to_train = session_.GetTrainableModelInitializers(params_.immutable_weigths_);
    for (const auto& not_to_train : params_.weights_not_to_train_) {
      weights_to_train.erase(not_to_train);
    }
  }

  for (auto weight : weights_to_train) {
    std::cout << "Training weight " << weight << std::endl;
  }

  std::unordered_map<std::string, in_graph_optimizer::OptimizerInfo> opt_info;
#ifdef USE_CUDA
  if (params_.use_cuda_) {
    ORT_RETURN_IF_ERROR(SetupOptimizerParams(weights_to_train, opt_info));
  }
#endif

  // Add gradient graph
  ORT_RETURN_IF_ERROR(session_.BuildGradientGraph(weights_to_train, params_.loss_func_info_.loss_name, opt_info));
  if (params_.world_rank_ == 0 && !params_.model_with_training_graph_path_.empty()) {
    Status s = session_.Save(params_.model_with_training_graph_path_, TrainingSession::SaveOption::NO_RELOAD);
    // TODO(bahuang): Currently AdamOptimizer's Moment_1 and Moment_2 are stored as graph initializers
    // They can be removed from initializers list
    if (!s.IsOK()) {
      std::cout << "Error when saving model " << params_.model_with_training_graph_path_ << " :" << s.ErrorMessage() << std::endl;
    }
  }

#ifdef USE_CUDA
  if (params_.use_cuda_) {
    CUDAExecutionProviderInfo xp_info{params_.world_rank_};
    ORT_RETURN_IF_ERROR(session_.RegisterExecutionProvider(std::make_unique<CUDAExecutionProvider>(xp_info)));
  }
#endif

  return session_.Initialize();
}

Status TrainingRunner::Run() {
  if (params_.world_rank_ == 0 && !params_.model_actual_running_graph_path_.empty()) {
    ORT_RETURN_IF_ERROR(session_.Save(params_.model_actual_running_graph_path_, TrainingSession::SaveOption::NO_RELOAD));
  }

  ORT_RETURN_IF_ERROR(TrainingLoop());

  ORT_RETURN_IF_ERROR(EndTraining());
  return Status::OK();
}

Status TrainingRunner::TrainingLoop() {
  // The optimizer out of the graph, will be used if params_.in_graph_optimizer_name_ is not set
  WeightUpdater<out_graph_optimizer::GradientDescent> weight_updater(session_,
                                                                     {params_.learning_rate_,
                                                                      TrainingUtil::GetCpuAllocator()});

  // Prepare output names
  auto output_names_include_gradients = session_.GetModelOutputNames();
  vector<string> training_output_names(output_names_include_gradients.begin(), output_names_include_gradients.end());
  vector<string> feed_names = training_data_->TensorNames();

  double total_time{0};
  //Set the first N batchs as warm-up iterations
  size_t warm_up_iters = 10;

  size_t num_shards_to_visit = params_.num_of_epoch_;
  if (training_data_loader_) {
    num_shards_to_visit *= training_data_loader_->NumShards();
  }

  for (size_t shard_it = 0; shard_it < num_shards_to_visit; ++shard_it) {
    // Shuffle the data for each epoch
    if (params_.shuffle_data_) {
      printf("Randomly shuffle training data.\n");
      training_data_->RandomShuffle();
    }

    // loop through the data
    for (size_t batch = 0; batch < training_data_->TotalBatch(params_.batch_size_); ++batch) {
      std::vector<MLValue> feeds = training_data_->GetKthBatch(params_.batch_size_, batch);
      vector<MLValue> gradient_fetches;

      std::chrono::duration<double> duration_seconds;
      auto start = std::chrono::high_resolution_clock::now();
      auto end = start;
      ORT_RETURN_IF_ERROR(session_.Run(RunOptions(),
                                       feed_names,
                                       feeds,
                                       training_output_names,
                                       &gradient_fetches));
      //Start counting after warm-up iterations
      if (batch >= warm_up_iters || shard_it > 0) {
        end = std::chrono::high_resolution_clock::now();
        duration_seconds = end - start;
        total_time += duration_seconds.count();
      }

      NameMLValMap grad;
      for (size_t i = 0; i < training_output_names.size(); i++) {
        if (training_output_names[i] == params_.loss_func_info_.loss_name ||
            training_output_names[i] == params_.model_prediction_name_) {
          continue;
        }
        grad.insert(make_pair(training_output_names[i], gradient_fetches[i]));
      }

      // Print some info when reaching the end of the batch.
      printf("batch: %d/%d, shard_iteration: %d/%d \n",
             static_cast<int>(batch),
             static_cast<int>(training_data_->TotalBatch(params_.batch_size_)),
             static_cast<int>(shard_it + 1),
             static_cast<int>(num_shards_to_visit));
      printf("Training data range: [%d - %d)\n",
             static_cast<int>(batch * params_.batch_size_),
             static_cast<int>((batch + 1) * params_.batch_size_ - 1));

      if (params_.in_graph_optimizer_name_.empty()) {
        weight_updater.Update(grad, params_.batch_size_);
      }
      ORT_RETURN_IF_ERROR(Evaluate(session_));
    }

    // Move to next shard of data
    if (training_data_loader_ != nullptr) {
      training_data_ = training_data_loader_->NextShard();
    }
  }

  auto total_batchs = num_shards_to_visit * training_data_->TotalBatch(params_.batch_size_) - warm_up_iters;
  std::cout << "Total running time:" << total_time << " seconds" << std::endl
            << "Average running time per batch:" << total_time / total_batchs * 1000 << " ms" << std::endl
            << "Throughput: " << params_.batch_size_ * total_batchs / total_time << " Examples / second" << std::endl;

  return Status::OK();
}

Status TrainingRunner::EndTraining() {
  if (params_.world_rank_ != 0) {
    printf("Skipping end-training on Device #%d, as it's not the root.", params_.world_rank_);
    return Status::OK();
  }

  // Test the in-memory model before saving.
  printf("\nEvaluateing the final model on the test set.\n");
  ORT_RETURN_IF_ERROR(Evaluate(session_));

  printf("\nSaving the trained model.\n");
  if (!params_.model_trained_path_.empty()) {
    ORT_RETURN_IF_ERROR(session_.Save(params_.model_trained_path_,
                                      TrainingSession::SaveOption::WITH_UPDATED_WEIGHTS));
  }
  if (!params_.model_trained_with_loss_func_path_.empty()) {
    ORT_RETURN_IF_ERROR(session_.Save(params_.model_trained_with_loss_func_path_,
                                      TrainingSession::SaveOption::WITH_UPDATED_WEIGHTS_AND_LOSS_FUNC));
  }

  // Load and test the trained model.
  printf("\nTesting the saved model: %s\n", params_.model_trained_with_loss_func_path_.c_str());
  return LoadAndEvaluate(params_.model_trained_with_loss_func_path_);
}

Status TrainingRunner::Evaluate(InferenceSession& session) {
  if (params_.skip_evaluation_) {
    printf("Skipping evaluation...\n");
    return Status::OK();
  }

  if (params_.world_rank_ != 0) {
    printf("Skipping evaluation on Device #%d, as it's not the root.\n", params_.world_rank_);
    return Status::OK();
  }

  // A static batch index representing current test batch
  static size_t current_batch = 0;

  if (params_.shuffle_data_ && current_batch == 0) {
    printf("Randomly shuffle test data.\n");
    test_data_->RandomShuffle();
  }

  size_t evaluation_batch_size = params_.eval_batch_size;

  printf("Test data range: [%d - %d)\n",
         static_cast<int>(current_batch * evaluation_batch_size),
         static_cast<int>((current_batch + 1) * evaluation_batch_size - 1));

  vector<string> feed_names = test_data_->TensorNames();

  size_t num_batches = size_t(ceil((float)evaluation_batch_size / (float)params_.batch_size_));
  if (evaluation_batch_size % params_.batch_size_ != 0) {
    printf(
        "evaluation_batch_size %zu is not an integer multiple of batch_size %zu. "
        "Using evaluation_batch_size %zu",
        evaluation_batch_size,
        params_.batch_size_,
        num_batches * params_.batch_size_);
  }

  for (size_t batch_idx = 0; batch_idx < num_batches; ++batch_idx) {
    std::vector<MLValue> feeds = test_data_->GetKthBatch(params_.batch_size_, current_batch);
    vector<MLValue> fetches;
    ORT_RETURN_IF_ERROR(session.Run(RunOptions(),
                                    feed_names,
                                    feeds,
                                    {params_.model_prediction_name_, params_.loss_func_info_.loss_name},
                                    &fetches));
    // Call error function with predict, label and loss.
    if (params_.error_function_) {
      params_.error_function_(fetches[0] /*predict*/, feeds.back() /*label*/, fetches[1] /*loss*/);
    }

    // Set to next batch
    if (++current_batch >= test_data_->TotalBatch(params_.batch_size_)) {
      if (test_data_loader_ != nullptr) {
        // Move to next shard
        test_data_ = test_data_loader_->NextShard();
      }
      current_batch = 0;
    }
  }

  // Call afer a test batch.
  if (params_.post_evaluation_callback_) {
    params_.post_evaluation_callback_(evaluation_batch_size);
  }

  return Status::OK();
}

Status TrainingRunner::LoadAndEvaluate(const std::string& model_path) {
  InferenceSession s{SessionOptions()};
  ORT_RETURN_IF_ERROR(s.Load(model_path));
  ORT_RETURN_IF_ERROR(s.Initialize());
  return Evaluate(s);
}

Status TrainingRunner::SetupOptimizerParams(const std::unordered_set<std::string>& weights_to_train,
                                            std::unordered_map<std::string, in_graph_optimizer::OptimizerInfo>& opt_infos) {
  // If in-graph optimizer is used, prepare the weight<->optimizer mapping.
  // Here all weights use the same SGDOptimizer or AdamOptimizer
  bool use_in_graph_optimizer = !params_.in_graph_optimizer_name_.empty();

  if (use_in_graph_optimizer) {
    in_graph_optimizer::OptimizerInfo opt_info{
        params_.in_graph_optimizer_name_,
        params_.learning_rate_,
        params_.world_rank_,
        params_.world_size_,
        {}};

    if (params_.in_graph_optimizer_name_ == "AdamOptimizer") {
      opt_info.attributes_["alpha"] = params_.adam_opt_params_.alpha_;
      opt_info.attributes_["beta"] = params_.adam_opt_params_.beta_;
      opt_info.attributes_["lambda"] = params_.adam_opt_params_.lambda_;
      opt_info.attributes_["epsilon"] = params_.adam_opt_params_.epsilon_;
    }

    opt_infos.reserve(weights_to_train.size());
    for (const auto& weight_name : weights_to_train) {
      opt_infos[weight_name] = opt_info;
    }
  }

  return Status::OK();
}
}  // namespace training
}  // namespace onnxruntime
