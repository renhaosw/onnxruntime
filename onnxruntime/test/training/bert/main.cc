// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "cxxopts.hpp"
#include "core/util/math.h"
#include "core/common/common.h"
#include "core/common/logging/logging.h"
#include "core/common/logging/sinks/clog_sink.h"
#include "core/session/environment.h"
#include "core/training/training_session.h"
#include "core/training/tensorboard/event_writer.h"
#include "core/training/mpi_setup.h"
#include "test/training/runner/constant.h"
#include "test/training/runner/training_runner.h"
#include "test/training/runner/training_util.h"
#include "test/training/runner/data_loader.h"

using namespace onnxruntime;
using namespace onnxruntime::training;
using namespace onnxruntime::training::tensorboard;
using namespace std;

struct BertParameters : public TrainingRunner::Parameters {
  int max_sequence_length = 512;
  int max_predictions_per_sequence = 80;
  size_t batch_size_phase2;
  int gradient_accumulation_steps_phase2 = 1;
  float initial_lr_phase2;
  size_t num_train_steps_phase2;
  float warmup_ratio_phase2;
  PATH_STRING_TYPE train_data_dir_phase2;
  PATH_STRING_TYPE test_data_dir_phase2;
};

Status ParseArguments(int argc, char* argv[], BertParameters& params) {
  cxxopts::Options options("BERT Training", "Main Program to train BERT");
  // clang-format off
  options
    .add_options()
      ("model_name", "model to be trained", cxxopts::value<std::string>())
      ("train_data_dir", "Input ONNX example files (can be a glob or comma separated).",
        cxxopts::value<std::string>()->default_value("bert_data/128/books_wiki_en_corpus/train"))
      ("test_data_dir", "Input ONNX example files (can be a glob or comma separated).",
        cxxopts::value<std::string>()->default_value("bert_data/128/books_wiki_en_corpus/test"))
      ("train_data_dir_phase2", "Input ONNX example files (can be a glob or comma separated).",
        cxxopts::value<std::string>()->default_value(""))
      ("test_data_dir_phase2", "Input ONNX example files (can be a glob or comma separated).",
        cxxopts::value<std::string>()->default_value(""))
      ("output_dir", "The output directory where the model checkpoints will be written.",
        cxxopts::value<std::string>())
      ("log_dir", "The directory to write tensorboard events.",
        cxxopts::value<std::string>()->default_value(""))
      ("train_batch_size", "Total batch size for training.", cxxopts::value<int>())
      ("train_batch_size_phase2", "Total batch size for training.", cxxopts::value<int>()->default_value("1"))
      ("eval_batch_size", "Total batch size for eval.", cxxopts::value<int>())
      ("learning_rate", "The initial learning rate for the optimizer.", cxxopts::value<float>()->default_value("5e-5"))
      ("learning_rate_phase2", "The initial learning rate for the optimizer.", cxxopts::value<float>()->default_value("4e-3"))
      ("num_train_steps", "Total number of training steps to perform.", cxxopts::value<int>()->default_value("100000"))
      ("num_train_steps_phase2", "Total number of training steps to perform.", cxxopts::value<int>()->default_value("1563"))
      ("warmup_ratio", "Fraction of training steps for learning rate warmup.", cxxopts::value<float>()->default_value("0"))
      ("warmup_ratio_phase2", "Fraction of training steps for learning rate warmup.", cxxopts::value<float>()->default_value("0.128"))
      ("warmup_mode", "Warmup mode, one of [None|Cosine|Constant|Linear|Poly], defaults None.",
       cxxopts::value<std::string>()->default_value("None"))
      ("do_eval", "Whether to run eval on the dev set.", cxxopts::value<bool>()->default_value("false"))
      ("evaluation_period",
        "How many training steps to make before making an evaluation.",
        cxxopts::value<size_t>()->default_value("100"))
      ("display_loss_steps", "How often to dump loss into tensorboard", cxxopts::value<size_t>()->default_value("10"))
      ("gradient_accumulation_steps", "The number of gradient accumulation steps before performing a backward/update pass.",
        cxxopts::value<int>()->default_value("1"))
      ("gradient_accumulation_steps_phase2", "The number of gradient accumulation steps before performing a backward/update pass in phase 2.",
        cxxopts::value<int>()->default_value("1"))
      ("save_checkpoint_steps", "How often to save the model checkpoint.", cxxopts::value<int>()->default_value("1000"))
      ("iterations_per_loop", "How many steps to make in each estimator call.", cxxopts::value<int>()->default_value("1000"))
      ("max_eval_steps", "Maximum number of eval steps.", cxxopts::value<int>()->default_value("100"))
      ("use_mixed_precision", "Whether to use a mix of fp32 and fp16 arithmetic on GPU.", cxxopts::value<bool>()->default_value("false"))
      ("allreduce_in_fp16", "whether to use fp16 in AllReduce, if false (default), doing allreduce in fp32", cxxopts::value<bool>()->default_value("false"))
      ("loss_scale", "Loss scaling, positive power of 2 values can improve fp16 convergence. "
        "Set it 0 to uses dynamic scaling; Other none-zero value will used as static scale",
        cxxopts::value<float>()->default_value("0.0"))
      ("use_fp16_moments", "Whether to use fp16 version of moments.", cxxopts::value<bool>()->default_value("false"))
      ("use_fp16_initializer", "FP16 weights will be created. Otherwise, cast nodes will be inserted for converting weights from FP32 to FP16",
        cxxopts::value<bool>()->default_value("true"))
      ("use_profiler", "Collect runtime profile data during this training run.", cxxopts::value<bool>()->default_value("false"))
      ("max_profile_records", "Maximum number of runtime profile data records to collect.",
        cxxopts::value<size_t>()->default_value(to_string(profiling::Profiler::DEFAULT_MAX_PROFILER_EVENTS)))
      ("mode", "mode for running, can be one of [train|perf]", cxxopts::value<std::string>()->default_value("train"))
      ("perf_warm_up_iters", "Num of warm-up iterations to run before the perf test", cxxopts::value<int>()->default_value("10"))
      ("histogram", "Tensor(s) to display a histogram on tensorboard (e.g. '417,3347,417_grad,3347_grad' for bert-large or '81,449,81_grad,449_grad' for bert-tiny)",
        cxxopts::value<std::vector<std::string>>()->default_value({}))
      ("max_seq_length",
        "The maximum total input sequence length after WordPiece tokenization. "
        "Sequences longer than this will be truncated, and sequences shorter "
        "than this will be padded. Must match data generation.", cxxopts::value<int>()->default_value("512"))
      ("max_predictions_per_seq",
        "Maximum number of masked LM predictions per sequence. "
        "Must match data generation.", cxxopts::value<int>()->default_value("80"))
      ("optimizer", "Adam or Lamb", cxxopts::value<std::string>()->default_value("Adam"))
      ("alpha", "Adam/Lamb alpha parameter", cxxopts::value<float>()->default_value("0.9"))
      ("beta", "Adam/Lamb beta parameter", cxxopts::value<float>()->default_value("0.999"))
      ("lambda", "Adam/Lamb lambda parameter", cxxopts::value<float>()->default_value("0"))
      ("epsilon", "Adam/Lamb epsilon parameter", cxxopts::value<float>()->default_value("1e-6"));
  // clang-format on

  try {
    auto flags = options.parse(argc, argv);

    params.model_name = flags["model_name"].as<std::string>();
    float lr = flags["learning_rate"].as<float>();
    if (lr > 1.f || lr < 0.f) {
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "learning_rate is not in valid range [0.0, 1.0]");
    }
    params.lr_params.initial_lr = lr;

    float lr_phase2 = flags["learning_rate_phase2"].as<float>();
    if (lr_phase2 > 1.f || lr_phase2 < 0.f) {
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "learning_rate_phase2 is not in valid range [0.0, 1.0]");
    }
    params.initial_lr_phase2 = lr_phase2;

    float ratio = flags["warmup_ratio"].as<float>();
    if (ratio > 1.f || ratio < 0.f) {
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "warmup_ratio is not in valid range [0.0, 1.0]");
    }
    params.lr_params.warmup_ratio = ratio;

    float ratio_phase2 = flags["warmup_ratio_phase2"].as<float>();
    if (ratio_phase2 > 1.f || ratio_phase2 < 0.f) {
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "warmup_ratio_phase2 is not in valid range [0.0, 1.0]");
    }
    params.warmup_ratio_phase2 = ratio_phase2;

    params.num_train_steps = flags["num_train_steps"].as<int>();
    params.num_train_steps_phase2 = flags["num_train_steps_phase2"].as<int>();

    params.perf_warm_up_iters = flags["perf_warm_up_iters"].as<int>();
    params.batch_size = flags["train_batch_size"].as<int>();
    if (flags.count("eval_batch_size")) {
      params.eval_batch_size = flags["eval_batch_size"].as<int>();
    } else {
      params.eval_batch_size = params.batch_size;
    }

    params.batch_size_phase2 = flags["train_batch_size_phase2"].as<int>();

    params.max_sequence_length = flags["max_seq_length"].as<int>();
    params.max_predictions_per_sequence = flags["max_predictions_per_seq"].as<int>();

    params.gradient_accumulation_steps = flags["gradient_accumulation_steps"].as<int>();
    if (params.gradient_accumulation_steps < 1) {
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "Invalid gradient_accumulation_steps parameter: should be >= 1");
    }

    params.gradient_accumulation_steps_phase2 = flags["gradient_accumulation_steps_phase2"].as<int>();
    if (params.gradient_accumulation_steps_phase2 < 1) {
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "Invalid gradient_accumulation_steps_phase2 parameter: should be >= 1");
    }

    params.do_eval = flags["do_eval"].as<bool>();
    params.evaluation_period = flags["evaluation_period"].as<size_t>();
    params.display_loss_steps = flags["display_loss_steps"].as<size_t>();

    params.use_profiler = flags.count("use_profiler") > 0;
    params.max_profile_records = flags["max_profile_records"].as<size_t>();

    auto train_data_dir = flags["train_data_dir"].as<std::string>();
    auto test_data_dir = flags["test_data_dir"].as<std::string>();
    auto train_data_dir_phase2 = flags["train_data_dir_phase2"].as<std::string>();
    auto test_data_dir_phase2 = flags["test_data_dir_phase2"].as<std::string>();
    auto log_dir = flags["log_dir"].as<std::string>();
    params.train_data_dir.assign(train_data_dir.begin(), train_data_dir.end());
    params.test_data_dir.assign(test_data_dir.begin(), test_data_dir.end());
    params.train_data_dir_phase2.assign(train_data_dir_phase2.begin(), train_data_dir_phase2.end());
    params.test_data_dir_phase2.assign(test_data_dir_phase2.begin(), test_data_dir_phase2.end());
    params.log_dir.assign(log_dir.begin(), log_dir.end());
    params.histogram_names = flags["histogram"].as<std::vector<std::string>>();

    std::string mode = flags["mode"].as<std::string>();
    if (mode == "perf" || mode == "train") {
      params.is_perf_test = mode == "perf";
    } else {
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "Incorrect command line for mode: it must be one of [perf|train]");
    }

    params.use_mixed_precision = flags["use_mixed_precision"].as<bool>();
    params.allreduce_in_fp16 = flags["allreduce_in_fp16"].as<bool>();
    if (params.use_mixed_precision) {
      printf("Mixed precision training is enabled.\n");
    }
    if (params.allreduce_in_fp16) {
      printf("Performing AllReduce in fp16 \n");
    } else {
      printf("Performing AllReduce in fp32 \n");
    }
    {
      const float loss_scale = flags["loss_scale"].as<float>();
      if (loss_scale < 0.0f) {
        return Status(ONNXRUNTIME, INVALID_ARGUMENT, "Loss scale should be >= 0.");
      }
      params.loss_scale = loss_scale;
      if (params.use_mixed_precision) {
        if (params.loss_scale == 0.0) {
          printf("Using Dynamic loss scale.\n");
        } else {
          printf("Mixed precision loss scale is: %f\n", params.loss_scale);
        }
      }
    }

    params.use_fp16_moments = flags["use_fp16_moments"].as<bool>();
    if (params.use_fp16_moments) {
      printf("Using fp16 version of moments.\n");
    }
    params.use_fp16_initializer = flags["use_fp16_initializer"].as<bool>();
    if (params.use_mixed_precision && params.use_fp16_initializer) {
      printf("FP16 initializer is enabled.\n");
    }

    std::string warmup_mode = flags["warmup_mode"].as<std::string>();
    if (warmup_mode == LRSchedule_NoWarmup ||
        warmup_mode == LRSchedule_Cosine ||
        warmup_mode == LRSchedule_Constant ||
        warmup_mode == LRSchedule_Linear ||
        warmup_mode == LRSchedule_Poly) {
      params.lr_params.warmup_mode = warmup_mode;
      printf("Using learning rate warmup mode: %s \n", warmup_mode.c_str());
    } else {
      return Status(ONNXRUNTIME, INVALID_ARGUMENT,
                    "Incorrect warup_mode: it must be one of [None|Cosine|Constant|Linear|Poly]");
    }

    std::string optimizer_name = flags["optimizer"].as<std::string>();
    if (optimizer_name == "adam" || optimizer_name == "Adam") {
      params.training_optimizer_name = "AdamOptimizer";
    } else if (optimizer_name == "lamb" || optimizer_name == "Lamb") {
      params.training_optimizer_name = "LambOptimizer";
    } else {
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "Incorrect optimizer type: it must be one of [Adam|Lamb]");
    }

    float alpha = flags["alpha"].as<float>();
    float beta = flags["beta"].as<float>();
    float lambda = flags["lambda"].as<float>();
    float epsilon = flags["epsilon"].as<float>();
    ORT_RETURN_IF_NOT(alpha >= 0.f && alpha <= 1.f, "alpha is not in valid range [0.0, 1.0]");
    ORT_RETURN_IF_NOT(beta >= 0.f && beta <= 1.f, "alpha is not in valid range [0.0, 1.0]");

    params.optimizer_attributes = {
        {"alpha", alpha},
        {"beta", beta},
        {"lambda", lambda},
        {"epsilon", epsilon},
    };
  } catch (const exception& e) {
    const std::string msg = "Failed to parse the command line arguments";
    cerr << msg << ": " << e.what() << "\n"
         << options.help() << "\n";
    return Status(ONNXRUNTIME, INVALID_ARGUMENT, msg);
  }
  return Status::OK();
}

// NOTE: these variables need to be alive when the error_function is called.
float total_loss = 0.0f;
float mlm_loss = 0.0f;
float nsp_loss = 0.0f;
std::vector<std::string> summary_loss;

float GetLossValue(const Tensor& loss_tensor) {
  float loss = 0;
  if (DataTypeImpl::GetType<float>() == loss_tensor.DataType()) {
    loss = *(loss_tensor.template Data<float>());
  } else if (DataTypeImpl::GetType<MLFloat16>() == loss_tensor.DataType()) {
    loss = math::halfToFloat(loss_tensor.template Data<MLFloat16>()->val);
  }
  return loss;
}

void setup_training_params(BertParameters& params) {
  params.model_path = params.model_name + ".onnx";
  params.model_with_loss_func_path = params.model_name + "_with_cost.onnx";
  params.model_with_training_graph_path = params.model_name + "_bw.onnx";
  params.model_actual_running_graph_path = params.model_name + "_bw_running.onnx";
  params.model_trained_path = params.model_name + "_trained.onnx";
  params.model_trained_with_loss_func_path = params.model_name + "_with_cost_trained.onnx";

#ifdef USE_HOROVOD
  params.mpi_context = setup_horovod();
#endif

  params.loss_func_info = LossFunctionInfo(OpDef("BertLoss", kOnnxDomain),
                                           "total_loss",
                                           {/*prediction_masked_lm*/ "output1",
                                            /*prediction_next_sentence*/ "output2",
                                            /*masked_lm_positions*/ "masked_lm_positions",
                                            /*masked_lm_ids*/ "masked_lm_ids",
                                            /*masked_lm_weights*/ "masked_lm_weights",
                                            /*next_sentence_labels*/ "next_sentence_labels",
                                            /*mlm_loss*/ "mlm_loss",
                                            /*nsp_loss*/ "nsp_loss",
                                            /*batch_size*/ std::to_string(params.batch_size),
                                            /*max_sequence_len*/ std::to_string(params.max_sequence_length),
                                            /*max_predictions_per_sequence*/ std::to_string(params.max_predictions_per_sequence)});

  params.weights_not_to_train = {
      "position_01",            // Slice's dat input
      "op_min_ends_expand_10",  //op_min_ends_expand_10
  };
  params.fetch_names = {"total_loss", "mlm_loss", "nsp_loss"};

  if (params.EnableTensorboard()) {
    params.fetch_names.push_back(params.summary_name);
    params.scalar_names = {"total_loss", "mlm_loss", "nsp_loss", params.lr_params.feed_name};
  }

  params.immutable_weights = {
      {"Div", {{1, 8.0f}, {1, 1.4142135381698608f}}},
      {"Add", {{1, 1.0f}, {1, 9.999999960041972e-13f}}},
      {"Mul", {{1, 0.5f}, {1, -10000.0f}}},
      {"Sub", {{0, 1.0f}}}};

  params.shuffle_data = false;

  // name_in_data_file -> name_in_model
  params.input_name_map = {
      {"input_ids", "input1"},
      {"segment_ids", "input2"},
      {"input_mask", "input3"},
      {"masked_lm_positions", "masked_lm_positions"},
      {"masked_lm_ids", "masked_lm_ids"},
      {"masked_lm_weights", "masked_lm_weights"},
      {"next_sentence_label", "next_sentence_labels"}};

  params.use_cuda = true;

  params.skip_evaluation = params.is_perf_test;

  params.error_function = [params](const std::vector<std::string>& /*feed_names*/,
                                   const std::vector<OrtValue>& /*feeds*/,
                                   const std::vector<std::string>& fetch_names,
                                   const std::vector<OrtValue>& fetches,
                                   size_t step) {
    const Tensor& total_loss_t = fetches[0].Get<Tensor>();
    const Tensor& mlm_loss_t = fetches[1].Get<Tensor>();
    const Tensor& nsp_loss_t = fetches[2].Get<Tensor>();

    total_loss += GetLossValue(total_loss_t);
    mlm_loss += GetLossValue(mlm_loss_t);
    nsp_loss += GetLossValue(nsp_loss_t);

    if (params.EnableTensorboard()) {
      const Tensor& summary_loss_t = fetches[3].Get<Tensor>();
      summary_loss.push_back(*(summary_loss_t.template Data<std::string>()));
    }

    if (params.dump_fetches) {
      std::ostringstream filename;
      filename << "./fetch_dumps/rank_" << params.mpi_context.world_rank << "_step_" << step << ".txt";
      ofstream ofs(filename.str());
      for (size_t i = 0; i < fetch_names.size(); ++i) {
        TrainingUtil::PrintTensor(fetch_names[i], fetches[i].Get<Tensor>(), ofs);
      }
      ofs.close();
    }
  };

  std::shared_ptr<EventWriter> tensorboard;
  if (params.EnableTensorboard())
    tensorboard = std::make_shared<EventWriter>(params.log_dir);

  params.post_evaluation_callback = [tensorboard](size_t num_samples, size_t step, const std::string tag) {
    if (tensorboard != nullptr) {
      for (const std::string& summary : summary_loss) {
        tensorboard->AddSummary(summary, step, tag);
      }
    }

    printf("Step: %zu, #examples: %d, total_loss: %0.04f, mlm_loss: %0.04f, nsp_loss: %0.04f \n\n",
           step,
           static_cast<int>(num_samples),
           total_loss,
           mlm_loss,
           nsp_loss);
    total_loss = 0.0f;
    mlm_loss = 0.0f;
    nsp_loss = 0.0f;
    summary_loss.clear();
  };
}

int main(int argc, char* argv[]) {
#ifndef USE_CUDA
  printf("BERT training is not supported in non-CUDA build. ");
#endif

  BertParameters params;
  RETURN_IF_FAIL(ParseArguments(argc, argv, params));
  setup_training_params(params);

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

  // start training session
  std::unique_ptr<TrainingRunner> runner;
  std::shared_ptr<IDataLoader> training_data_loader;
  std::shared_ptr<IDataLoader> test_data_loader;
  if (params.is_perf_test) {
    // setup fake data
    int batch_size = static_cast<int>(params.batch_size);
    std::vector<std::string> tensor_names = {"input1", /*input_ids*/
                                             "input2", /*token_type_ids*/
                                             "input3", /*input_mask*/
                                             "masked_lm_positions",
                                             "masked_lm_ids",
                                             "masked_lm_weights",
                                             "next_sentence_labels"};
    std::vector<TensorShape> tensor_shapes = {{batch_size, params.max_sequence_length},
                                              {batch_size, params.max_sequence_length},
                                              {batch_size, params.max_sequence_length},
                                              {batch_size, params.max_predictions_per_sequence},
                                              {batch_size, params.max_predictions_per_sequence},
                                              {batch_size, params.max_predictions_per_sequence},
                                              {batch_size}};
    std::vector<onnx::TensorProto_DataType> tensor_types = {onnx::TensorProto_DataType_INT64,
                                                            onnx::TensorProto_DataType_INT64,
                                                            onnx::TensorProto_DataType_INT64,
                                                            onnx::TensorProto_DataType_INT64,
                                                            onnx::TensorProto_DataType_INT64,
                                                            onnx::TensorProto_DataType_FLOAT,
                                                            onnx::TensorProto_DataType_INT64};
    const size_t num_of_perf_samples = params.num_train_steps * params.batch_size;
    auto random_perf_data = std::make_shared<RandomDataSet>(num_of_perf_samples, tensor_names, tensor_shapes, tensor_types);
    auto random_perf_data_loader = std::make_shared<SingleDataLoader>(random_perf_data, tensor_names);
    training_data_loader = random_perf_data_loader;
    test_data_loader = random_perf_data_loader;
  } else {
    const size_t max_num_files_preload = 2;

    auto training_data_loader_ = std::make_shared<DataLoader>(params.input_name_map,
                                                              params.train_data_dir,
                                                              max_num_files_preload,
                                                              params.mpi_context.world_rank,
                                                              params.mpi_context.world_size);
    RETURN_IF_FAIL(training_data_loader_->InitialPreLoadAsync());
    training_data_loader = training_data_loader_;

    // Evaluation is only done in device #0
    if (params.mpi_context.world_rank == 0) {
      auto test_data_loader_ = std::make_shared<DataLoader>(params.input_name_map,
                                                            params.test_data_dir,
                                                            max_num_files_preload);
      RETURN_IF_FAIL(test_data_loader_->InitialPreLoadAsync());
      test_data_loader = test_data_loader_;
    }
  }

  runner = std::make_unique<TrainingRunner>(params);
  RETURN_IF_FAIL(runner->Initialize());
  RETURN_IF_FAIL(runner->Run(training_data_loader, test_data_loader));

  if (!params.train_data_dir_phase2.empty()) {
    const size_t max_num_files_preload = 2;

    params.lr_params.initial_lr = params.initial_lr_phase2;
    params.lr_params.warmup_ratio = params.warmup_ratio_phase2;
    params.num_train_steps = params.num_train_steps_phase2;
    params.batch_size = params.batch_size_phase2;
    params.gradient_accumulation_steps = params.gradient_accumulation_steps_phase2;

    runner->UpdateParams(params);

    // Create phase-2 training set loader.
    auto training_data_loader_phase2 = std::make_shared<DataLoader>(
        params.input_name_map,
        params.train_data_dir_phase2,
        max_num_files_preload,
        params.mpi_context.world_rank,
        params.mpi_context.world_size);
    RETURN_IF_FAIL(training_data_loader_phase2->InitialPreLoadAsync());

    // Create phase-2 test set loader if presents.
    std::shared_ptr<DataLoader> test_data_loader_phase2;
    if (params.mpi_context.world_rank == 0 && !params.test_data_dir_phase2.empty()) {
      test_data_loader_phase2 = std::make_shared<DataLoader>(params.input_name_map,
                                                             params.test_data_dir_phase2,
                                                             max_num_files_preload);
      RETURN_IF_FAIL(test_data_loader_phase2->InitialPreLoadAsync());
    }

    // Do phase-2 training
    RETURN_IF_FAIL(runner->Run(training_data_loader_phase2, test_data_loader_phase2));
  }

#ifdef USE_HOROVOD
  shutdown_horovod();
#endif
}