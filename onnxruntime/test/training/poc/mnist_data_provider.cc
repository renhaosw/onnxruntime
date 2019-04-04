// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "mnist_reader/mnist_reader.hpp"
#include "mnist_reader/mnist_utils.hpp"
#include "mnist_data_provider.h"
#include "test/training/runner/training_util.h"

using namespace onnxruntime;
using namespace onnxruntime::training;
using namespace std;

typedef uint8_t Label;
typedef vector<uint8_t> Image;

pair<vector<vector<float>>, vector<vector<float>>> NormalizeData(const vector<Image>& images, const vector<Label>& labels) {
  vector<vector<float>> normalized_images;
  for (int i = 0; i < images.size(); i++) {
    // Binarize the image.
    vector<float> normalized_image(images[i].begin(), images[i].end());
    for (int j = 0; j < images[i].size(); j++) {
      if (images[i][j] > 0) {
        normalized_image[j] = 1.0f;
      }
    }
    normalized_images.emplace_back(move(normalized_image));
  }

  vector<vector<float>> one_hot_labels;
  for (int i = 0; i < labels.size(); i++) {
    vector<float> one_hot_label(10, 0.0f);
    one_hot_label[labels[i]] = 1.0f;  //one hot
    one_hot_labels.emplace_back(move(one_hot_label));
  }
  return make_pair(normalized_images, one_hot_labels);
}

void ConvertData(const vector<vector<float>>& images,
                 const vector<vector<float>>& labels,
                 vector<unique_ptr<DataPerRun>>& data_for_training) {
  const static vector<int64_t> image_dims = {1, 784};
  const static vector<int64_t> label_dims = {1, 10};

  for (int i = 0; i < images.size(); ++i) {
    MLValue imageMLValue;
    TrainingUtil::CreateMLValue(TrainingUtil::GetCpuAllocator(), image_dims, images[i], &imageMLValue);
    MLValue labelMLValue;
    TrainingUtil::CreateMLValue(TrainingUtil::GetCpuAllocator(), label_dims, labels[i], &labelMLValue);

    auto data_per_run = make_unique<DataPerRun>();
    data_per_run->names_ = {"X", "labels"};
    data_per_run->values_ = {imageMLValue, labelMLValue};
    data_per_run->label_index_ = 1;
    data_for_training.emplace_back(move(data_per_run));
  }
}

void PrepareMNISTData(const string& data_folder,
                      TrainingRunner::TrainingData& training_data,
                      TrainingRunner::TestData& test_data) {
  printf("Loading MNIST data ...\n");
  mnist::MNIST_dataset<std::vector, Image, Label> dataset =
      mnist::read_dataset<std::vector, std::vector, uint8_t, uint8_t>(data_folder);

  printf("#training images = %d \n", static_cast<int>(dataset.training_images.size()));
  printf("#training labels = %d \n", static_cast<int>(dataset.training_labels.size()));
  printf("#test images = %d \n", static_cast<int>(dataset.test_images.size()));
  printf("#test labels = %d \n", static_cast<int>(dataset.test_labels.size()));

  printf("Preparing data ...\n");
  auto training_images_labels = NormalizeData(dataset.training_images, dataset.training_labels);
  auto test_images_labels = NormalizeData(dataset.test_images, dataset.test_labels);
  ConvertData(training_images_labels.first, training_images_labels.second, training_data);
  ConvertData(test_images_labels.first, test_images_labels.second, test_data);
  printf("Preparing data: done\n");
}
