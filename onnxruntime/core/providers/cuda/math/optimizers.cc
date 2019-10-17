// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "optimizers.h"
#include "binary_elementwise_ops.h"
#include "core/providers/cuda/reduction/reduction_functions.h"
#include "core/providers/cuda/cuda_allocator.h"

namespace onnxruntime {
namespace cuda {

ONNX_OPERATOR_KERNEL_EX(
    SGDOptimizer,
    kOnnxDomain,
    9,
    kCudaExecutionProvider,
    KernelDefBuilder()
        .Alias(1, 0)  // Update weights in-place
        .TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
    SGDOptimizer);

Status SGDOptimizer::ComputeInternal(OpKernelContext* ctx) const {
  const Tensor& ETA = *ctx->Input<Tensor>(0);
  const Tensor& W = *ctx->Input<Tensor>(1);
  const Tensor& G = *ctx->Input<Tensor>(2);
  Tensor& NW = *ctx->Output(0, W.Shape());

  ORT_ENFORCE(W.Shape() == G.Shape());

  SGDOptimizerImpl(
      ETA.template Data<float>(),
      W.template Data<float>(),
      G.template Data<float>(),
      NW.template MutableData<float>(),
      W.Shape().Size());

  return Status::OK();
}

template <typename T>
Status CopyIfNotSameBuffer(const Tensor& source_tensor, Tensor& target_tensor) {
  const T* source = source_tensor.template Data<T>();
  T* target = target_tensor.template MutableData<T>();
  if (target != source) {
    CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(target, source, source_tensor.SizeInBytes(), cudaMemcpyDeviceToDevice));
  }
  return Status::OK();
}

// TODO: Once Schema is checked in to onnx lets fix this to match that
#define REGISTER_ADAM_KERNEL_TYPED(T1, T2, T3, T4, T_GRAD)                            \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                                      \
      AdamOptimizer,                                                                  \
      kOnnxDomain,                                                                    \
      9,                                                                              \
      T1##_##T2##_##T3##_##T4##_##T_GRAD,                                             \
      kCudaExecutionProvider,                                                         \
      KernelDefBuilder()                                                              \
          .Alias(1, 3)                             /* Update step count in-place */   \
          .Alias(2, 0)                             /* Update weights in-place */      \
          .Alias(4, 1)                             /* Update moment-1 in-place */     \
          .Alias(5, 2)                             /* Update moment-2 in-place */     \
          .Alias(6, 4)                             /* Update FP16 weights in-place */ \
          .InputMemoryType<OrtMemTypeCPUInput>(1)  /* Keep step count in CPU */       \
          .InputMemoryType<OrtMemTypeCPUInput>(7)  /* Keep noop_flag in CPU */        \
          .OutputMemoryType<OrtMemTypeCPUInput>(3) /* Keep step count in CPU */       \
          .TypeConstraint("T1", DataTypeImpl::GetTensorType<T1>())                    \
          .TypeConstraint("T2", DataTypeImpl::GetTensorType<T2>())                    \
          .TypeConstraint("T3", DataTypeImpl::GetTensorType<T3>())                    \
          .TypeConstraint("T4", DataTypeImpl::GetTensorType<T4>())                    \
          .TypeConstraint("T_GRAD", DataTypeImpl::GetTensorType<T_GRAD>())            \
          .TypeConstraint("T_FP16", DataTypeImpl::GetTensorType<MLFloat16>())         \
          .TypeConstraint("B", DataTypeImpl::GetTensorType<bool>()),                  \
      AdamOptimizer<T1, T2, T3, T4, T_GRAD>);

REGISTER_ADAM_KERNEL_TYPED(float, int64_t, float, float, float)
REGISTER_ADAM_KERNEL_TYPED(MLFloat16, int64_t, float, MLFloat16, float)
REGISTER_ADAM_KERNEL_TYPED(float, int64_t, float, MLFloat16, float)
REGISTER_ADAM_KERNEL_TYPED(float, int64_t, float, float, MLFloat16)
REGISTER_ADAM_KERNEL_TYPED(MLFloat16, int64_t, float, MLFloat16, MLFloat16)
REGISTER_ADAM_KERNEL_TYPED(float, int64_t, float, MLFloat16, MLFloat16)

template <typename T1, typename T2, typename T3, typename T4, typename T_GRAD>
Status AdamOptimizer<T1, T2, T3, T4, T_GRAD>::ComputeInternal(OpKernelContext* ctx) const {
  typedef typename ToCudaType<T1>::MappedType CudaT1;
  typedef typename ToCudaType<T3>::MappedType CudaT3;
  typedef typename ToCudaType<T4>::MappedType CudaT4;
  typedef typename ToCudaType<T_GRAD>::MappedType CudaT_GRAD;

  const Tensor& ETA = *ctx->Input<Tensor>(0);
  const Tensor& S = *ctx->Input<Tensor>(1);
  const Tensor& W = *ctx->Input<Tensor>(2);
  const Tensor& G = *ctx->Input<Tensor>(3);
  const Tensor& M1 = *ctx->Input<Tensor>(4);
  const Tensor& M2 = *ctx->Input<Tensor>(5);

  Tensor& NW = *ctx->Output(0, W.Shape());
  Tensor& NM1 = *ctx->Output(1, M1.Shape());
  Tensor& NM2 = *ctx->Output(2, M2.Shape());
  Tensor& NS = *ctx->Output(3, S.Shape());

  half* fp16_weights_out = nullptr;

  if (ctx->InputCount() >= 7 && ctx->OutputCount() >= 5) {
    const Tensor& W_FP16 = *ctx->Input<Tensor>(6);
    Tensor& NW_FP16 = *ctx->Output(4, W_FP16.Shape());
    fp16_weights_out = reinterpret_cast<half*>(NW_FP16.template MutableData<MLFloat16>());
  }

  const T2* S_in = S.template Data<T2>();
  if (ctx->InputCount() >= 8) {
    const Tensor& do_update_tensor = *ctx->Input<Tensor>(7);
    const bool do_update = *do_update_tensor.template Data<bool>();
    if (!do_update) {
      ORT_RETURN_IF_ERROR(CopyIfNotSameBuffer<T3>(W, NW));
      ORT_RETURN_IF_ERROR(CopyIfNotSameBuffer<T4>(M1, NM1));
      ORT_RETURN_IF_ERROR(CopyIfNotSameBuffer<T4>(M2, NM2));
      if (S_in != NS.template MutableData<T2>()) {
        *(NS.template MutableData<T2>()) = *(S_in);
      }

      if (fp16_weights_out) {
        const Tensor& W_FP16 = *ctx->Input<Tensor>(6);
        Tensor& NW_FP16 = *ctx->Output(4, W_FP16.Shape());
        ORT_RETURN_IF_ERROR(CopyIfNotSameBuffer<MLFloat16>(W_FP16, NW_FP16));
      }
      return Status::OK();
    }
  }

  AdamOptimizerImpl(
      reinterpret_cast<const CudaT1*>(ETA.template Data<T1>()),
      *S_in,
      reinterpret_cast<const CudaT3*>(W.template Data<T3>()),
      reinterpret_cast<const CudaT_GRAD*>(G.template Data<T_GRAD>()),
      reinterpret_cast<const CudaT4*>(M1.template Data<T4>()),
      reinterpret_cast<const CudaT4*>(M2.template Data<T4>()),
      ToCudaType<T4>::FromFloat(alpha_),
      ToCudaType<T4>::FromFloat(beta_),
      ToCudaType<T4>::FromFloat(lambda_),
      ToCudaType<T4>::FromFloat(epsilon_),
      reinterpret_cast<CudaT3*>(NW.template MutableData<T3>()),
      reinterpret_cast<CudaT4*>(NM1.template MutableData<T4>()),
      reinterpret_cast<CudaT4*>(NM2.template MutableData<T4>()),
      fp16_weights_out,
      W.Shape().Size());

  *(NS.template MutableData<T2>()) = *(S_in) + 1;

  return Status::OK();
}

// TODO: Once Schema is checked in to onnx lets fix this to match that
#define REGISTER_LAMB_KERNEL_TYPED(T1, T2, T3, T4)                                   \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                                     \
      LambOptimizer,                                                                 \
      kOnnxDomain,                                                                   \
      9,                                                                             \
      T1##_##T2##_##T3##_##T4,                                                       \
      kCudaExecutionProvider,                                                        \
      KernelDefBuilder()                                                             \
          .Alias(1, 0)                            /* Update weights in-place */      \
          .Alias(3, 1)                            /* Update moment-1 in-place */     \
          .Alias(4, 2)                            /* Update moment-2 in-place */     \
          .Alias(5, 3)                            /* Update FP16 weights in-place */ \
          .InputMemoryType<OrtMemTypeCPUInput>(6) /* Keep noop_flag in CPU */        \
          .TypeConstraint("T1", DataTypeImpl::GetTensorType<T1>())                   \
          .TypeConstraint("T2", DataTypeImpl::GetTensorType<T2>())                   \
          .TypeConstraint("T3", DataTypeImpl::GetTensorType<T3>())                   \
          .TypeConstraint("T4", DataTypeImpl::GetTensorType<T4>())                   \
          .TypeConstraint("T_FP16", DataTypeImpl::GetTensorType<MLFloat16>())        \
          .TypeConstraint("B", DataTypeImpl::GetTensorType<bool>()),                 \
      LambOptimizer<T1, T2, T3, T4>);

REGISTER_LAMB_KERNEL_TYPED(float, float, MLFloat16, float)
REGISTER_LAMB_KERNEL_TYPED(float, float, float, float)
REGISTER_LAMB_KERNEL_TYPED(double, double, double, double)
REGISTER_LAMB_KERNEL_TYPED(MLFloat16, float, MLFloat16, MLFloat16)
REGISTER_LAMB_KERNEL_TYPED(MLFloat16, float, MLFloat16, float)

template <typename T1, typename T2, typename T3, typename T4>
Status LambOptimizer<T1, T2, T3, T4>::ComputeInternal(OpKernelContext* ctx) const {
  // CudaT* are types used to invoke CUDA-based functions. It, for example, maps
  // MLFloat16 in ONNXRuntime to half in CUDA.
  typedef typename ToCudaType<T1>::MappedType CudaT1;
  typedef typename ToCudaType<T2>::MappedType CudaT2;
  typedef typename ToCudaType<T3>::MappedType CudaT3;
  typedef typename ToCudaType<T4>::MappedType CudaT4;

  const Tensor& eta_tensor = *ctx->Input<Tensor>(0);
  const Tensor& weights_tensor = *ctx->Input<Tensor>(1);
  const Tensor& gradients_tensor = *ctx->Input<Tensor>(2);
  const Tensor& moment_1_tensor = *ctx->Input<Tensor>(3);
  const Tensor& moment_2_tensor = *ctx->Input<Tensor>(4);

  const TensorShape& weight_tensor_shape = weights_tensor.Shape();
  const auto weight_tensor_size = weights_tensor.Shape().Size();

  ORT_ENFORCE(weight_tensor_shape == gradients_tensor.Shape());
  ORT_ENFORCE(weight_tensor_shape == moment_1_tensor.Shape());
  ORT_ENFORCE(weight_tensor_shape == moment_2_tensor.Shape());

  // It's an alias of weights_tensor, which leads to an in-place update.
  Tensor& weights_tensor_updated = *ctx->Output(0, weight_tensor_shape);
  Tensor& moment_1_tensor_updated = *ctx->Output(1, weight_tensor_shape);
  Tensor& moment_2_tensor_updated = *ctx->Output(2, weight_tensor_shape);

  half* fp16_weights_updated = nullptr;
  if (ctx->InputCount() >= 6 && ctx->OutputCount() >= 4) {
    const Tensor& fp16_weights_tensor = *ctx->Input<Tensor>(5);
    Tensor& fp16_weights_tensor_updated = *ctx->Output(3, fp16_weights_tensor.Shape());
    fp16_weights_updated = reinterpret_cast<half*>(fp16_weights_tensor_updated.template MutableData<MLFloat16>());
  }

  if (ctx->InputCount() >= 7) {
    const Tensor& do_update_tensor = *ctx->Input<Tensor>(6);
    const bool do_update = *do_update_tensor.template Data<bool>();
    if (!do_update) {
      ORT_RETURN_IF_ERROR(CopyIfNotSameBuffer<T2>(weights_tensor, weights_tensor_updated));
      ORT_RETURN_IF_ERROR(CopyIfNotSameBuffer<T4>(moment_1_tensor, moment_1_tensor_updated));
      ORT_RETURN_IF_ERROR(CopyIfNotSameBuffer<T4>(moment_2_tensor, moment_2_tensor_updated));

      if (fp16_weights_updated) {
        const Tensor& fp16_weights_tensor = *ctx->Input<Tensor>(5);
        Tensor& fp16_weights_tensor_updated = *ctx->Output(3, fp16_weights_tensor.Shape());
        ORT_RETURN_IF_ERROR(CopyIfNotSameBuffer<MLFloat16>(fp16_weights_tensor, fp16_weights_tensor_updated));
      }
      return Status::OK();
    }
  }

  // Compute update direction and the 1st and the 2nd momentums.
  // Gradient type controls the direction's type.
  IAllocatorUniquePtr<T3> update_direction_buffer = GetScratchBuffer<T3>(weight_tensor_size);
  LambComputeDirectionImpl(
      reinterpret_cast<const CudaT2*>(weights_tensor.template Data<T2>()),
      reinterpret_cast<const CudaT3*>(gradients_tensor.template Data<T3>()),
      reinterpret_cast<const CudaT4*>(moment_1_tensor.template Data<T4>()),
      reinterpret_cast<const CudaT4*>(moment_2_tensor.template Data<T4>()),
      ToCudaType<T4>::FromFloat(alpha_),
      ToCudaType<T4>::FromFloat(beta_),
      ToCudaType<T2>::FromFloat(lambda_),
      ToCudaType<T4>::FromFloat(epsilon_),
      reinterpret_cast<CudaT3*>(update_direction_buffer.get()),
      reinterpret_cast<CudaT4*>(moment_1_tensor_updated.template MutableData<T4>()),
      reinterpret_cast<CudaT4*>(moment_2_tensor_updated.template MutableData<T4>()),
      weight_tensor_size);

  // Allocate buffer for reduction computation of update direction.
  // We reduce type T3 tensor to type T2 scalar. An example is that T3=float16
  // and T2=float.
  IAllocatorUniquePtr<T2> direction_norm_buffer = GetScratchBuffer<T2>(1);
  // Allocate buffer for reduction computation of weight tensor.
  // We reduce type T3 tensor to type T2 scalar. An example is that T3=float16
  // and T2=float.
  IAllocatorUniquePtr<T2> weights_norm_buffer = GetScratchBuffer<T2>(1);

  // Compute buffer size in byte for reduction APIs.
  const auto buffer_size = static_cast<size_t>(
      compute_reduction_buffer_size(
          static_cast<int>(sizeof(T2)), static_cast<int>(weight_tensor_size)));

  // Allocate reduction buffer whose size is buffer_size bytes.
  IAllocatorUniquePtr<void> reduction_buffer = GetScratchBuffer<void>(buffer_size);

  // We should throw for overflow in reduction APIs.
  // The index in CUDA system is integer.
  ORT_ENFORCE(
      weight_tensor_size <
      static_cast<int64_t>(std::numeric_limits<int>::max()));

  reduce_l2_norm(
      reinterpret_cast<const CudaT2*>(weights_tensor.template Data<T2>()),
      weights_norm_buffer.get(),
      static_cast<int>(weight_tensor_size),
      reinterpret_cast<CudaT2*>(reduction_buffer.get()));

  reduce_l2_norm(
      reinterpret_cast<CudaT3*>(update_direction_buffer.get()),
      direction_norm_buffer.get(),
      static_cast<int>(weight_tensor_size),
      reinterpret_cast<CudaT2*>(reduction_buffer.get()));

  // Use the update direction and the computed norms to compute
  // the new weights.
  LambUpdateImpl(
      reinterpret_cast<const CudaT1*>(eta_tensor.template Data<T1>()),
      reinterpret_cast<const CudaT2*>(direction_norm_buffer.get()),
      reinterpret_cast<const CudaT2*>(weights_norm_buffer.get()),
      reinterpret_cast<const CudaT2*>(weights_tensor.template Data<T2>()),
      ToCudaType<T2>::FromFloat(threshold_),
      reinterpret_cast<CudaT3*>(update_direction_buffer.get()),
      reinterpret_cast<CudaT2*>(weights_tensor_updated.template MutableData<T2>()),
      fp16_weights_updated,
      weight_tensor_size);

  return Status::OK();
}

template <typename T, typename T_GRAD>
Status AccumulateGradient<T, T_GRAD>::ComputeInternal(OpKernelContext* ctx) const {
  typedef typename ToCudaType<T>::MappedType CudaT;
  typedef typename ToCudaType<T_GRAD>::MappedType CudaT_GRAD;

  const Tensor& gradient_buffer = *ctx->Input<Tensor>(0);
  const Tensor& gradient = *ctx->Input<Tensor>(1);
  Tensor& accumulated_gradient = *ctx->Output(0, gradient_buffer.Shape());

  AccumulateGradientImpl(
      reinterpret_cast<const CudaT*>(gradient_buffer.template Data<T>()),
      reinterpret_cast<const CudaT_GRAD*>(gradient.template Data<T_GRAD>()),
      reinterpret_cast<CudaT*>(accumulated_gradient.template MutableData<T>()),
      gradient.Shape().Size());

  return Status::OK();
}

#define REGISTER_GRADIENT_ACCUMULATOR_TYPED(T, T_GRAD)                      \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                            \
      GradientAccumulator,                                                  \
      kOnnxDomain,                                                          \
      9,                                                                    \
      T##_##T_GRAD,                                                         \
      kCudaExecutionProvider,                                               \
      KernelDefBuilder()                                                    \
          .Alias(0, 0) /* Accumulate gradients in-place */                  \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<T>())            \
          .TypeConstraint("T_GRAD", DataTypeImpl::GetTensorType<T_GRAD>()), \
      AccumulateGradient<T, T_GRAD>);
REGISTER_GRADIENT_ACCUMULATOR_TYPED(float, float)
REGISTER_GRADIENT_ACCUMULATOR_TYPED(float, MLFloat16)

template <typename T>
Status ZeroGradient<T>::ComputeInternal(OpKernelContext* ctx) const {
  const Tensor& old_gradient = *ctx->Input<Tensor>(0);
  Tensor& zero_gradient = *ctx->Output(0, old_gradient.Shape());

  CUDA_RETURN_IF_ERROR(cudaMemset(zero_gradient.template MutableData<T>(), 0, zero_gradient.Shape().Size() * sizeof(T)));
  return Status::OK();
}

#define REGISTER_ZERO_GRADIENT_TYPED(T)                           \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                  \
      ZeroGradient,                                               \
      kOnnxDomain,                                                \
      9,                                                          \
      T,                                                          \
      kCudaExecutionProvider,                                     \
      KernelDefBuilder()                                          \
          .Alias(0, 0) /* Zero out gradients in-place */          \
          .TypeConstraint("T1", DataTypeImpl::GetTensorType<T>()) \
          .TypeConstraint("T2", DataTypeImpl::AllTensorTypes()),  \
      ZeroGradient<T>);
REGISTER_ZERO_GRADIENT_TYPED(float)
REGISTER_ZERO_GRADIENT_TYPED(MLFloat16)

}  // namespace cuda
}  // namespace onnxruntime