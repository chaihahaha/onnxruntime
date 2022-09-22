// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "layer_norm_impl.h"

#include "core/common/safeint.h"
#include "core/framework/tensor.h"
#include "core/platform/threadpool.h"
#include "core/providers/common.h"
#include "core/util/math_cpuonly.h"

namespace onnxruntime {

LayerNormImpl::LayerNormImpl(const OpKernelInfo& op_kernel_info, bool simplified)
    : OpKernel(op_kernel_info), simplified_{simplified} {
  ORT_ENFORCE(op_kernel_info.GetAttr("axis", &axis_).IsOK());
  ORT_ENFORCE(op_kernel_info.GetAttr<float>("epsilon", &epsilon_).IsOK());
}

template <typename T>
struct LayerNormImpl::ComputeImpl {
  Status operator()(OpKernelContext* p_ctx, int64_t orig_axis, float epsilon, bool simplified) const {
    // Inputs
    const Tensor* X = p_ctx->Input<Tensor>(0);
    const Tensor* scale = p_ctx->Input<Tensor>(1);
    const Tensor* bias = p_ctx->Input<Tensor>(2);
    auto X_data = X->Data<T>();
    auto scale_data = scale->Data<T>();
    auto bias_data = (simplified || nullptr == bias) ? nullptr : bias->Data<T>();

    const TensorShape& x_shape = X->Shape();
    const int64_t axis = HandleNegativeAxis(orig_axis, x_shape.NumDimensions());
    auto norm_count = x_shape.SizeToDimension(axis);
    auto norm_size = x_shape.SizeFromDimension(axis);

    Tensor* Y = p_ctx->Output(0, x_shape);
    auto Y_data = Y->MutableData<T>();

    std::vector<int64_t> mean_inv_std_dev_dim;
    mean_inv_std_dev_dim.reserve(x_shape.NumDimensions());
    for (int i = 0; i < static_cast<int>(x_shape.NumDimensions()); ++i) {
      if (i < axis) {
        mean_inv_std_dev_dim.emplace_back(x_shape.GetDims()[i]);
      } else {
        mean_inv_std_dev_dim.emplace_back(1);
      }
    }

    AllocatorPtr alloc;
    ORT_RETURN_IF_ERROR(p_ctx->GetTempSpaceAllocator(&alloc));

    T* mean_data = nullptr;
    BufferUniquePtr mean_data_buf_ptr;

    int output_index = 1;

    if (!simplified) {
      Tensor* mean = p_ctx->Output(output_index++, TensorShape(mean_inv_std_dev_dim));
      if (mean != nullptr) {
        mean_data = mean->MutableData<T>();
      } else {
        auto mean_data_buf = alloc->Alloc(SafeInt<size_t>(sizeof(T)) * norm_count);
        mean_data_buf_ptr = BufferUniquePtr(mean_data_buf, BufferDeleter(alloc));
        mean_data = static_cast<T*>(mean_data_buf_ptr.get());
      }
    }

    T* inv_std_dev_data = nullptr;
    BufferUniquePtr inv_std_dev_data_buf_ptr;

    Tensor* inv_std_dev = p_ctx->Output(output_index, TensorShape(mean_inv_std_dev_dim));
    if (inv_std_dev != nullptr) {
      inv_std_dev_data = inv_std_dev->MutableData<T>();
    } else {
      auto inv_std_dev_data_buf = alloc->Alloc(SafeInt<size_t>(sizeof(T)) * norm_count);
      inv_std_dev_data_buf_ptr = BufferUniquePtr(inv_std_dev_data_buf, BufferDeleter(alloc));
      inv_std_dev_data = static_cast<T*>(inv_std_dev_data_buf_ptr.get());
    }

    concurrency::ThreadPool::TryBatchParallelFor(
        p_ctx->GetOperatorThreadPool(), static_cast<int32_t>(norm_count),
        [&](ptrdiff_t task_idx) {
          const T* p_input = X_data + task_idx * norm_size;
          T* p_output = Y_data + task_idx * norm_size;

          T mean = 0;
          T mean_square = 0;

          for (int64_t h = 0; h < norm_size; h++) {
            mean += p_input[h];
            mean_square += p_input[h] * p_input[h];
          }

          mean = mean / norm_size;
          if (simplified) {
            mean_square = sqrt(mean_square / norm_size + epsilon);
          } else {
            mean_square = sqrt(mean_square / norm_size - mean * mean + epsilon);
          }

          for (int64_t h = 0; h < norm_size; h++) {
            if (simplified) {
              p_output[h] = p_input[h] / mean_square * scale_data[h];
            } else if (nullptr == bias) {
              p_output[h] = (p_input[h] - mean) / mean_square * scale_data[h];
            } else {
              p_output[h] = (p_input[h] - mean) / mean_square * scale_data[h] + bias_data[h];
            }
          }

          if (mean_data != nullptr) {
            mean_data[task_idx] = mean;
          }
          inv_std_dev_data[task_idx] = 1 / mean_square;
        },
        0);

    return Status::OK();
  }
};

Status LayerNormImpl::Compute(OpKernelContext* p_ctx) const {
  const auto elem_type = p_ctx->Input<Tensor>(0)->GetElementType();

  // minor optimization to exclude support for 'double' as that's only used by the contrib op version.
#if defined DISABLE_CONTRIB_OPS
  using SupportedTypeList = boost::mp11::mp_list<float>;
#else
  using SupportedTypeList = boost::mp11::mp_list<float, double>;
#endif

  utils::MLTypeCallDispatcherFromTypeList<SupportedTypeList> t_disp(elem_type);
  return t_disp.InvokeRet<Status, ComputeImpl>(p_ctx, axis_, epsilon_, simplified_);
}

}  // namespace onnxruntime
