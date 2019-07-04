// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "gtest/gtest.h"
#include "test/providers/gradient_op_test_utils.h"
#include "test/providers/gradient_checker.h"
#include "core/training/training_optimizer.h"
#include "core/training/weight_updater.h"
#include "core/providers/cpu/cpu_execution_provider.h"

using namespace onnxruntime::training;
using namespace google::protobuf::util;

namespace onnxruntime {
namespace test {

constexpr auto ORIGINAL_MODEL_PATH = "testdata/test_training_model.onnx";
constexpr auto BACKWARD_MODEL_PATH = "backward_model.onnx";

AllocatorPtr GetAllocator() {
  static CPUExecutionProviderInfo info;
  static CPUExecutionProvider cpu_provider(info);
  return cpu_provider.GetAllocator(0, OrtMemTypeDefault);
}
template <typename T>
static void CreateMLValue(AllocatorPtr alloc,
                          const std::vector<int64_t>& dims,
                          const std::vector<T>& value,
                          MLValue* p_mlvalue) {
  TensorShape shape(dims);
  auto location = alloc->Info();
  auto element_type = DataTypeImpl::GetType<T>();
  void* buffer = alloc->Alloc(element_type->Size() * shape.Size());
  if (!value.empty()) {
    memcpy(buffer, &value[0], element_type->Size() * shape.Size());
  }

  std::unique_ptr<Tensor> p_tensor = std::make_unique<Tensor>(element_type,
                                                              shape,
                                                              buffer,
                                                              location);
  p_mlvalue->Init(p_tensor.release(),
                  DataTypeImpl::GetType<Tensor>(),
                  DataTypeImpl::GetType<Tensor>()->GetDeleteFunc());
}

static std::string BuildBackPropGraph(const std::string& forward_model_file) {
  const std::string backward_model_file = BACKWARD_MODEL_PATH;

  std::unique_ptr<Environment> env;
  EXPECT_TRUE(Environment::Create(env).IsOK());

  SessionOptions so;
  TrainingSession training_session{so};
  LossFunctionInfo loss(OpDef("MeanSquaredError"), "loss", {"predictions", "labels"});

  EXPECT_TRUE(training_session.Load(forward_model_file).IsOK());
  EXPECT_TRUE(training_session.BuildLossFunction(loss).IsOK());
  EXPECT_TRUE(training_session.BuildGradientGraph({"W1", "W2", "W3", "B1", "B2", "B3"}, "loss").IsOK());
  EXPECT_TRUE(training_session.Save(backward_model_file,
                                    TrainingSession::SaveOption::WITH_UPDATED_WEIGHTS_AND_LOSS_FUNC_AND_GRADIENTS)
                  .IsOK());

  return backward_model_file;
}

static std::unique_ptr<TrainingSession> RunTrainingSessionWithChecks(
  SessionOptions& so, const std::string& backprop_model_file)
{
  std::unique_ptr<Environment> env;
  EXPECT_TRUE(Environment::Create(env).IsOK());

  std::unique_ptr<TrainingSession> training_session = std::make_unique<TrainingSession>(so);

  EXPECT_TRUE(training_session->Load(backprop_model_file).IsOK());

  EXPECT_TRUE(training_session->Initialize().IsOK());

  // Create a WeightUpdater powered by GradientDescent algorithm.
  const static float LEARNING_RATE = 0.5f;

  WeightUpdater<out_graph_optimizer::GradientDescent> weight_updater(*training_session, {LEARNING_RATE, GetAllocator()});

  std::vector<MLValue> gradient_fetches;
  RunOptions run_option;

  // Create dummy feeds
  std::vector<int64_t> image_dims = {1, 784};
  std::vector<int64_t> label_dims = {1, 10};
  std::vector<float> image_value(784, 1);
  std::vector<float> label_value(10, 1);

  MLValue imageMLValue;
  CreateMLValue(GetAllocator(), image_dims, image_value, &imageMLValue);
  MLValue labelMLValue;
  CreateMLValue(GetAllocator(), label_dims, label_value, &labelMLValue);

  auto fw_feeds = std::make_pair<std::vector<std::string>, std::vector<MLValue>>({"X", "labels"}, {imageMLValue, labelMLValue});

  auto output_names_include_gradients = training_session->GetModelOutputNames();
  std::vector<std::string> training_output_names(output_names_include_gradients.begin(), output_names_include_gradients.end());

  EXPECT_TRUE(training_session->Run(run_option, fw_feeds.first, fw_feeds.second, training_output_names, &gradient_fetches).IsOK());

  // Get gradients
  NameMLValMap grad;
  for (size_t i = 0; i < training_output_names.size(); i++) {
    if (training_output_names[i] == "loss") continue;
    if (training_output_names[i] == "predictions") continue;

    grad.insert(make_pair(training_output_names[i], gradient_fetches[i]));
  }

  EXPECT_TRUE(weight_updater.Update(grad, 1).IsOK());

  return training_session;
}

TEST(GradientGraphBuilderTest, BuildGradientGraphTest) {
  const std::string& backprop_model_file = BuildBackPropGraph(ORIGINAL_MODEL_PATH);

  std::shared_ptr<Model> pModel;
  EXPECT_TRUE(Model::Load(backprop_model_file, pModel).IsOK());
}

TEST(GradientGraphBuilderTest, RunTrainingSessionTest) {
  const std::string& backprop_model_file = BuildBackPropGraph(ORIGINAL_MODEL_PATH);

  SessionOptions so;
  RunTrainingSessionWithChecks(so, backprop_model_file);
}

TEST(GradientGraphBuilderTest, RunTrainingSessionTestWithProfiler) {
  const std::string& backprop_model_file = BuildBackPropGraph(ORIGINAL_MODEL_PATH);

  SessionOptions so;
  so.enable_profiling = true;
  so.profile_file_prefix = ORT_TSTR("onnx_training_profiler_test");

  std::unique_ptr<TrainingSession> training_session = RunTrainingSessionWithChecks(so, backprop_model_file);

  std::string profile_file = training_session->EndProfiling();

  std::cout << "Profile output file = " << profile_file << std::endl;

  std::ifstream profile(profile_file);
  ASSERT_TRUE(profile);

  std::vector<std::string> tags = {"pid", "dur", "ts", "ph", "X", "name", "args"};
  int count = 0;
  std::string line;
  while (std::getline(profile, line)) {
    if (count == 0) {
      ASSERT_TRUE(line.find('[') != std::string::npos);
      // Opening array marker found.
    } else if (line.find(']') != std::string::npos) {
      // Closing array marker found.
      break;
    } else if (count >= 1) {
#ifdef DEBUG
      std::cout << count << ": " << line << std::endl;
#endif

      if (count == 1) {
        ASSERT_TRUE(line.find("model_loading_uri") != std::string::npos);
      }

      for (auto& s : tags) {
        ASSERT_TRUE(line.find(s) != std::string::npos);
      }
    }

    count++;
  }
  ASSERT_TRUE(count > 1);
}
}  // namespace test
}  // namespace onnxruntime
