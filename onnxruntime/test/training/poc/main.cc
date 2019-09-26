// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "cxxopts.hpp"
#include "core/common/logging/logging.h"
#include "core/common/logging/sinks/clog_sink.h"
#include "core/platform/env.h"
#include "core/session/environment.h"
#include "core/training/training_session.h"
#include "core/training/tensorboard/event_writer.h"
#include "core/training/mpi_setup.h"

#include "test/training/poc/mnist_data_provider.h"
#include "test/training/runner/training_runner.h"
#include "test/training/runner/training_util.h"

#include <condition_variable>
#include <mutex>
#include <tuple>

using namespace onnxruntime;
using namespace onnxruntime::training;
using namespace onnxruntime::training::tensorboard;
using namespace std;

const static int NUM_CLASS = 10;
const static vector<int64_t> IMAGE_DIMS = {784};  //{1, 28, 28} for mnist_conv
const static vector<int64_t> LABEL_DIMS = {10};

Status ParseArguments(int argc, char* argv[], TrainingRunner::Parameters& params) {
  cxxopts::Options options("POC Training", "Main Program to train on MNIST");
  // clang-format off
  options
    .add_options()
      ("model_name", "model to be trained", cxxopts::value<std::string>())
      ("train_data_dir", "MNIST training and test data path.",
        cxxopts::value<std::string>()->default_value("mnist_data"))
      ("log_dir", "The directory to write tensorboard events.",
        cxxopts::value<std::string>()->default_value(""))
      ("use_profiler", "Collect runtime profile data during this training run.", cxxopts::value<bool>()->default_value("false"))
      ("use_gist", "Use GIST encoding/decoding.")
      ("use_cuda", "Use CUDA execution provider for training.", cxxopts::value<bool>()->default_value("false"))
      ("num_train_steps", "Number of training steps.", cxxopts::value<int>()->default_value("2000"))
      ("train_batch_size", "Total batch size for training.", cxxopts::value<int>()->default_value("100"))
      ("eval_batch_size", "Total batch size for eval.", cxxopts::value<int>()->default_value("100"))
      ("learning_rate", "The initial learning rate for Adam.", cxxopts::value<float>()->default_value("0.01"))
      ("perf_warm_up_iters", "Num of warm-up iterations to run before the perf test", cxxopts::value<int>()->default_value("0"))
      ("evaluation_period", "How many training steps to make before making an evaluation.",
        cxxopts::value<size_t>()->default_value("1"));
  // clang-format on

  try {
    auto flags = options.parse(argc, argv);

    params.model_name = flags["model_name"].as<std::string>();
    params.use_cuda = flags.count("use_cuda") > 0;
    params.use_gist = flags.count("use_gist") > 0;
    params.lr_params.initial_lr = flags["learning_rate"].as<float>();
    params.num_train_steps = flags["num_train_steps"].as<int>();
    params.batch_size = flags["train_batch_size"].as<int>();
    if (flags.count("eval_batch_size")) {
      params.eval_batch_size = flags["eval_batch_size"].as<int>();
    } else {
      params.eval_batch_size = params.batch_size;
    }
    params.evaluation_period = flags["evaluation_period"].as<size_t>();
    params.perf_warm_up_iters = flags["perf_warm_up_iters"].as<int>();

    auto train_data_dir = flags["train_data_dir"].as<std::string>();
    auto log_dir = flags["log_dir"].as<std::string>();
    params.train_data_dir.assign(train_data_dir.begin(), train_data_dir.end());
    params.log_dir.assign(log_dir.begin(), log_dir.end());
    params.use_profiler = flags.count("use_profiler") > 0;

  } catch (const exception& e) {
    const std::string msg = "Failed to parse the command line arguments";
    cerr << msg << ": " << e.what() << "\n"
         << options.help() << "\n";
    return Status(ONNXRUNTIME, FAIL, msg);
  }
  return Status::OK();
}

// NOTE: these variables need to be alive when the error_function is called.
int true_count = 0;
float total_loss = 0.0f;

void setup_training_params(TrainingRunner::Parameters& params) {
  params.model_path = params.model_name + ".onnx";
  params.model_with_loss_func_path = params.model_name + "_with_cost.onnx";
  params.model_with_training_graph_path = params.model_name + "_bw.onnx";
  params.model_actual_running_graph_path = params.model_name + "_bw_running.onnx";
  params.model_trained_path = params.model_name + "_trained.onnx";
  params.model_trained_with_loss_func_path = params.model_name + "_with_cost_trained.onnx";

  //Gist encode
  params.model_gist_encode = params.model_name + "_encode_gist.onnx";
  params.loss_func_info = LossFunctionInfo(OpDef("SoftmaxCrossEntropy"),
                                           "loss",
                                           {"predictions", "labels"});
  params.fetch_names = {"predictions", "loss"};

  // TODO: simplify provider/optimizer configuration. For now it is fixed to used SGD with CPU and Adam with GPU.
  if (params.use_cuda) {
    // TODO: This should be done in SGD optimizer. Will refactor when optimizing the kernel.
    // Adding another cuda kernel call for this division seems wasteful currently.
    params.training_optimizer_name = "AdamOptimizer";
    params.optimizer_attributes = {
        {"alpha", 0.9f},
        {"beta", 0.999f},
        {"lambda", 0.0f},
        {"epsilon", 0.1f},
    };
  } else {
    params.training_optimizer_name = "SGDOptimizer";
  }

#ifdef USE_HOROVOD
  params.mpi_context = setup_horovod();
#endif

  params.error_function = [](const std::vector<std::string>& /*feed_names*/,
                             const std::vector<OrtValue>& feeds,
                             const std::vector<std::string>& /*fetch_names*/,
                             const std::vector<OrtValue>& fetches) {
    const Tensor& label_t = feeds[1].Get<Tensor>();
    const Tensor& predict_t = fetches[0].Get<Tensor>();
    const Tensor& loss_t = fetches[1].Get<Tensor>();

    const float* prediction_data = predict_t.template Data<float>();
    const float* label_data = label_t.template Data<float>();
    const float* loss_data = loss_t.template Data<float>();

    const TensorShape predict_shape = predict_t.Shape();
    const TensorShape label_shape = label_t.Shape();
    const TensorShape loss_shape = loss_t.Shape();
    ORT_ENFORCE(predict_shape == label_shape);

    int64_t batch_size = predict_shape[0];
    for (int n = 0; n < batch_size; ++n) {
      auto max_class_index = std::distance(prediction_data,
                                           std::max_element(prediction_data, prediction_data + NUM_CLASS));

      if (static_cast<int>(label_data[max_class_index]) == 1) {
        true_count++;
      }

      prediction_data += predict_shape.SizeFromDimension(1);
      label_data += label_shape.SizeFromDimension(1);
    }
    total_loss += *loss_data;
  };

  std::shared_ptr<EventWriter> tensorboard;
  if (!params.log_dir.empty() && params.mpi_context.world_rank == 0)
    tensorboard = std::make_shared<EventWriter>(params.log_dir);

  params.post_evaluation_callback = [tensorboard](size_t num_samples, size_t step, const std::string /**/) {
    float precision = float(true_count) / num_samples;
    float average_loss = total_loss / float(num_samples);
    if (tensorboard != nullptr) {
      tensorboard->AddScalar("precision", precision, step);
      tensorboard->AddScalar("loss", average_loss, step);
    }
    printf("Step: %zu, #examples: %d, #correct: %d, precision: %0.04f, loss: %0.04f \n\n",
           step,
           static_cast<int>(num_samples),
           true_count,
           precision,
           average_loss);
    true_count = 0;
    total_loss = 0.0f;
  };
}

int main(int argc, char* args[]) {
  // setup logger
  string default_logger_id{"Default"};
  logging::LoggingManager default_logging_manager{unique_ptr<logging::ISink>{new logging::CLogSink{}},
                                                  logging::Severity::kWARNING,
                                                  false,
                                                  logging::LoggingManager::InstanceType::Default,
                                                  &default_logger_id};

  // setup onnxruntime env
  unique_ptr<Environment> env;
  ORT_ENFORCE(Environment::Create(env).IsOK());

  // setup training params
  TrainingRunner::Parameters params;
  RETURN_IF_FAIL(ParseArguments(argc, args, params));
  setup_training_params(params);

  // setup data
  auto device_count = params.mpi_context.world_size;
  std::vector<string> feeds{"X", "labels"};
  auto trainingData = std::make_shared<DataSet>(feeds);
  auto testData = std::make_shared<DataSet>(feeds);
  std::string mnist_data_path(params.train_data_dir.begin(), params.train_data_dir.end());
  PrepareMNISTData(mnist_data_path, IMAGE_DIMS, LABEL_DIMS, *trainingData, *testData, params.mpi_context.world_rank /* shard_to_load */, device_count /* total_shards */);

  if (testData->NumSamples() == 0) {
    printf("Warning: No data loaded - run cancelled.\n");
    return -1;
  }
  // start training session
  auto training_data_loader = std::make_shared<SingleDataLoader>(trainingData, feeds);
  auto test_data_loader = std::make_shared<SingleDataLoader>(testData, feeds);
  auto runner = std::make_unique<TrainingRunner>(training_data_loader, test_data_loader, params);
  RETURN_IF_FAIL(runner->Initialize());
  RETURN_IF_FAIL(runner->Run());

#ifdef USE_HOROVOD
  shutdown_horovod();
#endif
}
