// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "all.h"

namespace onnxruntime {
namespace cuda {

#define REGISTER_ALL_KERNEL_TYPED(T)                                            \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                                \
      All,                                                                      \
      kOnnxDomain,                                                              \
      9,                                                                        \
      T,                                                                        \
      kCudaExecutionProvider,                                                   \
      KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<T>()), \
      All<T>);

template <typename T>
Status All<T>::ComputeInternal(OpKernelContext* ctx) const {
  const Tensor& input = *ctx->Input<Tensor>(0);
  Tensor& output = *ctx->Output(0, {});

  const auto size = input.Shape().Size();
  ORT_ENFORCE(size <= std::numeric_limits<int>::max(), "Number of reduced elements (",
              size, ") exceeds the max allowed value (", std::numeric_limits<int>::max(), ").");

  LaunchAllKernel(
    input.Data<T>(),
    static_cast<int>(size),
    output.MutableData<bool>());

  return Status::OK();
}

REGISTER_ALL_KERNEL_TYPED(bool)

}  // namespace cuda
}  // namespace onnxruntime