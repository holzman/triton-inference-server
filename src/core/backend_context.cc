// Copyright (c) 2019-2020, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "src/core/backend_context.h"

#include "src/core/cuda_utils.h"
#include "src/core/logging.h"
#include "src/core/nvtx.h"

namespace nvidia { namespace inferenceserver {

namespace {

TRITONSERVER_MemoryType
GetUsePinnedMemoryType(TRITONSERVER_MemoryType ref_buffer_type)
{
  // The following matrix is used for both input and output.
  // src   \ dest | non-pinned    | pinned     | device
  // non-pinned   | memcpy        | memcpy     | buffer needed
  // pinned       | memcpy        | memcpy     | cudaMemcpy
  // device       | buffer needed | cudaMemcpy | cudaMemcpy
  if (ref_buffer_type == TRITONSERVER_MEMORY_CPU_PINNED) {
    return TRITONSERVER_MEMORY_CPU_PINNED;
  }

  return (ref_buffer_type == TRITONSERVER_MEMORY_CPU) ? TRITONSERVER_MEMORY_GPU
                                                      : TRITONSERVER_MEMORY_CPU;
}

}  // namespace

BackendContext::BackendContext(
    const std::string& name, const int gpu_device, const int max_batch_size,
    const bool enable_pinned_input, const bool enable_pinned_output)
    : name_(name), gpu_device_(gpu_device), max_batch_size_(max_batch_size),
      enable_pinned_input_(enable_pinned_input),
      enable_pinned_output_(enable_pinned_output)
{
#ifdef TRTIS_ENABLE_GPU
  stream_ = nullptr;
#endif  // TRTIS_ENABLE_GPU
}

BackendContext::~BackendContext()
{
#ifdef TRTIS_ENABLE_GPU
  if (stream_ != nullptr) {
    cudaError_t err = cudaStreamDestroy(stream_);
    if (err != cudaSuccess) {
      LOG_ERROR << "Failed to destroy cuda stream: " << cudaGetErrorString(err);
    }
    stream_ = nullptr;
  }
#endif  // TRTIS_ENABLE_GPU
}

Status
BackendContext::CreateCudaStream(
    const int cuda_stream_priority, cudaStream_t* stream)
{
#ifdef TRTIS_ENABLE_GPU
  if (gpu_device_ != NO_GPU_DEVICE) {
    // Make sure that correct device is set before creating stream and
    // then restore the device to what was set by the caller.
    int current_device;
    auto cuerr = cudaGetDevice(&current_device);
    bool overridden = false;
    if (cuerr == cudaSuccess) {
      overridden = (current_device != gpu_device_);
      if (overridden) {
        cuerr = cudaSetDevice(gpu_device_);
      }
    }

    if (cuerr == cudaSuccess) {
      cudaStream_t* s = (stream == nullptr) ? &stream_ : stream;
      cuerr = cudaStreamCreateWithPriority(
          s, cudaStreamDefault, cuda_stream_priority);
    }

    if (overridden) {
      cudaSetDevice(current_device);
    }

    if (cuerr != cudaSuccess) {
      return Status(
          Status::Code::INTERNAL, "unable to create stream for " + name_ +
                                      ": " + cudaGetErrorString(cuerr));
    }
  }
#endif  // TRTIS_ENABLE_GPU

  return Status::Success;
}

bool
BackendContext::SetInputBuffer(
    const std::string& name, const std::vector<size_t>& expected_byte_sizes,
    std::vector<std::unique_ptr<InferenceRequest>>* requests, InputInfo* input)
{
  return SetInputBuffer(name, expected_byte_sizes, requests, stream_, input);
}

bool
BackendContext::SetInputBuffer(
    const std::string& name, const std::vector<size_t>& expected_byte_sizes,
    std::vector<std::unique_ptr<InferenceRequest>>* requests,
    cudaStream_t stream, InputInfo* input)
{
  bool cuda_copy = false;

  TRITONSERVER_MemoryType use_pinned_memory_type =
      TRITONSERVER_MEMORY_CPU_PINNED;
  if (enable_pinned_input_) {
    use_pinned_memory_type = GetUsePinnedMemoryType(input->memory_type_);
  }

  BufferInfo pinned_buffer_info{0, 0, {}};

  // Visit the requests in order and copy the input tensors to
  // 'buffer'.
  size_t buffer_copy_offset = 0;
  for (size_t idx = 0; idx < expected_byte_sizes.size(); idx++) {
    auto& request = (*requests)[idx];
    const size_t expected_byte_size = expected_byte_sizes[idx];

    const InferenceRequest::Input* rinput;
    Status status = request->ImmutableInput(name, &rinput);
    if (!status.IsOk()) {
      // FIXME      InferenceRequest::RespondWithError(request, status);
      break;
    }

    const std::shared_ptr<Memory>& data = rinput->Data();

    size_t copied_byte_size = 0;
    size_t data_idx = 0;
    while (request != nullptr) {
      auto src_memory_type = input->memory_type_;
      auto src_memory_type_id = input->memory_type_id_;
      size_t content_byte_size = expected_byte_size - copied_byte_size;
      const void* content = data->BufferAt(
          data_idx, &content_byte_size, &src_memory_type, &src_memory_type_id);

      // No more input content available then done with copying...
      if (content == nullptr) {
        break;
      }

      if ((copied_byte_size + content_byte_size) > expected_byte_size) {
        // FIXME        InferenceRequest::RespondWithError(
        //            request,
        //            Status(
        //                Status::Code::INVALID_ARG,
        //                "unexpected size " +
        //                    std::to_string(copied_byte_size +
        //                    content_byte_size) + " for inference input '" +
        //                    name + "', expecting " +
        //                    std::to_string(expected_byte_size)));
        break;
      }

      if (content_byte_size > 0) {
        // Defer memory copy for the buffer if it's better put into an
        // intermediate pinned buffer first.
        if ((use_pinned_memory_type != TRITONSERVER_MEMORY_CPU_PINNED) &&
            (src_memory_type == use_pinned_memory_type)) {
          std::get<1>(pinned_buffer_info) += content_byte_size;
          std::get<2>(pinned_buffer_info)
              .emplace_back(idx, data.get(), data_idx);
        } else {
          // If copy should be perform directly, two steps to be done:
          // 1. Issue copy for the current buffer
          // 2. Finalize the existing intermediate buffer
          bool cuda_used = false;
          Status status = CopyBuffer(
              name, src_memory_type, src_memory_type_id, input->memory_type_,
              input->memory_type_id_, content_byte_size, content,
              input->input_buffer_ + buffer_copy_offset + copied_byte_size,
              stream, &cuda_used);
          if (!status.IsOk()) {
            // FIXME            InferenceRequest::RespondWithError(request,
            // status);
          }

          cuda_copy |= cuda_used;

          if (std::get<1>(pinned_buffer_info) > 0) {
            cuda_copy |= IssueIndirectInputBufferCopy(
                name, pinned_buffer_info, requests, stream, input);
          }
          // always reset 'pinned_buffer_info' to maintain proper input offset
          pinned_buffer_info = BufferInfo{
              buffer_copy_offset + copied_byte_size + content_byte_size, 0, {}};
        }
      }

      copied_byte_size += content_byte_size;
      data_idx++;
    }

    if ((request != nullptr) && (copied_byte_size != expected_byte_size)) {
      // FIXME      InferenceRequest::RespondWithError(
      //          request, Status(
      //                      Status::Code::INTERNAL,
      //                    "expected " + std::to_string(expected_byte_size) +
      //                      " bytes of data for inference input '" + name +
      //                    "', got " + std::to_string(copied_byte_size)));
    }

    // When the request is nullptr that indicates that an error
    // occurred during the above processing, maintain a new indirect
    // buffer as the contiguousity ends here. And there are pending
    // indirect buffer copies, issue them.
    if (request == nullptr) {
      if (std::get<1>(pinned_buffer_info) > 0) {
        cuda_copy |= IssueIndirectInputBufferCopy(
            name, pinned_buffer_info, requests, stream, input);
      }
      // reset 'pinned_buffer_info'
      pinned_buffer_info =
          BufferInfo{buffer_copy_offset + expected_byte_size, 0, {}};
    }

    buffer_copy_offset += expected_byte_size;
  }

  // Issue pending indirect copy if any
  if (std::get<1>(pinned_buffer_info) > 0) {
    cuda_copy |= IssueIndirectInputBufferCopy(
        name, pinned_buffer_info, requests, stream, input);
  }

  return cuda_copy;
}

bool
BackendContext::IssueIndirectInputBufferCopy(
    const std::string& name,
    const BackendContext::BufferInfo& pinned_buffer_info,
    std::vector<std::unique_ptr<InferenceRequest>>* requests,
    cudaStream_t stream, InputInfo* input)
{
  NVTX_RANGE(nvtx_, "IndirectInputBufferCopy");

  bool cuda_copy = false;
  bool cuda_used = false;
  auto mem_type = TRITONSERVER_MEMORY_CPU_PINNED;
  int64_t mem_id = 0;
  const auto input_offset = std::get<0>(pinned_buffer_info);
  const auto pinned_buffer_size = std::get<1>(pinned_buffer_info);
  std::unique_ptr<AllocatedMemory> local_indirect_buffer(
      new AllocatedMemory(pinned_buffer_size, mem_type, mem_id));
  char* buffer = local_indirect_buffer->MutableBuffer(&mem_type, &mem_id);
  std::vector<size_t> request_idxs;

  // If can't reserve the intermediate buffer, the copy should be
  // perform directly to input buffer
  bool direct_copy = (mem_type != TRITONSERVER_MEMORY_CPU_PINNED);
  if (direct_copy) {
    buffer = input->input_buffer_ + input_offset;
    mem_type = input->memory_type_;
    mem_id = input->memory_type_id_;
  }

  auto src_mem_type = input->memory_type_;
  auto src_mem_type_id = input->memory_type_id_;
  size_t src_byte_size;
  size_t buffer_offset = 0;
  for (const auto& data_info : std::get<2>(pinned_buffer_info)) {
    request_idxs.emplace_back(std::get<0>(data_info));
    const void* src_data = std::get<1>(data_info)->BufferAt(
        std::get<2>(data_info), &src_byte_size, &src_mem_type,
        &src_mem_type_id);
    Status status = CopyBuffer(
        name, src_mem_type, src_mem_type_id, mem_type, mem_id, src_byte_size,
        src_data, buffer + buffer_offset, stream, &cuda_used);
    if (!status.IsOk()) {
      //      auto& request = (*requests)[request_idxs.back()];
      // FIXME      InferenceRequest::RespondWithError(request, status);
    }

    buffer_offset += src_byte_size;
    cuda_copy |= cuda_used;
  }

  if (!direct_copy) {
    input->indirect_buffers_.emplace_back(
        std::move(local_indirect_buffer), input_offset,
        std::move(request_idxs));
  }

  return cuda_copy;
}

bool
BackendContext::SetShapeInputBuffer(
    const std::string& name, const int32_t total_batch_size,
    const int expected_byte_size, const bool support_batching,
    std::unique_ptr<InferenceRequest>& request,
    TRITONSERVER_MemoryType dst_memory_type, int64_t dst_memory_type_id,
    char* input_buffer)
{
  if (request == nullptr) {
    return false;
  }

  size_t buffer_copy_offset = support_batching ? sizeof(int32_t) : 0;

  const InferenceRequest::Input* rinput;
  Status status = request->ImmutableInput(name, &rinput);
  if (!status.IsOk()) {
    // FIXME    InferenceRequest::RespondWithError(request, status);
    return false;
  }

  auto src_memory_type = dst_memory_type;
  auto src_memory_type_id = dst_memory_type_id;
  const void* content;
  size_t content_byte_size = expected_byte_size;

  // This code assumes that the entire tensor data is in a single
  // buffer... but the expected_byte_size check below will fail if
  // that is not the case.
  status = rinput->DataBuffer(
      0 /* idx */, &content, &content_byte_size, &src_memory_type,
      &src_memory_type_id);
  if (!status.IsOk()) {
    // FIXME    InferenceRequest::RespondWithError(request, status);
    return false;
  }

  if ((expected_byte_size) != (int)content_byte_size) {
    // FIXME    InferenceRequest::RespondWithError(
    //        request, Status(
    //                   Status::Code::INVALID_ARG,
    //                 "unexpected size " + std::to_string(content_byte_size) +
    //                   " for inference input '" + name + "', expecting " +
    //                 std::to_string(expected_byte_size)));
    return false;
  }

  bool cuda_copy = false;

  if (content_byte_size > 0) {
    bool cuda_used = false;
    status = CopyBuffer(
        name, src_memory_type, src_memory_type_id, dst_memory_type,
        dst_memory_type_id, expected_byte_size, content,
        input_buffer + buffer_copy_offset, stream_, &cuda_used);
    if (!status.IsOk()) {
      // FIXME      InferenceRequest::RespondWithError(request, status);
      return cuda_copy;
    }
  }

  if (support_batching) {
    bool cuda_used = false;
    status = CopyBuffer(
        name, TRITONSERVER_MEMORY_CPU, 0, dst_memory_type, dst_memory_type_id,
        sizeof(int32_t), (void*)&total_batch_size, input_buffer, stream_,
        &cuda_used);
    if (!status.IsOk()) {
      // FIXME      InferenceRequest::RespondWithError(request, status);
    }
    cuda_copy |= cuda_used;
  }

  return cuda_copy;
}

bool
BackendContext::SetOutputShapeTensorBuffer(
    const std::string& name, const int32_t* content,
    std::vector<int64_t>& content_shape, const bool support_batching,
    TRITONSERVER_MemoryType src_memory_type, int64_t src_memory_type_id,
    std::vector<std::unique_ptr<InferenceRequest>>* requests)
{
  if (content_shape.empty()) {
    return false;
  }

  bool cuda_copy = false;
  int shape_index = (support_batching ? 1 : 0);
  int nb_shape_values = content_shape[shape_index];
  for (auto& request : *requests) {
    int this_batch_size = request->BatchSize();
    // Fix the content shape for this request
    if (support_batching) {
      content_shape[0] = this_batch_size;
    }

    const size_t expected_byte_size =
        nb_shape_values * sizeof(int32_t) * this_batch_size;

    // If 'request' should have valid output (status ok) and
    // if 'request' requested this output then copy it from
    // 'content'. If it did not request this output then just
    // skip it in the 'content'.
    if ((request != nullptr) &&
        false /* FIXME request->RequiresOutput(name) */) {
      auto dst_memory_type = src_memory_type;
      int64_t dst_memory_type_id = 0;
      char* buffer = nullptr;

      Status status(Status::Code::INTERNAL, "NYI...");
#if 0
      // FIXME
      response_provider_->AllocateOutputBuffer(
          name, (void**)&buffer, expected_byte_size, content_shape,
          src_memory_type, src_memory_type_id, &dst_memory_type,
          &dst_memory_type_id);
#endif
      if (status.IsOk() && (expected_byte_size != 0)) {
        if (buffer == nullptr) {
          status = Status(
              Status::Code::INTERNAL,
              "failed to allocate buffer for output '" + name + "'");
        } else {
          bool cuda_used = false;
          size_t content_offset = support_batching ? 1 : 0;
          size_t buffer_offset = 0;
          for (int i = 0; i < this_batch_size; i++) {
            status = CopyBuffer(
                name, src_memory_type, src_memory_type_id, dst_memory_type,
                dst_memory_type_id, nb_shape_values * sizeof(int32_t),
                (void*)(content + content_offset),
                (void*)(buffer + buffer_offset), stream_, &cuda_used);
            cuda_copy |= cuda_used;
            buffer_offset += nb_shape_values * sizeof(int32_t);
          }
        }
      }

      if (!status.IsOk()) {
        // FIXME        InferenceRequest::RespondWithError(request, status);
      }
    }
  }

  return cuda_copy;
}


Status
BackendContext::GetContiguousInputContent(
    const std::string& name, TRITONSERVER_MemoryType memory_type,
    int64_t memory_type_id, const std::unique_ptr<InferenceRequest>& request,
    const char** content, size_t* content_byte_size,
    std::unique_ptr<AllocatedMemory>* contiguous_buffer, bool* cuda_copy)
{
  contiguous_buffer->reset();

  const InferenceRequest::Input* rinput;
  RETURN_IF_ERROR(request->ImmutableInput(name, &rinput));

  // Peek input buffers to check if data copy is necessary
  MemoryReference input_buffers;
  size_t chunk_count = 0;
  bool type_mismatch = false;
  for (size_t idx = 0; idx < rinput->DataBufferCount(); ++idx) {
    TRITONSERVER_MemoryType src_memory_type = memory_type;
    int64_t src_memory_type_id = memory_type_id;
    size_t src_byte_size = *content_byte_size;
    const void* src_ptr;

    RETURN_IF_ERROR(rinput->DataBuffer(
        idx, &src_ptr, &src_byte_size, &src_memory_type, &src_memory_type_id));

    if (src_ptr != nullptr) {
      input_buffers.AddBuffer(
          (const char*)src_ptr, src_byte_size, src_memory_type,
          src_memory_type_id);
      chunk_count++;
      type_mismatch |=
          ((src_memory_type != memory_type) ||
           (src_memory_type_id != memory_type_id));
    }
  }

  if (chunk_count == 0) {
    *content = nullptr;
    *content_byte_size = 0;
  } else if ((chunk_count == 1) && !type_mismatch) {
    *content = input_buffers.BufferAt(
        0, content_byte_size, &memory_type, &memory_type_id);
  } else {
    contiguous_buffer->reset(new AllocatedMemory(
        input_buffers.TotalByteSize(), memory_type, memory_type_id));
    auto dst_ptr =
        (*contiguous_buffer)->MutableBuffer(&memory_type, &memory_type_id);
    if (dst_ptr == nullptr) {
      return Status(
          Status::Code::INTERNAL, "failed to allocate contiguous buffer");
    }

    size_t offset = 0;
    for (size_t i = 0; i < chunk_count; i++) {
      bool cuda_used;
      TRITONSERVER_MemoryType src_memory_type;
      int64_t src_memory_type_id;
      auto src_ptr = input_buffers.BufferAt(
          i, content_byte_size, &src_memory_type, &src_memory_type_id);
      RETURN_IF_ERROR(CopyBuffer(
          name, src_memory_type, src_memory_type_id, memory_type,
          memory_type_id, *content_byte_size, src_ptr, dst_ptr + offset,
          stream_, &cuda_used));
      *cuda_copy |= cuda_used;
      offset += *content_byte_size;
    }

    *content = dst_ptr;
    *content_byte_size = (*contiguous_buffer)->TotalByteSize();
  }

  return Status::Success;
}

Status
BackendContext::CompareOutputDims(
    const std::string& tensor_name, const std::vector<int64_t>& model_shape,
    const DimsList& dims, const bool supports_batching)
{
  if (supports_batching) {
    DimsList full_dims;
    full_dims.Add(WILDCARD_DIM);
    for (int i = 0; i < dims.size(); ++i) {
      full_dims.Add(dims[i]);
    }

    bool succ = (model_shape.size() == (size_t)full_dims.size());
    if (succ) {
      for (int i = 0; i < full_dims.size(); ++i) {
        const int64_t model_dim = model_shape[i];
        if (full_dims[i] != WILDCARD_DIM) {
          succ &= (model_dim == full_dims[i]);
        }
      }
    }

    if (!succ) {
      return Status(
          Status::Code::INVALID_ARG,
          "tensor '" + tensor_name + "': the model expects " +
              std::to_string(model_shape.size()) + " dimensions (shape " +
              DimsListToString(model_shape) +
              ") but the model configuration specifies " +
              std::to_string(full_dims.size()) +
              " dimensions (an initial batch dimension because max_batch_size "
              "> 0 followed by the explicit tensor shape, making complete "
              "shape " +
              DimsListToString(full_dims) + ")");
    }
  } else {
    // ! supports_batching
    bool succ = (model_shape.size() == (size_t)dims.size());
    if (succ) {
      for (int i = 0; i < dims.size(); ++i) {
        const int64_t model_dim = model_shape[i];
        if (dims[i] != WILDCARD_DIM) {
          succ &= (model_dim == dims[i]);
        }
      }
    }

    if (!succ) {
      return Status(
          Status::Code::INVALID_ARG,
          "tensor '" + tensor_name + "': the model expects " +
              std::to_string(model_shape.size()) + " dimensions (shape " +
              DimsListToString(model_shape) +
              ") but the model configuration specifies " +
              std::to_string(dims.size()) + " dimensions (shape " +
              DimsListToString(dims) + ")");
    }
  }

  return Status::Success;
}

Status
BackendContext::PeekShapeTensor(
    const InferenceRequest::Input& input,
    const std::unique_ptr<InferenceRequest>& request,
    std::vector<int64_t>* shape)
{
  // By default a backend doesn't support shape tensors.
  return Status(Status::Code::INTERNAL, "shape tensors not supported");
}

//
// BackendResponder
//
void
BackendResponder::ProcessTensor(
    const std::string& name, const DataType datatype,
    const std::vector<int64_t>& shape, const char* buffer,
    const TRITONSERVER_MemoryType memory_type, const int64_t memory_type_id)
{
  // A value of CPU_PINNED indicates that pinned memory buffer is not
  // needed for this tensor. Any other value indicates that a pinned
  // memory buffer is needed when the target memory type matches
  // 'use_pinned_memory_type'.
  TRITONSERVER_MemoryType use_pinned_memory_type =
      TRITONSERVER_MEMORY_CPU_PINNED;
  if (pinned_enabled_) {
    use_pinned_memory_type = GetUsePinnedMemoryType(memory_type);
  }

  size_t tensor_offset = 0;
  const size_t tensor_byte_size = GetByteSize(datatype, shape);

  for (size_t idx = 0; idx < responses_->size(); idx++) {
    auto& request = requests_[idx];
    auto& response = (*responses_)[idx];

    // If then pending copies are from tensor buffer that is not
    // contiguous with 'response's part of that buffer, then need to
    // go ahead and perform the pending copies so that can start a
    // new contiguous region if necessary.
    if ((pending_pinned_byte_size_ > 0) &&
        (tensor_offset !=
         (pending_pinned_byte_size_ + pending_pinned_offset_))) {
      need_sync_ |= FlushPendingPinned(buffer, memory_type, memory_type_id);
    }

    InferenceResponse::Output* response_output = nullptr;
    if ((response != nullptr) &&
        (request->ImmutableRequestedOutputs().find(name) !=
         request->ImmutableRequestedOutputs().end())) {
      response->AddOutput(name, datatype, shape, &response_output);
      need_sync_ |= SetFixedSizeOutputBuffer(
          &response, response_output, tensor_byte_size, tensor_offset, buffer,
          memory_type, memory_type_id, use_pinned_memory_type);
    }

    tensor_offset += tensor_byte_size;
  }

  // Done with the tensor, flush any pending pinned copies.
  need_sync_ |= FlushPendingPinned(buffer, memory_type, memory_type_id);
}

bool
BackendResponder::Finalize()
{
#ifdef TRTIS_ENABLE_GPU
  if (need_sync_) {
    cudaStreamSynchronize(stream_);
    need_sync_ = false;
  }
#endif

  // After the above sync all the GPU->pinned copies are complete. Any
  // deferred copies of pinned->CPU can now be done.
  for (auto& def : deferred_pinned_) {
    auto pinned_memory_type = TRITONSERVER_MEMORY_CPU_PINNED;
    int64_t pinned_memory_id = 0;
    char* pinned_buffer = def.pinned_memory_->MutableBuffer(
        &pinned_memory_type, &pinned_memory_id);

    size_t offset = 0;
    for (auto& pr : def.responses_) {
      std::unique_ptr<InferenceResponse>* response = pr.first;
      InferenceResponse::Output* response_output = pr.second;

      const void* response_buffer;
      size_t response_byte_size;
      TRITONSERVER_MemoryType response_memory_type;
      int64_t response_memory_type_id;
      void* userp;

      Status status = response_output->DataBuffer(
          &response_buffer, &response_byte_size, &response_memory_type,
          &response_memory_type_id, &userp);
      if (!status.IsOk()) {
        LOG_STATUS_ERROR(
            InferenceResponse::SendWithStatus(std::move(*response), status),
            "error sending TensorFlow response");
      } else {
        bool cuda_used = false;
        status = CopyBuffer(
            response_output->Name(), pinned_memory_type, pinned_memory_id,
            response_memory_type, response_memory_type_id, response_byte_size,
            pinned_buffer + offset, const_cast<void*>(response_buffer), stream_,
            &cuda_used);
        need_sync_ |= cuda_used;

        if (!status.IsOk()) {
          LOG_STATUS_ERROR(
              InferenceResponse::SendWithStatus(std::move(*response), status),
              "error sending TensorFlow response");
        }
      }

      offset += response_byte_size;
    }
  }

  deferred_pinned_.clear();

  return need_sync_;
}

bool
BackendResponder::SetFixedSizeOutputBuffer(
    std::unique_ptr<InferenceResponse>* response,
    InferenceResponse::Output* response_output, const size_t tensor_byte_size,
    const size_t tensor_offset, const char* tensor_buffer,
    const TRITONSERVER_MemoryType tensor_memory_type,
    const int64_t tensor_memory_type_id,
    const TRITONSERVER_MemoryType use_pinned_memory_type)
{
  void* buffer = nullptr;
  bool cuda_copy = false;

  TRITONSERVER_MemoryType actual_memory_type = tensor_memory_type;
  int64_t actual_memory_type_id = tensor_memory_type_id;

  Status status = response_output->AllocateDataBuffer(
      &buffer, tensor_byte_size, &actual_memory_type, &actual_memory_type_id);
  if (!status.IsOk()) {
    LOG_STATUS_ERROR(
        InferenceResponse::SendWithStatus(std::move(*response), status),
        "error sending TensorFlow response");
    return cuda_copy;
  }

  // If the response buffer matches the memory type that should use an
  // intermediate pinned memory buffer for the transfer, then just
  // record the response as pending and increase the size required for
  // the intermediate pinned buffer.
  if ((use_pinned_memory_type != TRITONSERVER_MEMORY_CPU_PINNED) &&
      (actual_memory_type == use_pinned_memory_type)) {
    if (pending_pinned_byte_size_ == 0) {
      pending_pinned_offset_ = tensor_offset;
    }

    pending_pinned_byte_size_ += tensor_byte_size;
    pending_pinned_output_.push_back(std::make_pair(response, response_output));
  } else {
    // Direct copy without intermediate pinned memory.
    bool cuda_used = false;
    status = CopyBuffer(
        response_output->Name(), tensor_memory_type, tensor_memory_type_id,
        actual_memory_type, actual_memory_type_id, tensor_byte_size,
        tensor_buffer + tensor_offset, buffer, stream_, &cuda_used);
    cuda_copy |= cuda_used;

    if (!status.IsOk()) {
      LOG_STATUS_ERROR(
          InferenceResponse::SendWithStatus(std::move(*response), status),
          "error sending TensorFlow response");
      return cuda_copy;
    }
  }

  return cuda_copy;
}

bool
BackendResponder::FlushPendingPinned(
    const char* tensor_buffer, const TRITONSERVER_MemoryType tensor_memory_type,
    const int64_t tensor_memory_type_id)
{
  bool cuda_copy = false;

  // Will be copying from CPU->pinned->GPU or GPU->pinned->CPU

  // Always need a pinned buffer...
  auto pinned_memory_type = TRITONSERVER_MEMORY_CPU_PINNED;
  int64_t pinned_memory_id = 0;

  std::unique_ptr<AllocatedMemory> pinned_memory(new AllocatedMemory(
      pending_pinned_byte_size_, pinned_memory_type, pinned_memory_id));
  char* pinned_buffer =
      pinned_memory->MutableBuffer(&pinned_memory_type, &pinned_memory_id);

  // If the pinned buffer isn't actually pinned memory then just
  // perform a direct copy. In this case 'pinned_memory' is just
  // deallocated and not used.
  if (pinned_memory_type != TRITONSERVER_MEMORY_CPU_PINNED) {
    pinned_memory.reset();

    size_t offset = 0;
    for (auto& pr : pending_pinned_output_) {
      std::unique_ptr<InferenceResponse>* response = pr.first;
      InferenceResponse::Output* response_output = pr.second;

      const void* response_buffer;
      size_t response_byte_size;
      TRITONSERVER_MemoryType response_memory_type;
      int64_t response_memory_type_id;
      void* userp;

      Status status = response_output->DataBuffer(
          &response_buffer, &response_byte_size, &response_memory_type,
          &response_memory_type_id, &userp);
      if (!status.IsOk()) {
        LOG_STATUS_ERROR(
            InferenceResponse::SendWithStatus(std::move(*response), status),
            "error sending TensorFlow response");
      } else {
        bool cuda_used = false;
        status = CopyBuffer(
            response_output->Name(), tensor_memory_type, tensor_memory_type_id,
            response_memory_type, response_memory_type_id, response_byte_size,
            tensor_buffer + pending_pinned_offset_ + offset,
            const_cast<void*>(response_buffer), stream_, &cuda_used);
        cuda_copy |= cuda_used;

        if (!status.IsOk()) {
          LOG_STATUS_ERROR(
              InferenceResponse::SendWithStatus(std::move(*response), status),
              "error sending TensorFlow response");
        }
      }

      offset += response_byte_size;
    }
  }
  // We have a pinned buffer so do a single copy of a block of tensor
  // to the pinned buffer.
  else {  // pinned_memory_type == TRITONSERVER_MEMORY_CPU_PINNED
    bool cuda_used = false;
    Status status = CopyBuffer(
        "pinned buffer", tensor_memory_type, tensor_memory_type_id,
        pinned_memory_type, pinned_memory_id, pending_pinned_byte_size_,
        tensor_buffer + pending_pinned_offset_, pinned_buffer, stream_,
        &cuda_used);
    cuda_copy |= cuda_used;

    // If something goes wrong with the copy all the pending
    // responses fail...
    if (!status.IsOk()) {
      for (auto& pr : pending_pinned_output_) {
        LOG_STATUS_ERROR(
            InferenceResponse::SendWithStatus(std::move(*(pr.first)), status),
            "error sending TensorFlow response");
      }
    }

    // If the copy was not async (i.e. if tensor was in CPU so a
    // CPU->CPU-PINNED copy was performed above), then the pinned
    // buffer now holds the tensor contents and we can immediately
    // issue the copies from the pinned buffer to the
    // responses.
    //
    // Otherwise the GPU->CPU-PINNED async copies are in flight and we
    // simply remember the pinned buffer and the corresponding
    // response outputs so that we can do the pinned->CPU copies in
    // finalize after we have waited for all async copies to complete.
    if (!cuda_used) {
      size_t offset = 0;
      for (auto& pr : pending_pinned_output_) {
        std::unique_ptr<InferenceResponse>* response = pr.first;
        InferenceResponse::Output* response_output = pr.second;

        const void* response_buffer;
        size_t response_byte_size;
        TRITONSERVER_MemoryType response_memory_type;
        int64_t response_memory_type_id;
        void* userp;

        Status status = response_output->DataBuffer(
            &response_buffer, &response_byte_size, &response_memory_type,
            &response_memory_type_id, &userp);
        if (!status.IsOk()) {
          LOG_STATUS_ERROR(
              InferenceResponse::SendWithStatus(std::move(*response), status),
              "error sending TensorFlow response");
        } else {
          bool cuda_used = false;
          status = CopyBuffer(
              response_output->Name(), pinned_memory_type, pinned_memory_id,
              response_memory_type, response_memory_type_id, response_byte_size,
              pinned_buffer + offset, const_cast<void*>(response_buffer),
              stream_, &cuda_used);
          cuda_copy |= cuda_used;

          if (!status.IsOk()) {
            LOG_STATUS_ERROR(
                InferenceResponse::SendWithStatus(std::move(*response), status),
                "error sending TensorFlow response");
          }
        }

        offset += response_byte_size;
      }
    } else {
      deferred_pinned_.emplace_back(
          std::move(pinned_memory), std::move(pending_pinned_output_));
    }
  }

  // Pending pinned copies are flushed to clear...
  pending_pinned_byte_size_ = 0;
  pending_pinned_offset_ = 0;
  pending_pinned_output_.clear();

  // Need to hold on to the allocated pinned buffer as there are still
  // copies in flight. Will delete it in finalize.
  if (pinned_memory != nullptr) {
    pinned_memories_.push_back(std::move(pinned_memory));
  }

  return cuda_copy;
}

}}  // namespace nvidia::inferenceserver
