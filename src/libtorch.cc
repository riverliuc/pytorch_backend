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

#include <stdint.h>
#include <exception>
#include "libtorch_utils.h"
#include "triton/backend/backend_common.h"
#include "triton/backend/backend_input_collector.h"
#include "triton/backend/backend_memory.h"
#include "triton/backend/backend_model.h"
#include "triton/backend/backend_model_instance.h"
#include "triton/backend/backend_output_responder.h"
#include "triton/core/tritonbackend.h"

// Suppress warnings in torch headers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma warning(push, 0)
#include <torchvision/ops/ops.h>
#include <torchvision/vision.h>  // Torchvision header
#pragma warning(pop)
#pragma GCC diagnostic pop

#ifdef TRITON_ENABLE_GPU
#include <c10/cuda/CUDACachingAllocator.h>
#include <cuda_runtime_api.h>
#endif  // TRITON_ENABLE_GPU

//
// PyTorch C++ (LibTorch) Backend that implements the TRITONBACKEND API.
//

namespace triton { namespace backend { namespace pytorch {

//
// ModelState
//
// State associated with a model that is using this backend. An object
// of this class is created and associated with each
// TRITONBACKEND_Model.
//
class ModelState : public BackendModel {
  /* ModelState类继承了BackendModel类，基类中包含了诸多BackendModel的基本信息，包括：*/
  /* server的指针，mem_manager指针，模型的名称，model_repo的路径，以及model的config信息等等。 */
  /* 通常我们在ModelState中实现读取这种backend模型的函数。*/
 public:
  static TRITONSERVER_Error* Create(
      TRITONBACKEND_Model* triton_model, ModelState** state);
  virtual ~ModelState() = default;

  // Load a TorchScript model using 'artifact_name' as the name for the
  // TorchScript file. Return in 'model_path' the full path to the
  // TorchScript file, return in 'torch_model' the Torch Module
  // representing the model.
  TRITONSERVER_Error* LoadModel(
      const std::string& artifact_name, const torch::Device device,
      std::string* model_path,
      std::unique_ptr<torch::jit::script::Module>* torch_model);

 private:
  ModelState(TRITONBACKEND_Model* triton_model);
  TRITONSERVER_Error* AutoCompleteConfig();
};


TRITONSERVER_Error*
ModelState::Create(TRITONBACKEND_Model* triton_model, ModelState** state)
{
  try {
    /* 新建ModelState对象, 将利用二级指针返回 */
    *state = new ModelState(triton_model);
  }
  catch (const BackendModelException& ex) {
    RETURN_ERROR_IF_TRUE(
        ex.err_ == nullptr, TRITONSERVER_ERROR_INTERNAL,
        std::string("unexpected nullptr in BackendModelException"));
    RETURN_IF_ERROR(ex.err_);
  }

  // Auto-complete the configuration if requested...
  bool auto_complete_config = false;
  /* 查看当前的Model是否需需要自动补全config，PyTorch backend不支持自动补全。生命在tritonbackend.h中，定义在triton_model.cc中 */
  RETURN_IF_ERROR(TRITONBACKEND_ModelAutoCompleteConfig(
      triton_model, &auto_complete_config));
  /* 利用Triton提供的utility函数，自动补全模型config中的部分信息，包括max batch size, group instance等 */
  if (auto_complete_config) {
    RETURN_IF_ERROR((*state)->AutoCompleteConfig());

    triton::common::TritonJson::WriteBuffer json_buffer;
    /* 将补全后的config写成json */
    (*state)->ModelConfig().Write(&json_buffer);

    TRITONSERVER_Message* message;
    RETURN_IF_ERROR(TRITONSERVER_MessageNewFromSerializedJson(
        &message, json_buffer.Base(), json_buffer.Size()));
    RETURN_IF_ERROR(TRITONBACKEND_ModelSetConfig(
        triton_model, 1 /* config_version */, message));
  }

  return nullptr;  // success
}

ModelState::ModelState(TRITONBACKEND_Model* triton_model)
    : BackendModel(triton_model)
{
}

/* 读取PyTorch模型文件 */
TRITONSERVER_Error*
ModelState::LoadModel(
    const std::string& artifact_name, const torch::Device device,
    std::string* model_path,
    std::unique_ptr<torch::jit::script::Module>* torch_model)
{
  // Find the TorchScript file that describes the model. If the model
  // configuration doesn't have an explicit model file specified then
  // use the default name ("model.pt").
  /* 生成PyTorch模型文件路径，并检查模型是否存在 */
  std::string cc_model_filename = artifact_name;
  if (cc_model_filename.empty()) {
    cc_model_filename = "model.pt";
  }

  *model_path = JoinPath(
      {RepositoryPath(), std::to_string(Version()), cc_model_filename});

  {
    bool exists;
    RETURN_IF_ERROR(FileExists(*model_path, &exists));
    RETURN_ERROR_IF_FALSE(
        exists, TRITONSERVER_ERROR_UNAVAILABLE,
        std::string("unable to find '") + *model_path +
            "' for model instance '" + Name() + "'");
  }

  /* 开始读取模型文件 */
  // Serialize the torch model to string
  std::string model_data_str;
  RETURN_IF_ERROR(ReadTextFile(*model_path, &model_data_str));

  try {
    std::istringstream model_stream(model_data_str);
    /* 从string流读入模型并创建为Torch JIT模型对象, 通过unique指针返回 */
    torch_model->reset(
        new torch::jit::Module(torch::jit::load(model_stream, device)));
  }
  catch (const std::exception& ex) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("failed to load model '" + Name() + "': " + ex.what()).c_str());
  }

  return nullptr;  // success
}

TRITONSERVER_Error*
ModelState::AutoCompleteConfig()
{
  // Auto-complete configuration is not supported since PyTorch does not
  // store/capture sufficient model metadata so just log error instead.
  LOG_MESSAGE(
      TRITONSERVER_LOG_WARN,
      (std::string("skipping model configuration auto-complete for '") +
       Name() + "': not supported for pytorch backend")
          .c_str());

  return nullptr;  // success
}


//
// ModelInstanceState
//
// State associated with a model instance. An object of this class is
// created and associated with each TRITONBACKEND_ModelInstance.
//
class ModelInstanceState : public BackendModelInstance {
  /* ModelInstanceState继承BackendModelInstance类，基类中包含了诸多与某个特定model instance相关的信息，包括：*/
  /* 指向instance对应BackendModel的指针，instance的类型(GPU/CPU)，运行设备的id，模型文件的路径等 */
  /* 这个类的对象将依附于TRITONBACKEND_ModelInstance对象，执行实际的输入数据准备、模型前行推理和输出数据收集和发送等步骤， */
  /* 是Backend在某个设备上的实际executor */
 public:
  static TRITONSERVER_Error* Create(
      ModelState* model_state,
      TRITONBACKEND_ModelInstance* triton_model_instance,
      ModelInstanceState** state);
  virtual ~ModelInstanceState();

  // Get the state of the model that corresponds to this instance.
  ModelState* StateForModel() const { return model_state_; }

  // Execute...
  void ProcessRequests(
      TRITONBACKEND_Request** requests, const uint32_t request_count);

 private:
  ModelInstanceState(
      ModelState* model_state,
      TRITONBACKEND_ModelInstance* triton_model_instance);
  TRITONSERVER_Error* ValidateBooleanSequenceControl(
      triton::common::TritonJson::Value& sequence_batching,
      const std::string& control_kind, bool required, bool* have_control);
  TRITONSERVER_Error* ValidateTypedSequenceControl(
      triton::common::TritonJson::Value& sequence_batching,
      const std::string& control_kind, bool required, bool* have_control);
  TRITONSERVER_Error* ValidateInputs();
  TRITONSERVER_Error* ValidateOutputs();
  void Execute(
      std::vector<TRITONBACKEND_Response*>* responses,
      const uint32_t response_count,
      std::vector<torch::jit::IValue>* input_tensors,
      std::vector<torch::Tensor>* output_tensors);
  void SetInputTensors(
      size_t total_batch_size, TRITONBACKEND_Request** requests,
      const uint32_t request_count,
      std::vector<TRITONBACKEND_Response*>* responses,
      BackendInputCollector* collector, std::vector<const char*>* input_names,
      std::vector<torch::jit::IValue>* input_tensors,
      std::vector<BackendMemory*>* input_memories, bool* cuda_copy);
  void ReadOutputTensors(
      size_t total_batch_size, const std::vector<const char*>& output_names,
      const std::vector<torch::Tensor>& output_tensors,
      TRITONBACKEND_Request** requests, const uint32_t request_count,
      std::vector<TRITONBACKEND_Response*>* responses);

  ModelState* model_state_;

  // The full path to the TorchScript model file.
  std::string model_path_;

  std::unique_ptr<torch::jit::script::Module> torch_model_;
  torch::Device device_;

  // Map from configuration name for an input to the index of
  // that input in the model.
  std::unordered_map<std::string, int> input_index_map_;

  // Map from configuration name for an output to the index of
  // that output in the model.
  std::unordered_map<std::string, int> output_index_map_;
  std::unordered_map<std::string, TRITONSERVER_DataType> output_dtype_map_;
};

TRITONSERVER_Error*
ModelInstanceState::Create(
    ModelState* model_state, TRITONBACKEND_ModelInstance* triton_model_instance,
    ModelInstanceState** state)
{
  try {
    *state = new ModelInstanceState(model_state, triton_model_instance);
  }
  catch (const BackendModelInstanceException& ex) {
    RETURN_ERROR_IF_TRUE(
        ex.err_ == nullptr, TRITONSERVER_ERROR_INTERNAL,
        std::string("unexpected nullptr in BackendModelInstanceException"));
    RETURN_IF_ERROR(ex.err_);
  }

  return nullptr;  // success
}

ModelInstanceState::ModelInstanceState(
    ModelState* model_state, TRITONBACKEND_ModelInstance* triton_model_instance)
    : BackendModelInstance(model_state, triton_model_instance),
      model_state_(model_state), device_(torch::kCPU)
{
  if (Kind() == TRITONSERVER_INSTANCEGROUPKIND_GPU) {
    device_ = torch::Device(torch::kCUDA, DeviceId());
  }

  /* 根据模型config携带的模型文件名，去读取PyTorch模型 */
  THROW_IF_BACKEND_INSTANCE_ERROR(model_state->LoadModel(
      ArtifactFilename(), device_, &model_path_, &torch_model_));
  
  /* 从模型config中获取输入的数量 */
  size_t expected_input_cnt = 0;
  {
    triton::common::TritonJson::Value inputs;
    if (model_state->ModelConfig().Find("input", &inputs)) {
      expected_input_cnt = inputs.ArraySize();
    }
  }

  // If this is a sequence model then make sure that the required
  // inputs are present in the model and have the correct shape and
  // datatype.
  triton::common::TritonJson::Value sequence_batching;
  if (model_state->ModelConfig().Find(
          "sequence_batching", &sequence_batching)) {
    bool have_start, have_end, have_ready, have_corrid;
    THROW_IF_BACKEND_INSTANCE_ERROR(ValidateBooleanSequenceControl(
        sequence_batching, "CONTROL_SEQUENCE_START", false /* required */,
        &have_start));
    THROW_IF_BACKEND_INSTANCE_ERROR(ValidateBooleanSequenceControl(
        sequence_batching, "CONTROL_SEQUENCE_END", false /* required */,
        &have_end));
    THROW_IF_BACKEND_INSTANCE_ERROR(ValidateBooleanSequenceControl(
        sequence_batching, "CONTROL_SEQUENCE_READY", false /* required */,
        &have_ready));
    THROW_IF_BACKEND_INSTANCE_ERROR(ValidateTypedSequenceControl(
        sequence_batching, "CONTROL_SEQUENCE_CORRID", false /* required */,
        &have_corrid));
    if (have_start) {
      expected_input_cnt += 1;
    }
    if (have_end) {
      expected_input_cnt += 1;
    }
    if (have_ready) {
      expected_input_cnt += 1;
    }
    if (have_corrid) {
      expected_input_cnt += 1;
    }
  }

  THROW_IF_BACKEND_INSTANCE_ERROR(ValidateInputs());
  THROW_IF_BACKEND_INSTANCE_ERROR(ValidateOutputs());
}

ModelInstanceState::~ModelInstanceState()
{
  torch_model_.reset();
#ifdef TRITON_ENABLE_GPU
  if (device_.is_cuda()) {
    c10::cuda::CUDACachingAllocator::emptyCache();
  }
#endif  // TRITON_ENABLE_GPU
}

TRITONSERVER_Error*
ModelInstanceState::ValidateBooleanSequenceControl(
    triton::common::TritonJson::Value& sequence_batching,
    const std::string& control_kind, bool required, bool* have_control)
{
  std::string tensor_name;
  std::string tensor_datatype;
  RETURN_IF_ERROR(GetBooleanSequenceControlProperties(
      sequence_batching, model_state_->Name(), control_kind, required,
      &tensor_name, &tensor_datatype, nullptr, nullptr, nullptr, nullptr));
  *have_control = !tensor_name.empty();
  if (*have_control) {
    std::string deliminator = "__";
    int ip_index = 0;
    try {
      int start_pos = tensor_name.find(deliminator);
      if (start_pos == -1) {
        throw std::invalid_argument("input must follow naming convention");
      }
      ip_index = std::atoi(tensor_name.substr(start_pos + 2).c_str());
      input_index_map_[tensor_name] = ip_index;
    }
    catch (std::exception& ex) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          ("input '" + tensor_name +
           "' does not follow naming convention i.e. <name>__<index>.")
              .c_str());
    }
  }

  return nullptr;  // success
}

TRITONSERVER_Error*
ModelInstanceState::ValidateTypedSequenceControl(
    triton::common::TritonJson::Value& sequence_batching,
    const std::string& control_kind, bool required, bool* have_control)
{
  std::string tensor_name;
  std::string tensor_datatype;
  RETURN_IF_ERROR(GetTypedSequenceControlProperties(
      sequence_batching, model_state_->Name(), control_kind, required,
      &tensor_name, &tensor_datatype));
  *have_control = !tensor_name.empty();
  if (*have_control) {
    std::string deliminator = "__";
    int ip_index = 0;
    try {
      int start_pos = tensor_name.find(deliminator);
      if (start_pos == -1) {
        throw std::invalid_argument("input must follow naming convention");
      }
      ip_index = std::atoi(tensor_name.substr(start_pos + 2).c_str());
      input_index_map_[tensor_name] = ip_index;
    }
    catch (std::exception& ex) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          ("input '" + tensor_name +
           "' does not follow naming convention i.e. <name>__<index>.")
              .c_str());
    }
  }

  return nullptr;  // success
}

TRITONSERVER_Error*
ModelInstanceState::ValidateInputs()
{
  triton::common::TritonJson::Value ios;
  RETURN_IF_ERROR(model_state_->ModelConfig().MemberAsArray("input", &ios));
  std::string deliminator = "__";
  int ip_index = 0;

  for (size_t i = 0; i < ios.ArraySize(); i++) {
    triton::common::TritonJson::Value io;
    RETURN_IF_ERROR(ios.IndexAsObject(i, &io));

    // Validate name
    std::string io_name;
    RETURN_IF_ERROR(io.MemberAsString("name", &io_name));
    try {
      int start_pos = io_name.find(deliminator);
      if (start_pos == -1) {
        throw std::invalid_argument("input must follow naming convention");
      }
      ip_index = std::atoi(io_name.substr(start_pos + 2).c_str());
      input_index_map_[io_name] = ip_index;
    }
    catch (std::exception& ex) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          ("input '" + io_name +
           "' does not follow naming convention i.e. <name>__<index>.")
              .c_str());
    }

    // Validate data type
    std::string io_dtype;
    RETURN_IF_ERROR(io.MemberAsString("data_type", &io_dtype));
    const auto pr = ModelConfigDataTypeToTorchType(io_dtype);
    if (!pr.first) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          ("unsupported datatype " + io_dtype + " for input '" + io_name +
           "' for model '" + model_state_->Name() + "'")
              .c_str());
    }
  }

  return nullptr;  // success
}

TRITONSERVER_Error*
ModelInstanceState::ValidateOutputs()
{
  triton::common::TritonJson::Value ios;
  RETURN_IF_ERROR(model_state_->ModelConfig().MemberAsArray("output", &ios));
  std::string deliminator = "__";
  int op_index = 0;

  for (size_t i = 0; i < ios.ArraySize(); i++) {
    triton::common::TritonJson::Value io;
    RETURN_IF_ERROR(ios.IndexAsObject(i, &io));

    // Validate name
    std::string io_name;
    RETURN_IF_ERROR(io.MemberAsString("name", &io_name));
    try {
      int start_pos = io_name.find(deliminator);
      if (start_pos == -1) {
        throw std::invalid_argument("output must follow naming convention");
      }
      op_index = std::atoi(io_name.substr(start_pos + 2).c_str());
    }
    catch (std::exception& ex) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          ("output '" + io_name +
           "' does not follow naming convention i.e. <name>__<index>.")
              .c_str());
    }

    // Validate data type
    std::string io_dtype;
    RETURN_IF_ERROR(io.MemberAsString("data_type", &io_dtype));
    const auto pr = ModelConfigDataTypeToTorchType(io_dtype);
    if (!pr.first) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          ("unsupported datatype " + io_dtype + " for output '" + io_name +
           "' for model '" + model_state_->Name() + "'")
              .c_str());
    }
    output_index_map_[io_name] = op_index;
    output_dtype_map_[io_name] = ConvertTorchTypeToDataType(pr.second);
  }

  return nullptr;  // success
}

void
ModelInstanceState::ProcessRequests(
    TRITONBACKEND_Request** requests, const uint32_t request_count)
{
  LOG_MESSAGE(
      TRITONSERVER_LOG_VERBOSE,
      (std::string("TRITONBACKEND_ModelExecute: Running ") + Name() + " with " +
       std::to_string(request_count) + " requests")
          .c_str());

  uint64_t exec_start_ns = 0;
  SET_TIMESTAMP(exec_start_ns);

  const int max_batch_size = model_state_->MaxBatchSize();

  // For each request collect the total batch size for this inference
  // execution. The batch-size, number of inputs, and size of each
  // input has already been checked so don't need to do that here.
  /* 以下收集送到backend的所有request的batch_size总和, 并检查是否超过max_batch_size */
  size_t total_batch_size = 0;
  for (size_t i = 0; i < request_count; i++) {
    // If we get a nullptr request then something is badly wrong. Fail
    // and release all requests.
    /* 检查送来的每个request是否为空，如果是空，则说明遇到严重错误，此时直接中断所有推理 */
    if (requests[i] == nullptr) {
      RequestsRespondWithError(
          requests, request_count,
          TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INTERNAL,
              std::string(
                  "null request given to PyTorch backend for '" + Name() + "'")
                  .c_str()));
      return;
    }

    if (max_batch_size > 0) {
      // Retrieve the batch size from one of the inputs, if the model
      // supports batching, the first dimension size is batch size
      /* 获取每个request中input的batch_size */
      TRITONBACKEND_Input* input;
      TRITONSERVER_Error* err =
          TRITONBACKEND_RequestInputByIndex(requests[i], 0 /* index */, &input);
      if (err == nullptr) {
        const int64_t* shape;
        err = TRITONBACKEND_InputProperties(
            input, nullptr, nullptr, &shape, nullptr, nullptr, nullptr);
        total_batch_size += shape[0];
      }
      if (err != nullptr) {
        RequestsRespondWithError(requests, request_count, err);
        return;
      }
    } else {
      total_batch_size += 1;
    }
  }

  // If there are no valid payloads then no need to run the inference.
  if (total_batch_size == 0) {
    return;
  }

  // Make sure the maximum batch size is not exceeded. The
  // total_batch_size must be 1 for models that don't support batching
  // (i.e. max_batch_size == 0). If max_batch_size is exceeded then
  // scheduler has done something badly wrong so fail and release all
  // requests.
  /* 检查所有request的batch_size之和是否小于max_batch_size 或 等于1 */
  if ((total_batch_size != 1) && (total_batch_size > (size_t)max_batch_size)) {
    RequestsRespondWithError(
        requests, request_count,
        TRITONSERVER_ErrorNew(
            TRITONSERVER_ERROR_INTERNAL,
            std::string(
                "batch size " + std::to_string(total_batch_size) + " for '" +
                Name() + "', max allowed is " + std::to_string(max_batch_size))
                .c_str()));
    return;
  }

  // At this point we are committed to running inference with all
  // 'requests'. Create a response for each request. During input
  // processing if there is an error with any request that error will
  // be sent immediately with the corresponding response (and the
  // response unique_ptr will then be nullptr). The request object
  // itself will not be released until after all inferencing is done
  // (below) as we may need to access the request object when
  // determine how to process outputs (for example, even if we don't
  // need the outputs for a request that has an error, we do need to
  // know the size of those outputs associated with the request so we
  // can skip them in the output tensors).
  /* 维护一个response列表，每个request对应列表中的一个response */
  std::vector<TRITONBACKEND_Response*> responses;
  responses.reserve(request_count);

  for (size_t i = 0; i < request_count; i++) {
    TRITONBACKEND_Response* response;
    /* 为每个request创建对应的response对象, 包括设置response的id为request的id, 设置response的回调函数等等 */
    auto err = TRITONBACKEND_ResponseNew(&response, requests[i]);
    if (err == nullptr) {
      responses.emplace_back(response);
    } else {
      responses.emplace_back(nullptr);
      LOG_MESSAGE(TRITONSERVER_LOG_ERROR, "Fail to create response");
      TRITONSERVER_ErrorDelete(err);
    }
  }

  std::vector<const char*> input_names;
  std::vector<torch::jit::IValue> input_tensors;
  std::vector<BackendMemory*> input_memories;
  bool cuda_copy = false;
  /* 创建工具类的对象collector, 用于准备输入Tensors的 */
  BackendInputCollector collector(
      requests, request_count, &responses, model_state_->TritonMemoryManager() /* 定义在ModelState的基类BackendModel中*/,
      model_state_->EnablePinnedInput(), CudaStream());
  /* 着手准备输入Tensors, 包括为每个input创建buffer(大小为所有request中该input tensor的size之和), */
  /* 将送来所有request中的input都聚合为大的batch，以及把request中的输入数据拷贝到input buffer中 */
  SetInputTensors(
      total_batch_size, requests, request_count, &responses, &collector,
      &input_names, &input_tensors, &input_memories, &cuda_copy);

  // Request to retrieve all model outputs. 'output_names' and
  // 'output_tensors' are parallel vectors and so must be kept in
  // sync.
  /* 获取每个输出tensor的名称 */
  std::vector<const char*> output_names;
  std::vector<torch::Tensor> output_tensors;
  {
    triton::common::TritonJson::Value ios;
    TRITONSERVER_Error* err =
        model_state_->ModelConfig().MemberAsArray("output", &ios);

    if (err == nullptr) {
      for (size_t i = 0; i < ios.ArraySize(); i++) {
        triton::common::TritonJson::Value io;
        err = ios.IndexAsObject(i, &io);
        if (err != nullptr) {
          break;
        }

        // Use names from ModelConfig by reference since the model
        // config will persist longer than this inference execution.
        const char* io_name;
        size_t io_name_len;
        err = io.MemberAsString("name", &io_name, &io_name_len);
        if (err != nullptr) {
          break;
        }

        output_names.emplace_back(io_name);
      }
    }

    if (err != nullptr) {
      SendErrorForResponses(&responses, request_count, err);
      output_names.clear();
    }
  }

  // Wait for any in-flight input tensor copies to complete.
  /* 等待所有输入tensor内容的拷贝过程结束 */
#ifdef TRITON_ENABLE_GPU
  if (cuda_copy) {
    cudaStreamSynchronize(CudaStream());
  }
#endif

  uint64_t compute_start_ns = 0;
  SET_TIMESTAMP(compute_start_ns);

  // Run...
  /* 执行真正的推理 */
  Execute(&responses, request_count, &input_tensors, &output_tensors);

  uint64_t compute_end_ns = 0;
  SET_TIMESTAMP(compute_end_ns);

  // Free BackendMemory used for inputs
  for (BackendMemory* mem : input_memories) {
    delete mem;
  }
  input_memories.clear();

  // Verify output indices are valid with number of outputs after execution
  /* 检查config定义的输出tensor的index是否在合理范围内(大于0小于实际输出的tensor数量) */
  bool invalid_index = false;
  int max_index = output_tensors.size() - 1;
  for (const auto& name : output_names) {
    int op_index = output_index_map_[name];
    if ((op_index < 0) || (op_index > max_index)) {
      SendErrorForResponses(
          &responses, request_count,
          TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INVALID_ARG,
              std::string(
                  "The output " + std::string(name) +
                  " in the model configuration refers to an output index which"
                  " doesn't exist. This model has " +
                  std::to_string(max_index + 1) + " outputs")
                  .c_str()));
      invalid_index = true;
      break;
    }
  }

  /* 将PyTorch模型运行结果输出Tensor导出到responses中 */
  /* 主要将batch的输出tensor中，属于各个request的部分取出来，放到其对应的response中 */
  if (!invalid_index) {
    ReadOutputTensors(
        total_batch_size, output_names, output_tensors, requests, request_count,
        &responses);
  }

  uint64_t exec_end_ns = 0;
  SET_TIMESTAMP(exec_end_ns);

  // Send all the responses that haven't already been sent because of
  // an earlier error. Note that the responses are not set to nullptr
  // here as we need that indication below to determine if the request
  // we successful or not.
  /* 发送response，把推理结果发送给Triton */
  for (auto& response : responses) {
    if (response != nullptr) {
      LOG_IF_ERROR(
          TRITONBACKEND_ResponseSend(
              response, TRITONSERVER_RESPONSE_COMPLETE_FINAL, nullptr),
          "failed to send PyTorch backend response");
    }
  }

  // Report statistics for each request.
  for (uint32_t r = 0; r < request_count; ++r) {
    auto& request = requests[r];
    /* 发送每个request的统计数据 */
    LOG_IF_ERROR(
        TRITONBACKEND_ModelInstanceReportStatistics(
            TritonModelInstance(), request,
            (responses[r] != nullptr) /* success */, exec_start_ns,
            compute_start_ns, compute_end_ns, exec_end_ns),
        "failed reporting request statistics");

    /* 释放每个request对象 */
    LOG_IF_ERROR(
        TRITONBACKEND_RequestRelease(request, TRITONSERVER_REQUEST_RELEASE_ALL),
        "failed releasing request");
  }

  // Report the entire batch statistics.
  LOG_IF_ERROR(
      TRITONBACKEND_ModelInstanceReportBatchStatistics(
          TritonModelInstance(), total_batch_size, exec_start_ns,
          compute_start_ns, compute_end_ns, exec_end_ns),
      "failed reporting batch request statistics");
}

void
ModelInstanceState::Execute(
    std::vector<TRITONBACKEND_Response*>* responses,
    const uint32_t response_count,
    std::vector<torch::jit::IValue>* input_tensors,
    std::vector<torch::Tensor>* output_tensors)
{
  torch::jit::IValue model_outputs_;

  try {
    torch::NoGradGuard no_grad;
    /* PyTorch执行推理 */
    model_outputs_ = torch_model_->forward(*input_tensors);
    if (model_outputs_.isTuple()) {
      /* 将模型输出tensor收集起来 */
      auto model_outputs_tuple = model_outputs_.toTuple();
      for (auto& m_op : model_outputs_tuple->elements()) {
        output_tensors->push_back(m_op.toTensor());
      }
    } else {
      auto model_output_tensor = model_outputs_.toTensor();
      output_tensors->push_back(model_output_tensor);
    }
  }
  catch (std::exception& ex) {
    /* 如果模型前向失败，则对每个response都发送失败response */
    SendErrorForResponses(
        responses, response_count,
        TRITONSERVER_ErrorNew(
            TRITONSERVER_ERROR_INTERNAL,
            ("PyTorch execute failure: " + std::string(ex.what())).c_str()));
  }
}

void
ModelInstanceState::SetInputTensors(
    size_t total_batch_size, TRITONBACKEND_Request** requests,
    const uint32_t request_count,
    std::vector<TRITONBACKEND_Response*>* responses,
    BackendInputCollector* collector, std::vector<const char*>* input_names,
    std::vector<torch::jit::IValue>* input_tensors,
    std::vector<BackendMemory*>* input_memories, bool* cuda_copy)
{
  const int max_batch_size = model_state_->MaxBatchSize();

  // All requests must have equally-sized input tensors so use any
  // request as the representative for the input tensors.
  uint32_t input_count;
  /* 首先用任意一个request来获取每个request中input tensor的数量 */
  RESPOND_ALL_AND_RETURN_IF_ERROR(
      responses, request_count,
      TRITONBACKEND_RequestInputCount(requests[0], &input_count));
  input_tensors->resize(input_count);
  /* 对每个input依次进行处理 */
  for (uint32_t input_idx = 0; input_idx < input_count; input_idx++) {
    TRITONBACKEND_Input* input;
    /* 获取request中的目标input对象 */
    RESPOND_ALL_AND_RETURN_IF_ERROR(
        responses, request_count,
        TRITONBACKEND_RequestInputByIndex(requests[0], input_idx, &input));

    const char* input_name;
    TRITONSERVER_DataType input_datatype;
    const int64_t* input_shape;
    uint32_t input_dims_count;
    /* 获取input的相关属性，包括shape, 类型等 */
    RESPOND_ALL_AND_RETURN_IF_ERROR(
        responses, request_count,
        TRITONBACKEND_InputProperties(
            input, &input_name, &input_datatype, &input_shape,
            &input_dims_count, nullptr, nullptr));

    input_names->emplace_back(input_name);

    // The shape for the entire input patch, [total_batch_size, ...]
    std::vector<int64_t> batchn_shape(
        input_shape, input_shape + input_dims_count);
    /* 把batch_size那一维设置成所有request总的batch_size */
    if (max_batch_size != 0) {
      batchn_shape[0] = total_batch_size;
    }

    // The input must be in contiguous CPU/GPU memory.
    const int64_t batchn_byte_size = GetByteSize(input_datatype, batchn_shape);

    std::vector<BackendMemory::AllocationType> alloc_perference;
    if (device_.is_cpu()) {
      alloc_perference = {BackendMemory::AllocationType::CPU};
    } else {
      alloc_perference = {BackendMemory::AllocationType::GPU_POOL,
                          BackendMemory::AllocationType::GPU};
    }

    /* 为input tensor在特定设备上分配内存。这里相当于把所有request中的目标input都聚合在一起进行内存分配 */
    BackendMemory* input_memory;
    RESPOND_ALL_AND_RETURN_IF_ERROR(
        responses, request_count,
        BackendMemory::Create(
            model_state_->TritonMemoryManager(), alloc_perference,
            device_.is_cpu() ? 0 : device_.index(), batchn_byte_size,
            &input_memory));
    input_memories->push_back(input_memory);

    /* 创建input buffer */
    TRITONSERVER_MemoryType memory_type = input_memory->MemoryType();
    int64_t memory_type_id = input_memory->MemoryTypeId();
    char* input_buffer = input_memory->MemoryPtr();

    /* 将所有request中的目标input聚合在一起，并将输入数据拷贝到刚才申请的input tensor buffer中 */
    collector->ProcessTensor(
        input_name, input_buffer, batchn_byte_size, memory_type,
        memory_type_id);

    // Create Torch tenor
    const auto torch_dtype = ConvertDataTypeToTorchType(input_datatype);
    torch::TensorOptions options{torch_dtype.second};
    auto updated_options = device_.is_cuda()
                               ? options.device(torch::kCUDA, device_.index())
                               : options.device(torch::kCPU);

    /* 从input_buffer中的输入数据创建PyTorch的输入tensors */
    torch::Tensor input_tensor =
        torch::from_blob(input_buffer, batchn_shape, updated_options);
    (*input_tensors)[input_index_map_[input_name]] = input_tensor;
  }

  // Finalize...
  *cuda_copy |= collector->Finalize();
}

void
ModelInstanceState::ReadOutputTensors(
    size_t total_batch_size, const std::vector<const char*>& output_names,
    const std::vector<torch::Tensor>& output_tensors,
    TRITONBACKEND_Request** requests, const uint32_t request_count,
    std::vector<TRITONBACKEND_Response*>* responses)
{
  BackendOutputResponder responder(
      requests, request_count, responses, model_state_->MaxBatchSize(),
      model_state_->TritonMemoryManager(), model_state_->EnablePinnedInput(),
      CudaStream());

  bool cuda_copy = false;
  std::vector<std::vector<char>> string_buffers;
  /* 依次处理每个输出 */
  for (size_t idx = 0; idx < output_names.size(); idx++) {
    std::string name = output_names[idx];
    int op_index = output_index_map_[name];
    torch::Tensor output_flat;

    /* 获取当前的目标output tensor，并转换为连续且flattened的内存块 */
    try {
      output_flat = output_tensors[op_index].contiguous().flatten();
    }
    catch (std::exception& ex) {
      RESPOND_ALL_AND_RETURN_IF_ERROR(
          responses, request_count,
          TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INTERNAL,
              (std::string("output tensor '") + name + "' is not found")
                  .c_str()));
    }

    // Verify output datatype matches datatype from model config
    TRITONSERVER_DataType output_dtype =
        ConvertTorchTypeToDataType(output_flat.scalar_type());
    TRITONSERVER_DataType config_datatype = output_dtype_map_[name];
    if (config_datatype != output_dtype) {
      RESPOND_ALL_AND_RETURN_IF_ERROR(
          responses, request_count,
          TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INVALID_ARG,
              (std::string("unexpected datatype TYPE_") +
               TRITONSERVER_DataTypeString(output_dtype) +
               " for inference output '" + name + "', expecting TYPE_" +
               TRITONSERVER_DataTypeString(config_datatype))
                  .c_str()));
    }

    /* 获取output buffer */
    const char* output_buffer =
        static_cast<const char*>(output_flat.data_ptr());

    //  Set output shape
    std::vector<int64_t> batchn_shape;
    auto shape = output_tensors[op_index].sizes();
    for (auto itr = shape.begin(); itr != shape.end(); itr++) {
      batchn_shape.push_back(*itr);
    }

    /* 对当前的output进行处理，从大output batch中提取相应输出数据生成对应request的response */
    responder.ProcessTensor(
        name, output_dtype, batchn_shape, output_buffer,
        (device_.type() == torch::kCPU) ? TRITONSERVER_MEMORY_CPU
                                        : TRITONSERVER_MEMORY_GPU,
        (device_.type() == torch::kCPU) ? 0 : device_.index());
  }

  // Finalize and wait for any pending buffer copies.
  cuda_copy |= responder.Finalize();

#ifdef TRITON_ENABLE_GPU
  if (cuda_copy) {
    cudaStreamSynchronize(stream_);
  }
#endif  // TRITON_ENABLE_GPU
}

/////////////

extern "C" {

TRITONSERVER_Error*
TRITONBACKEND_Initialize(TRITONBACKEND_Backend* backend)
{
  const char* cname;
  RETURN_IF_ERROR(TRITONBACKEND_BackendName(backend, &cname));
  std::string name(cname);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_Initialize: ") + name).c_str());

  // Check the backend API version that Triton supports vs. what this
  // backend was compiled against.
  uint32_t api_version_major, api_version_minor;
  RETURN_IF_ERROR(
      TRITONBACKEND_ApiVersion(&api_version_major, &api_version_minor));

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("Triton TRITONBACKEND API version: ") +
       std::to_string(api_version_major) + "." +
       std::to_string(api_version_minor))
          .c_str());
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("'") + name + "' TRITONBACKEND API version: " +
       std::to_string(TRITONBACKEND_API_VERSION_MAJOR) + "." +
       std::to_string(TRITONBACKEND_API_VERSION_MINOR))
          .c_str());

  if ((api_version_major != TRITONBACKEND_API_VERSION_MAJOR) ||
      (api_version_minor < TRITONBACKEND_API_VERSION_MINOR)) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_UNSUPPORTED,
        (std::string("Triton TRITONBACKEND API version: ") +
         std::to_string(api_version_major) + "." +
         std::to_string(api_version_minor) + " does not support '" + name +
         "' TRITONBACKEND API version: " +
         std::to_string(TRITONBACKEND_API_VERSION_MAJOR) + "." +
         std::to_string(TRITONBACKEND_API_VERSION_MINOR))
            .c_str());
  }

  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelInitialize(TRITONBACKEND_Model* model)
{
  const char* cname;
  RETURN_IF_ERROR(TRITONBACKEND_ModelName(model, &cname));
  std::string name(cname);

  uint64_t version;
  RETURN_IF_ERROR(TRITONBACKEND_ModelVersion(model, &version));

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_ModelInitialize: ") + name + " (version " +
       std::to_string(version) + ")")
          .c_str());

  // Create a ModelState object and associate it with the
  // TRITONBACKEND_Model.
  /* 创建ModelState并依附于该Model对象，用于维护Model相关的重要信息，以及提供读取这种Backend模型的函数 */
  ModelState* model_state;
  RETURN_IF_ERROR(ModelState::Create(model, &model_state));
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelSetState(model, reinterpret_cast<void*>(model_state)));

  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelFinalize(TRITONBACKEND_Model* model)
{
  void* vstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelState(model, &vstate));
  ModelState* model_state = reinterpret_cast<ModelState*>(vstate);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO, "TRITONBACKEND_ModelFinalize: delete model state");

  delete model_state;

  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceInitialize(TRITONBACKEND_ModelInstance* instance)
{
  const char* cname;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceName(instance, &cname));
  std::string name(cname);

  int32_t device_id;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceDeviceId(instance, &device_id));

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_ModelInstanceInitialize: ") + name +
       " (device " + std::to_string(device_id) + ")")
          .c_str());

  // Get the model state associated with this instance's model.
  TRITONBACKEND_Model* model;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceModel(instance, &model));

  void* vmodelstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelState(model, &vmodelstate));
  ModelState* model_state = reinterpret_cast<ModelState*>(vmodelstate);

  // Create a ModelInstanceState object and associate it with the
  // TRITONBACKEND_ModelInstance.
  ModelInstanceState* instance_state;
  /* 创建ModelInstanceState对象，用于真正承担模型推理的工作 */
  RETURN_IF_ERROR(
      ModelInstanceState::Create(model_state, instance, &instance_state));
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceSetState(
      instance, reinterpret_cast<void*>(instance_state)));

  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceFinalize(TRITONBACKEND_ModelInstance* instance)
{
  void* vstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceState(instance, &vstate));
  ModelInstanceState* instance_state =
      reinterpret_cast<ModelInstanceState*>(vstate);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      "TRITONBACKEND_ModelInstanceFinalize: delete instance state");

  delete instance_state;

  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceExecute(
    TRITONBACKEND_ModelInstance* instance, TRITONBACKEND_Request** requests,
    const uint32_t request_count)
{
  // Triton will not call this function simultaneously for the same
  // 'instance'. But since this backend could be used by multiple
  // instances from multiple models the implementation needs to handle
  // multiple calls to this function at the same time (with different
  // 'instance' objects). Suggested practice for this is to use only
  // function-local and model-instance-specific state (obtained from
  // 'instance'), which is what we do here.
  ModelInstanceState* instance_state;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceState(
      instance, reinterpret_cast<void**>(&instance_state)));
  ModelState* model_state = instance_state->StateForModel();

  // This backend specifies BLOCKING execution policy. That means that
  // we should not return from this function until execution is
  // complete. Triton will automatically release 'instance' on return
  // from this function so that it is again available to be used for
  // another call to TRITONBACKEND_ModelInstanceExecute.

  LOG_MESSAGE(
      TRITONSERVER_LOG_VERBOSE,
      (std::string("model ") + model_state->Name() + ", instance " +
       instance_state->Name() + ", executing " + std::to_string(request_count) +
       " requests")
          .c_str());

  // At this point we accept ownership of 'requests', which means that
  // even if something goes wrong we must still return success from
  // this function. If something does go wrong in processing a
  // particular request then we send an error response just for the
  // specific request.
  instance_state->ProcessRequests(requests, request_count);

  return nullptr;  // success
}

}  // extern "C"

}}}  // namespace triton::backend::pytorch
