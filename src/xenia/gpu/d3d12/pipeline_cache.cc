/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/pipeline_cache.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/xxhash/xxhash.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/string.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/ui/d3d12/d3d12_util.h"

DEFINE_bool(d3d12_dxbc_disasm, false,
            "Disassemble DXBC shaders after generation.", "D3D12");
DEFINE_bool(
    d3d12_dxbc_disasm_dxilconv, false,
    "Disassemble DXBC shaders after conversion to DXIL, if DXIL shaders are "
    "supported by the OS, and DirectX Shader Compiler DLLs available at "
    "https://github.com/microsoft/DirectXShaderCompiler/releases are present.",
    "D3D12");
DEFINE_int32(
    d3d12_pipeline_creation_threads, -1,
    "Number of threads used for graphics pipeline creation. -1 to calculate "
    "automatically (75% of logical CPU cores), a positive number to specify "
    "the number of threads explicitly (up to the number of logical CPU cores), "
    "0 to disable multithreaded pipeline creation.",
    "D3D12");
DEFINE_bool(d3d12_tessellation_wireframe, false,
            "Display tessellated surfaces as wireframe for debugging.",
            "D3D12");

namespace xe {
namespace gpu {
namespace d3d12 {

// Generated with `xb buildhlsl`.
#include "xenia/gpu/d3d12/shaders/dxbc/adaptive_quad_hs.h"
#include "xenia/gpu/d3d12/shaders/dxbc/adaptive_triangle_hs.h"
#include "xenia/gpu/d3d12/shaders/dxbc/continuous_quad_hs.h"
#include "xenia/gpu/d3d12/shaders/dxbc/continuous_triangle_hs.h"
#include "xenia/gpu/d3d12/shaders/dxbc/discrete_quad_hs.h"
#include "xenia/gpu/d3d12/shaders/dxbc/discrete_triangle_hs.h"
#include "xenia/gpu/d3d12/shaders/dxbc/primitive_point_list_gs.h"
#include "xenia/gpu/d3d12/shaders/dxbc/primitive_quad_list_gs.h"
#include "xenia/gpu/d3d12/shaders/dxbc/primitive_rectangle_list_gs.h"
#include "xenia/gpu/d3d12/shaders/dxbc/tessellation_vs.h"

PipelineCache::PipelineCache(D3D12CommandProcessor& command_processor,
                             const RegisterFile& register_file,
                             bool bindless_resources_used, bool edram_rov_used,
                             uint32_t resolution_scale)
    : command_processor_(command_processor),
      register_file_(register_file),
      bindless_resources_used_(bindless_resources_used),
      edram_rov_used_(edram_rov_used),
      resolution_scale_(resolution_scale) {
  auto& provider = command_processor_.GetD3D12Context().GetD3D12Provider();

  shader_translator_ = std::make_unique<DxbcShaderTranslator>(
      provider.GetAdapterVendorID(), bindless_resources_used_, edram_rov_used_,
      provider.GetGraphicsAnalysis() != nullptr);

  if (edram_rov_used_) {
    depth_only_pixel_shader_ =
        std::move(shader_translator_->CreateDepthOnlyPixelShader());
  }
}

PipelineCache::~PipelineCache() { Shutdown(); }

bool PipelineCache::Initialize() {
  auto& provider = command_processor_.GetD3D12Context().GetD3D12Provider();

  // Initialize the command processor thread DXIL objects.
  dxbc_converter_ = nullptr;
  dxc_utils_ = nullptr;
  dxc_compiler_ = nullptr;
  if (cvars::d3d12_dxbc_disasm_dxilconv) {
    if (FAILED(provider.DxbcConverterCreateInstance(
            CLSID_DxbcConverter, IID_PPV_ARGS(&dxbc_converter_)))) {
      XELOGE(
          "Failed to create DxbcConverter, converted DXIL disassembly for "
          "debugging will be unavailable");
    }
    if (FAILED(provider.DxcCreateInstance(CLSID_DxcUtils,
                                          IID_PPV_ARGS(&dxc_utils_)))) {
      XELOGE(
          "Failed to create DxcUtils, converted DXIL disassembly for debugging "
          "will be unavailable");
    }
    if (FAILED(provider.DxcCreateInstance(CLSID_DxcCompiler,
                                          IID_PPV_ARGS(&dxc_compiler_)))) {
      XELOGE(
          "Failed to create DxcCompiler, converted DXIL disassembly for "
          "debugging will be unavailable");
    }
  }

  uint32_t logical_processor_count = xe::threading::logical_processor_count();
  if (!logical_processor_count) {
    // Pick some reasonable amount if couldn't determine the number of cores.
    logical_processor_count = 6;
  }
  // Initialize creation thread synchronization data even if not using creation
  // threads because they may be used anyway to create pipelines from the
  // storage.
  creation_threads_busy_ = 0;
  creation_completion_event_ =
      xe::threading::Event::CreateManualResetEvent(true);
  creation_completion_set_event_ = false;
  creation_threads_shutdown_from_ = SIZE_MAX;
  if (cvars::d3d12_pipeline_creation_threads != 0) {
    size_t creation_thread_count;
    if (cvars::d3d12_pipeline_creation_threads < 0) {
      creation_thread_count =
          std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      creation_thread_count =
          std::min(uint32_t(cvars::d3d12_pipeline_creation_threads),
                   logical_processor_count);
    }
    for (size_t i = 0; i < creation_thread_count; ++i) {
      std::unique_ptr<xe::threading::Thread> creation_thread =
          xe::threading::Thread::Create({}, [this, i]() { CreationThread(i); });
      creation_thread->set_name("D3D12 Pipelines");
      creation_threads_.push_back(std::move(creation_thread));
    }
  }
  return true;
}

void PipelineCache::Shutdown() {
  ClearCache(true);

  // Shut down all threads.
  if (!creation_threads_.empty()) {
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_threads_shutdown_from_ = 0;
    }
    creation_request_cond_.notify_all();
    for (size_t i = 0; i < creation_threads_.size(); ++i) {
      xe::threading::Wait(creation_threads_[i].get(), false);
    }
    creation_threads_.clear();
  }
  creation_completion_event_.reset();

  ui::d3d12::util::ReleaseAndNull(dxc_compiler_);
  ui::d3d12::util::ReleaseAndNull(dxc_utils_);
  ui::d3d12::util::ReleaseAndNull(dxbc_converter_);
}

void PipelineCache::ClearCache(bool shutting_down) {
  bool reinitialize_shader_storage =
      !shutting_down && storage_write_thread_ != nullptr;
  std::filesystem::path shader_storage_root;
  uint32_t shader_storage_title_id = shader_storage_title_id_;
  if (reinitialize_shader_storage) {
    shader_storage_root = shader_storage_root_;
  }
  ShutdownShaderStorage();

  // Remove references to the current pipeline.
  current_pipeline_ = nullptr;

  if (!creation_threads_.empty()) {
    // Empty the pipeline creation queue and make sure there are no threads
    // currently creating pipelines because pipelines are going to be deleted.
    bool await_creation_completion_event = false;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_queue_.clear();
      await_creation_completion_event = creation_threads_busy_ != 0;
      if (await_creation_completion_event) {
        creation_completion_event_->Reset();
        creation_completion_set_event_ = true;
      }
    }
    if (await_creation_completion_event) {
      creation_request_cond_.notify_one();
      xe::threading::Wait(creation_completion_event_.get(), false);
    }
  }

  // Destroy all pipelines.
  for (auto it : pipelines_) {
    it.second->state->Release();
    delete it.second;
  }
  pipelines_.clear();
  COUNT_profile_set("gpu/pipeline_cache/pipelines", 0);

  // Destroy all shaders.
  command_processor_.NotifyShaderBindingsLayoutUIDsInvalidated();
  if (bindless_resources_used_) {
    bindless_sampler_layout_map_.clear();
    bindless_sampler_layouts_.clear();
  }
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();
  for (auto it : shaders_) {
    delete it.second;
  }
  shaders_.clear();

  if (reinitialize_shader_storage) {
    InitializeShaderStorage(shader_storage_root, shader_storage_title_id,
                            false);
  }
}

void PipelineCache::InitializeShaderStorage(
    const std::filesystem::path& storage_root, uint32_t title_id,
    bool blocking) {
  ShutdownShaderStorage();

  auto shader_storage_root = storage_root / "shaders";
  // For files that can be moved between different hosts.
  // Host PSO blobs - if ever added - should be stored in shaders/local/ (they
  // currently aren't used because because they may be not very practical -
  // would need to invalidate them every commit likely, and additional I/O
  // cost - though D3D's internal validation would possibly be enough to ensure
  // they are up to date).
  auto shader_storage_shareable_root = shader_storage_root / "shareable";
  if (!std::filesystem::exists(shader_storage_shareable_root)) {
    if (!std::filesystem::create_directories(shader_storage_shareable_root)) {
      XELOGE(
          "Failed to create the shareable shader storage directory, persistent "
          "shader storage will be disabled: {}",
          xe::path_to_utf8(shader_storage_shareable_root));
      return;
    }
  }

  size_t logical_processor_count = xe::threading::logical_processor_count();
  if (!logical_processor_count) {
    // Pick some reasonable amount if couldn't determine the number of cores.
    logical_processor_count = 6;
  }

  // Initialize the Xenos shader storage stream.
  uint64_t shader_storage_initialization_start =
      xe::Clock::QueryHostTickCount();
  auto shader_storage_file_path =
      shader_storage_shareable_root / fmt::format("{:08X}.xsh", title_id);
  shader_storage_file_ =
      xe::filesystem::OpenFile(shader_storage_file_path, "a+b");
  if (!shader_storage_file_) {
    XELOGE(
        "Failed to open the guest shader storage file for writing, persistent "
        "shader storage will be disabled: {}",
        xe::path_to_utf8(shader_storage_file_path));
    return;
  }
  shader_storage_file_flush_needed_ = false;
  struct {
    uint32_t magic;
    uint32_t version_swapped;
  } shader_storage_file_header;
  // 'XESH'.
  const uint32_t shader_storage_magic = 0x48534558;
  if (fread(&shader_storage_file_header, sizeof(shader_storage_file_header), 1,
            shader_storage_file_) &&
      shader_storage_file_header.magic == shader_storage_magic &&
      xe::byte_swap(shader_storage_file_header.version_swapped) ==
          ShaderStoredHeader::kVersion) {
    uint64_t shader_storage_valid_bytes = sizeof(shader_storage_file_header);
    // Load and translate shaders written by previous Xenia executions until the
    // end of the file or until a corrupted one is detected.
    ShaderStoredHeader shader_header;
    std::vector<uint32_t> ucode_dwords;
    ucode_dwords.reserve(0xFFFF);
    size_t shaders_translated = 0;

    // Threads overlapping file reading.
    std::mutex shaders_translation_thread_mutex;
    std::condition_variable shaders_translation_thread_cond;
    std::deque<std::pair<ShaderStoredHeader, D3D12Shader*>>
        shaders_to_translate;
    size_t shader_translation_threads_busy = 0;
    bool shader_translation_threads_shutdown = false;
    std::mutex shaders_failed_to_translate_mutex;
    std::vector<D3D12Shader*> shaders_failed_to_translate;
    auto shader_translation_thread_function = [&]() {
      auto& provider = command_processor_.GetD3D12Context().GetD3D12Provider();
      DxbcShaderTranslator translator(
          provider.GetAdapterVendorID(), bindless_resources_used_,
          edram_rov_used_, provider.GetGraphicsAnalysis() != nullptr);
      // If needed and possible, create objects needed for DXIL conversion and
      // disassembly on this thread.
      IDxbcConverter* dxbc_converter = nullptr;
      IDxcUtils* dxc_utils = nullptr;
      IDxcCompiler* dxc_compiler = nullptr;
      if (cvars::d3d12_dxbc_disasm_dxilconv && dxbc_converter_ && dxc_utils_ &&
          dxc_compiler_) {
        provider.DxbcConverterCreateInstance(CLSID_DxbcConverter,
                                             IID_PPV_ARGS(&dxbc_converter));
        provider.DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxc_utils));
        provider.DxcCreateInstance(CLSID_DxcCompiler,
                                   IID_PPV_ARGS(&dxc_compiler));
      }
      for (;;) {
        std::pair<ShaderStoredHeader, D3D12Shader*> shader_to_translate;
        for (;;) {
          std::unique_lock<std::mutex> lock(shaders_translation_thread_mutex);
          if (shaders_to_translate.empty()) {
            if (shader_translation_threads_shutdown) {
              return;
            }
            shaders_translation_thread_cond.wait(lock);
            continue;
          }
          shader_to_translate = shaders_to_translate.front();
          shaders_to_translate.pop_front();
          ++shader_translation_threads_busy;
          break;
        }
        assert_not_null(shader_to_translate.second);
        if (!TranslateShader(
                translator, *shader_to_translate.second,
                shader_to_translate.first.sq_program_cntl, dxbc_converter,
                dxc_utils, dxc_compiler,
                shader_to_translate.first.host_vertex_shader_type)) {
          std::lock_guard<std::mutex> lock(shaders_failed_to_translate_mutex);
          shaders_failed_to_translate.push_back(shader_to_translate.second);
        }
        {
          std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
          --shader_translation_threads_busy;
        }
      }
      if (dxc_compiler) {
        dxc_compiler->Release();
      }
      if (dxc_utils) {
        dxc_utils->Release();
      }
      if (dxbc_converter) {
        dxbc_converter->Release();
      }
    };
    std::vector<std::unique_ptr<xe::threading::Thread>>
        shader_translation_threads;

    while (true) {
      if (!fread(&shader_header, sizeof(shader_header), 1,
                 shader_storage_file_)) {
        break;
      }
      size_t ucode_byte_count =
          shader_header.ucode_dword_count * sizeof(uint32_t);
      if (shaders_.find(shader_header.ucode_data_hash) != shaders_.end()) {
        // Already added - usually shaders aren't added without the intention of
        // translating them imminently, so don't do additional checks to
        // actually ensure that translation happens right now (they would cause
        // a race condition with shaders currently queued for translation).
        if (!xe::filesystem::Seek(shader_storage_file_,
                                  int64_t(ucode_byte_count), SEEK_CUR)) {
          break;
        }
        shader_storage_valid_bytes += sizeof(shader_header) + ucode_byte_count;
        continue;
      }
      ucode_dwords.resize(shader_header.ucode_dword_count);
      if (shader_header.ucode_dword_count &&
          !fread(ucode_dwords.data(), ucode_byte_count, 1,
                 shader_storage_file_)) {
        break;
      }
      uint64_t ucode_data_hash =
          XXH64(ucode_dwords.data(), ucode_byte_count, 0);
      if (shader_header.ucode_data_hash != ucode_data_hash) {
        // Validation failed.
        break;
      }
      D3D12Shader* shader =
          new D3D12Shader(shader_header.type, ucode_data_hash,
                          ucode_dwords.data(), shader_header.ucode_dword_count);
      shaders_.emplace(ucode_data_hash, shader);
      // Create new threads if the currently existing threads can't keep up with
      // file reading, but not more than the number of logical processors minus
      // one.
      size_t shader_translation_threads_needed;
      {
        std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
        shader_translation_threads_needed =
            std::min(shader_translation_threads_busy +
                         shaders_to_translate.size() + size_t(1),
                     logical_processor_count - size_t(1));
      }
      while (shader_translation_threads.size() <
             shader_translation_threads_needed) {
        shader_translation_threads.push_back(xe::threading::Thread::Create(
            {}, shader_translation_thread_function));
        shader_translation_threads.back()->set_name("Shader Translation");
      }
      {
        std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
        shaders_to_translate.emplace_back(shader_header, shader);
      }
      shaders_translation_thread_cond.notify_one();
      shader_storage_valid_bytes += sizeof(shader_header) + ucode_byte_count;
      ++shaders_translated;
    }
    if (!shader_translation_threads.empty()) {
      {
        std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
        shader_translation_threads_shutdown = true;
      }
      shaders_translation_thread_cond.notify_all();
      for (auto& shader_translation_thread : shader_translation_threads) {
        xe::threading::Wait(shader_translation_thread.get(), false);
      }
      shader_translation_threads.clear();
      for (D3D12Shader* shader : shaders_failed_to_translate) {
        shaders_.erase(shader->ucode_data_hash());
        delete shader;
      }
    }
    XELOGGPU("Translated {} shaders from the storage in {} milliseconds",
             shaders_translated,
             (xe::Clock::QueryHostTickCount() -
              shader_storage_initialization_start) *
                 1000 / xe::Clock::QueryHostTickFrequency());
    xe::filesystem::TruncateStdioFile(shader_storage_file_,
                                      shader_storage_valid_bytes);
  } else {
    xe::filesystem::TruncateStdioFile(shader_storage_file_, 0);
    shader_storage_file_header.magic = shader_storage_magic;
    shader_storage_file_header.version_swapped =
        xe::byte_swap(ShaderStoredHeader::kVersion);
    fwrite(&shader_storage_file_header, sizeof(shader_storage_file_header), 1,
           shader_storage_file_);
  }

  // 'DXRO' or 'DXRT'.
  const uint32_t pipeline_storage_magic_api =
      edram_rov_used_ ? 0x4F525844 : 0x54525844;

  // Initialize the pipeline storage stream.
  uint64_t pipeline_storage_initialization_start_ =
      xe::Clock::QueryHostTickCount();
  auto pipeline_storage_file_path =
      shader_storage_shareable_root /
      fmt::format("{:08X}.{}.d3d12.xpso", title_id,
                  edram_rov_used_ ? "rov" : "rtv");
  pipeline_storage_file_ =
      xe::filesystem::OpenFile(pipeline_storage_file_path, "a+b");
  if (!pipeline_storage_file_) {
    XELOGE(
        "Failed to open the Direct3D 12 pipeline description storage file for "
        "writing, persistent shader storage will be disabled: {}",
        xe::path_to_utf8(pipeline_storage_file_path));
    fclose(shader_storage_file_);
    shader_storage_file_ = nullptr;
    return;
  }
  pipeline_storage_file_flush_needed_ = false;
  // 'XEPS'.
  const uint32_t pipeline_storage_magic = 0x53504558;
  struct {
    uint32_t magic;
    uint32_t magic_api;
    uint32_t version_swapped;
  } pipeline_storage_file_header;
  if (fread(&pipeline_storage_file_header, sizeof(pipeline_storage_file_header),
            1, pipeline_storage_file_) &&
      pipeline_storage_file_header.magic == pipeline_storage_magic &&
      pipeline_storage_file_header.magic_api == pipeline_storage_magic_api &&
      xe::byte_swap(pipeline_storage_file_header.version_swapped) ==
          PipelineDescription::kVersion) {
    uint64_t pipeline_storage_valid_bytes =
        sizeof(pipeline_storage_file_header);
    // Enqueue pipeline descriptions written by previous Xenia executions until
    // the end of the file or until a corrupted one is detected.
    xe::filesystem::Seek(pipeline_storage_file_, 0, SEEK_END);
    int64_t pipeline_storage_told_end =
        xe::filesystem::Tell(pipeline_storage_file_);
    size_t pipeline_storage_told_count = size_t(
        pipeline_storage_told_end >= int64_t(pipeline_storage_valid_bytes)
            ? (uint64_t(pipeline_storage_told_end) -
               pipeline_storage_valid_bytes) /
                  sizeof(PipelineStoredDescription)
            : 0);
    if (pipeline_storage_told_count &&
        xe::filesystem::Seek(pipeline_storage_file_,
                             int64_t(pipeline_storage_valid_bytes), SEEK_SET)) {
      std::vector<PipelineStoredDescription> pipeline_stored_descriptions;
      pipeline_stored_descriptions.resize(pipeline_storage_told_count);
      pipeline_stored_descriptions.resize(
          fread(pipeline_stored_descriptions.data(),
                sizeof(PipelineStoredDescription), pipeline_storage_told_count,
                pipeline_storage_file_));
      if (!pipeline_stored_descriptions.empty()) {
        // Launch additional creation threads to use all cores to create
        // pipelines faster. Will also be using the main thread, so minus 1.
        size_t creation_thread_original_count = creation_threads_.size();
        size_t creation_thread_needed_count =
            std::max(std::min(pipeline_stored_descriptions.size(),
                              logical_processor_count) -
                         size_t(1),
                     creation_thread_original_count);
        while (creation_threads_.size() < creation_thread_original_count) {
          size_t creation_thread_index = creation_threads_.size();
          std::unique_ptr<xe::threading::Thread> creation_thread =
              xe::threading::Thread::Create(
                  {}, [this, creation_thread_index]() {
                    CreationThread(creation_thread_index);
                  });
          creation_thread->set_name("D3D12 Pipelines");
          creation_threads_.push_back(std::move(creation_thread));
        }
        size_t pipelines_created = 0;
        for (const PipelineStoredDescription& pipeline_stored_description :
             pipeline_stored_descriptions) {
          const PipelineDescription& pipeline_description =
              pipeline_stored_description.description;
          // Validate file integrity, stop and truncate the stream if data is
          // corrupted.
          if (XXH64(&pipeline_stored_description.description,
                    sizeof(pipeline_stored_description.description),
                    0) != pipeline_stored_description.description_hash) {
            break;
          }
          pipeline_storage_valid_bytes += sizeof(PipelineStoredDescription);
          // Skip already known pipelines - those have already been enqueued.
          auto found_range = pipelines_.equal_range(
              pipeline_stored_description.description_hash);
          bool pipeline_found = false;
          for (auto it = found_range.first; it != found_range.second; ++it) {
            Pipeline* found_pipeline = it->second;
            if (!std::memcmp(&found_pipeline->description.description,
                             &pipeline_description,
                             sizeof(pipeline_description))) {
              pipeline_found = true;
              break;
            }
          }
          if (pipeline_found) {
            continue;
          }

          PipelineRuntimeDescription pipeline_runtime_description;
          auto vertex_shader_it =
              shaders_.find(pipeline_description.vertex_shader_hash);
          if (vertex_shader_it == shaders_.end()) {
            continue;
          }
          pipeline_runtime_description.vertex_shader = vertex_shader_it->second;
          if (!pipeline_runtime_description.vertex_shader->is_valid()) {
            continue;
          }
          if (pipeline_description.pixel_shader_hash) {
            auto pixel_shader_it =
                shaders_.find(pipeline_description.pixel_shader_hash);
            if (pixel_shader_it == shaders_.end()) {
              continue;
            }
            pipeline_runtime_description.pixel_shader = pixel_shader_it->second;
            if (!pipeline_runtime_description.pixel_shader->is_valid()) {
              continue;
            }
          } else {
            pipeline_runtime_description.pixel_shader = nullptr;
          }
          pipeline_runtime_description.root_signature =
              command_processor_.GetRootSignature(
                  pipeline_runtime_description.vertex_shader,
                  pipeline_runtime_description.pixel_shader);
          if (!pipeline_runtime_description.root_signature) {
            continue;
          }
          std::memcpy(&pipeline_runtime_description.description,
                      &pipeline_description, sizeof(pipeline_description));

          Pipeline* new_pipeline = new Pipeline;
          new_pipeline->state = nullptr;
          std::memcpy(&new_pipeline->description, &pipeline_runtime_description,
                      sizeof(pipeline_runtime_description));
          pipelines_.emplace(pipeline_stored_description.description_hash,
                             new_pipeline);
          COUNT_profile_set("gpu/pipeline_cache/pipelines", pipelines_.size());
          if (!creation_threads_.empty()) {
            // Submit the pipeline for creation to any available thread.
            {
              std::lock_guard<std::mutex> lock(creation_request_lock_);
              creation_queue_.push_back(new_pipeline);
            }
            creation_request_cond_.notify_one();
          } else {
            new_pipeline->state =
                CreateD3D12Pipeline(pipeline_runtime_description);
          }
          ++pipelines_created;
        }
        CreateQueuedPipelinesOnProcessorThread();
        if (creation_threads_.size() > creation_thread_original_count) {
          {
            std::lock_guard<std::mutex> lock(creation_request_lock_);
            creation_threads_shutdown_from_ = creation_thread_original_count;
            // Assuming the queue is empty because of
            // CreateQueuedPipelinesOnProcessorThread.
          }
          creation_request_cond_.notify_all();
          while (creation_threads_.size() > creation_thread_original_count) {
            xe::threading::Wait(creation_threads_.back().get(), false);
            creation_threads_.pop_back();
          }
          bool await_creation_completion_event;
          {
            // Cleanup so additional threads can be created later again.
            std::lock_guard<std::mutex> lock(creation_request_lock_);
            creation_threads_shutdown_from_ = SIZE_MAX;
            // If the invocation is blocking, all the shader storage
            // initialization is expected to be done before proceeding, to avoid
            // latency in the command processor after the invocation.
            await_creation_completion_event =
                blocking && creation_threads_busy_ != 0;
            if (await_creation_completion_event) {
              creation_completion_event_->Reset();
              creation_completion_set_event_ = true;
            }
          }
          if (await_creation_completion_event) {
            creation_request_cond_.notify_one();
            xe::threading::Wait(creation_completion_event_.get(), false);
          }
        }
        XELOGGPU(
            "Created {} graphics pipelines from the storage in {} milliseconds",
            pipelines_created,
            (xe::Clock::QueryHostTickCount() -
             pipeline_storage_initialization_start_) *
                1000 / xe::Clock::QueryHostTickFrequency());
      }
    }
    xe::filesystem::TruncateStdioFile(pipeline_storage_file_,
                                      pipeline_storage_valid_bytes);
  } else {
    xe::filesystem::TruncateStdioFile(pipeline_storage_file_, 0);
    pipeline_storage_file_header.magic = pipeline_storage_magic;
    pipeline_storage_file_header.magic_api = pipeline_storage_magic_api;
    pipeline_storage_file_header.version_swapped =
        xe::byte_swap(PipelineDescription::kVersion);
    fwrite(&pipeline_storage_file_header, sizeof(pipeline_storage_file_header),
           1, pipeline_storage_file_);
  }

  shader_storage_root_ = storage_root;
  shader_storage_title_id_ = title_id;

  // Start the storage writing thread.
  storage_write_flush_shaders_ = false;
  storage_write_flush_pipelines_ = false;
  storage_write_thread_shutdown_ = false;
  storage_write_thread_ =
      xe::threading::Thread::Create({}, [this]() { StorageWriteThread(); });
}

void PipelineCache::ShutdownShaderStorage() {
  if (storage_write_thread_) {
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_thread_shutdown_ = true;
    }
    storage_write_request_cond_.notify_all();
    xe::threading::Wait(storage_write_thread_.get(), false);
    storage_write_thread_.reset();
  }
  storage_write_shader_queue_.clear();
  storage_write_pipeline_queue_.clear();

  if (pipeline_storage_file_) {
    fclose(pipeline_storage_file_);
    pipeline_storage_file_ = nullptr;
    pipeline_storage_file_flush_needed_ = false;
  }

  if (shader_storage_file_) {
    fclose(shader_storage_file_);
    shader_storage_file_ = nullptr;
    shader_storage_file_flush_needed_ = false;
  }

  shader_storage_root_.clear();
  shader_storage_title_id_ = 0;
}

void PipelineCache::EndSubmission() {
  if (shader_storage_file_flush_needed_ ||
      pipeline_storage_file_flush_needed_) {
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      if (shader_storage_file_flush_needed_) {
        storage_write_flush_shaders_ = true;
      }
      if (pipeline_storage_file_flush_needed_) {
        storage_write_flush_pipelines_ = true;
      }
    }
    storage_write_request_cond_.notify_one();
    shader_storage_file_flush_needed_ = false;
    pipeline_storage_file_flush_needed_ = false;
  }
  if (!creation_threads_.empty()) {
    CreateQueuedPipelinesOnProcessorThread();
    // Await creation of all queued pipelines.
    bool await_creation_completion_event;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      // Assuming the creation queue is already empty (because the processor
      // thread also worked on creating the leftover pipelines), so only check
      // if there are threads with pipelines currently being created.
      await_creation_completion_event = creation_threads_busy_ != 0;
      if (await_creation_completion_event) {
        creation_completion_event_->Reset();
        creation_completion_set_event_ = true;
      }
    }
    if (await_creation_completion_event) {
      creation_request_cond_.notify_one();
      xe::threading::Wait(creation_completion_event_.get(), false);
    }
  }
}

bool PipelineCache::IsCreatingPipelines() {
  if (creation_threads_.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(creation_request_lock_);
  return !creation_queue_.empty() || creation_threads_busy_ != 0;
}

D3D12Shader* PipelineCache::LoadShader(xenos::ShaderType shader_type,
                                       uint32_t guest_address,
                                       const uint32_t* host_address,
                                       uint32_t dword_count) {
  // Hash the input memory and lookup the shader.
  uint64_t data_hash = XXH64(host_address, dword_count * sizeof(uint32_t), 0);
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    // Shader has been previously loaded.
    return it->second;
  }

  // Always create the shader and stash it away.
  // We need to track it even if it fails translation so we know not to try
  // again.
  D3D12Shader* shader =
      new D3D12Shader(shader_type, data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);

  return shader;
}

Shader::HostVertexShaderType PipelineCache::GetHostVertexShaderTypeIfValid()
    const {
  // If the values this functions returns are changed, INVALIDATE THE SHADER
  // STORAGE (increase kVersion for BOTH shaders and pipelines)! The exception
  // is when the function originally returned "unsupported", but started to
  // return a valid value (in this case the shader wouldn't be cached in the
  // first place). Otherwise games will not be able to locate shaders for draws
  // for which the host vertex shader type has changed!
  const auto& regs = register_file_;
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (!xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode,
                                  vgt_draw_initiator.prim_type)) {
    // VGT_OUTPUT_PATH_CNTL and HOS registers are ignored in implicit major
    // mode.
    return Shader::HostVertexShaderType::kVertex;
  }
  if (regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select !=
      xenos::VGTOutputPath::kTessellationEnable) {
    return Shader::HostVertexShaderType::kVertex;
  }
  xenos::TessellationMode tessellation_mode =
      regs.Get<reg::VGT_HOS_CNTL>().tess_mode;
  switch (vgt_draw_initiator.prim_type) {
    case xenos::PrimitiveType::kTriangleList:
      // Also supported by triangle strips and fans according to:
      // https://www.khronos.org/registry/OpenGL/extensions/AMD/AMD_vertex_shader_tessellator.txt
      // Would need to convert those to triangle lists, but haven't seen any
      // games using tessellated strips/fans so far.
      switch (tessellation_mode) {
        case xenos::TessellationMode::kDiscrete:
          // - Call of Duty 3 - nets above barrels in the beginning of the
          //   first mission (turn right after the end of the intro) -
          //   kTriangleList.
        case xenos::TessellationMode::kContinuous:
          // - Viva Pinata - tree building with a beehive in the beginning
          //   (visible on the start screen behind the logo), waterfall in the
          //   beginning - kTriangleList.
          return Shader::HostVertexShaderType::kTriangleDomainCPIndexed;
        default:
          break;
      }
      break;
    case xenos::PrimitiveType::kQuadList:
      switch (tessellation_mode) {
        // Also supported by quad strips according to:
        // https://www.khronos.org/registry/OpenGL/extensions/AMD/AMD_vertex_shader_tessellator.txt
        // Would need to convert those to quad lists, but haven't seen any games
        // using tessellated strips so far.
        case xenos::TessellationMode::kDiscrete:
          // Not seen in games so far.
        case xenos::TessellationMode::kContinuous:
          // - Defender - retro screen and beams in the main menu - kQuadList.
          return Shader::HostVertexShaderType::kQuadDomainCPIndexed;
        default:
          break;
      }
      break;
    case xenos::PrimitiveType::kTrianglePatch:
      // - Banjo-Kazooie: Nuts & Bolts - water - adaptive.
      // - Halo 3 - water - adaptive.
      return Shader::HostVertexShaderType::kTriangleDomainPatchIndexed;
    case xenos::PrimitiveType::kQuadPatch:
      // - Fable II - continuous.
      // - Viva Pinata - garden ground - adaptive.
      return Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
    default:
      // TODO(Triang3l): Support line patches.
      break;
  }
  XELOGE(
      "Unsupported tessellation mode {} for primitive type {}. Report the game "
      "to Xenia developers!",
      uint32_t(tessellation_mode), uint32_t(vgt_draw_initiator.prim_type));
  return Shader::HostVertexShaderType(-1);
}

bool PipelineCache::EnsureShadersTranslated(
    D3D12Shader* vertex_shader, D3D12Shader* pixel_shader,
    Shader::HostVertexShaderType host_vertex_shader_type) {
  const auto& regs = register_file_;
  auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();

  // Edge flags are not supported yet (because polygon primitives are not).
  assert_true(sq_program_cntl.vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdge &&
              sq_program_cntl.vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdgeKill);
  assert_false(sq_program_cntl.gen_index_vtx);

  if (!vertex_shader->is_translated()) {
    if (!TranslateShader(*shader_translator_, *vertex_shader, sq_program_cntl,
                         dxbc_converter_, dxc_utils_, dxc_compiler_,
                         host_vertex_shader_type)) {
      XELOGE("Failed to translate the vertex shader!");
      return false;
    }
    if (shader_storage_file_) {
      assert_not_null(storage_write_thread_);
      shader_storage_file_flush_needed_ = true;
      {
        std::lock_guard<std::mutex> lock(storage_write_request_lock_);
        storage_write_shader_queue_.push_back(
            std::make_pair(vertex_shader, sq_program_cntl));
      }
      storage_write_request_cond_.notify_all();
    }
  }

  if (pixel_shader != nullptr && !pixel_shader->is_translated()) {
    if (!TranslateShader(*shader_translator_, *pixel_shader, sq_program_cntl,
                         dxbc_converter_, dxc_utils_, dxc_compiler_)) {
      XELOGE("Failed to translate the pixel shader!");
      return false;
    }
    if (shader_storage_file_) {
      assert_not_null(storage_write_thread_);
      shader_storage_file_flush_needed_ = true;
      {
        std::lock_guard<std::mutex> lock(storage_write_request_lock_);
        storage_write_shader_queue_.push_back(
            std::make_pair(pixel_shader, sq_program_cntl));
      }
      storage_write_request_cond_.notify_all();
    }
  }

  return true;
}

bool PipelineCache::ConfigurePipeline(
    D3D12Shader* vertex_shader, D3D12Shader* pixel_shader,
    xenos::PrimitiveType primitive_type, xenos::IndexFormat index_format,
    bool early_z,
    const RenderTargetCache::PipelineRenderTarget render_targets[5],
    void** pipeline_handle_out, ID3D12RootSignature** root_signature_out) {
#if XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES

  assert_not_null(pipeline_handle_out);
  assert_not_null(root_signature_out);

  PipelineRuntimeDescription runtime_description;
  if (!GetCurrentStateDescription(vertex_shader, pixel_shader, primitive_type,
                                  index_format, early_z, render_targets,
                                  runtime_description)) {
    return false;
  }
  PipelineDescription& description = runtime_description.description;

  if (current_pipeline_ != nullptr &&
      !std::memcmp(&current_pipeline_->description.description, &description,
                   sizeof(description))) {
    *pipeline_handle_out = current_pipeline_;
    *root_signature_out = runtime_description.root_signature;
    return true;
  }

  // Find an existing pipeline in the cache.
  uint64_t hash = XXH64(&description, sizeof(description), 0);
  auto found_range = pipelines_.equal_range(hash);
  for (auto it = found_range.first; it != found_range.second; ++it) {
    Pipeline* found_pipeline = it->second;
    if (!std::memcmp(&found_pipeline->description.description, &description,
                     sizeof(description))) {
      current_pipeline_ = found_pipeline;
      *pipeline_handle_out = found_pipeline;
      *root_signature_out = found_pipeline->description.root_signature;
      return true;
    }
  }

  if (!EnsureShadersTranslated(
          vertex_shader, pixel_shader,
          Shader::HostVertexShaderType(description.host_vertex_shader_type))) {
    return false;
  }

  Pipeline* new_pipeline = new Pipeline;
  new_pipeline->state = nullptr;
  std::memcpy(&new_pipeline->description, &runtime_description,
              sizeof(runtime_description));
  pipelines_.emplace(hash, new_pipeline);
  COUNT_profile_set("gpu/pipeline_cache/pipelines", pipelines_.size());

  if (!creation_threads_.empty()) {
    // Submit the pipeline for creation to any available thread.
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_queue_.push_back(new_pipeline);
    }
    creation_request_cond_.notify_one();
  } else {
    new_pipeline->state = CreateD3D12Pipeline(runtime_description);
  }

  if (pipeline_storage_file_) {
    assert_not_null(storage_write_thread_);
    pipeline_storage_file_flush_needed_ = true;
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_pipeline_queue_.emplace_back();
      PipelineStoredDescription& stored_description =
          storage_write_pipeline_queue_.back();
      stored_description.description_hash = hash;
      std::memcpy(&stored_description.description, &description,
                  sizeof(description));
    }
    storage_write_request_cond_.notify_all();
  }

  current_pipeline_ = new_pipeline;
  *pipeline_handle_out = new_pipeline;
  *root_signature_out = runtime_description.root_signature;
  return true;
}

bool PipelineCache::TranslateShader(
    DxbcShaderTranslator& translator, D3D12Shader& shader,
    reg::SQ_PROGRAM_CNTL cntl, IDxbcConverter* dxbc_converter,
    IDxcUtils* dxc_utils, IDxcCompiler* dxc_compiler,
    Shader::HostVertexShaderType host_vertex_shader_type) {
  // Perform translation.
  // If this fails the shader will be marked as invalid and ignored later.
  if (!translator.Translate(&shader, cntl, host_vertex_shader_type)) {
    XELOGE("Shader {:016X} translation failed; marking as ignored",
           shader.ucode_data_hash());
    return false;
  }

  const char* host_shader_type;
  if (shader.type() == xenos::ShaderType::kVertex) {
    switch (shader.host_vertex_shader_type()) {
      case Shader::HostVertexShaderType::kLineDomainCPIndexed:
        host_shader_type = "control-point-indexed line domain";
        break;
      case Shader::HostVertexShaderType::kLineDomainPatchIndexed:
        host_shader_type = "patch-indexed line domain";
        break;
      case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
        host_shader_type = "control-point-indexed triangle domain";
        break;
      case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
        host_shader_type = "patch-indexed triangle domain";
        break;
      case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
        host_shader_type = "control-point-indexed quad domain";
        break;
      case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
        host_shader_type = "patch-indexed quad domain";
        break;
      default:
        host_shader_type = "vertex";
    }
  } else {
    host_shader_type = "pixel";
  }
  XELOGGPU("Generated {} shader ({}b) - hash {:016X}:\n{}\n", host_shader_type,
           shader.ucode_dword_count() * 4, shader.ucode_data_hash(),
           shader.ucode_disassembly().c_str());

  // Set up texture and sampler bindings.
  uint32_t texture_binding_count;
  const DxbcShaderTranslator::TextureBinding* translator_texture_bindings =
      translator.GetTextureBindings(texture_binding_count);
  uint32_t sampler_binding_count;
  const DxbcShaderTranslator::SamplerBinding* sampler_bindings =
      translator.GetSamplerBindings(sampler_binding_count);
  shader.SetTexturesAndSamplers(translator_texture_bindings,
                                texture_binding_count, sampler_bindings,
                                sampler_binding_count);
  assert_false(bindless_resources_used_ &&
               texture_binding_count + sampler_binding_count >
                   D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 4);
  // Get hashable texture bindings, without translator-specific info.
  const D3D12Shader::TextureBinding* texture_bindings =
      shader.GetTextureBindings(texture_binding_count);
  size_t texture_binding_layout_bytes =
      texture_binding_count * sizeof(*texture_bindings);
  uint64_t texture_binding_layout_hash = 0;
  if (texture_binding_count) {
    texture_binding_layout_hash =
        XXH64(texture_bindings, texture_binding_layout_bytes, 0);
  }
  uint32_t bindless_sampler_count =
      bindless_resources_used_ ? sampler_binding_count : 0;
  uint64_t bindless_sampler_layout_hash = 0;
  if (bindless_sampler_count) {
    XXH64_state_t hash_state;
    XXH64_reset(&hash_state, 0);
    for (uint32_t i = 0; i < bindless_sampler_count; ++i) {
      XXH64_update(&hash_state, &sampler_bindings[i].bindless_descriptor_index,
                   sizeof(sampler_bindings[i].bindless_descriptor_index));
    }
    bindless_sampler_layout_hash = XXH64_digest(&hash_state);
  }
  // Obtain the unique IDs of binding layouts if there are any texture bindings
  // or bindless samplers, for invalidation in the command processor.
  size_t texture_binding_layout_uid = kLayoutUIDEmpty;
  // Use sampler count for the bindful case because it's the only thing that
  // must be the same for layouts to be compatible in this case
  // (instruction-specified parameters are used as overrides for actual
  // samplers).
  static_assert(
      kLayoutUIDEmpty == 0,
      "Empty layout UID is assumed to be 0 because for bindful samplers, the "
      "UID is their count");
  size_t sampler_binding_layout_uid = bindless_resources_used_
                                          ? kLayoutUIDEmpty
                                          : size_t(sampler_binding_count);
  if (texture_binding_count || bindless_sampler_count) {
    std::lock_guard<std::mutex> layouts_mutex_(layouts_mutex_);
    if (texture_binding_count) {
      auto found_range =
          texture_binding_layout_map_.equal_range(texture_binding_layout_hash);
      for (auto it = found_range.first; it != found_range.second; ++it) {
        if (it->second.vector_span_length == texture_binding_count &&
            !std::memcmp(
                texture_binding_layouts_.data() + it->second.vector_span_offset,
                texture_bindings, texture_binding_layout_bytes)) {
          texture_binding_layout_uid = it->second.uid;
          break;
        }
      }
      if (texture_binding_layout_uid == kLayoutUIDEmpty) {
        static_assert(
            kLayoutUIDEmpty == 0,
            "Layout UID is size + 1 because it's assumed that 0 is the UID for "
            "an empty layout");
        texture_binding_layout_uid = texture_binding_layout_map_.size() + 1;
        LayoutUID new_uid;
        new_uid.uid = texture_binding_layout_uid;
        new_uid.vector_span_offset = texture_binding_layouts_.size();
        new_uid.vector_span_length = texture_binding_count;
        texture_binding_layouts_.resize(new_uid.vector_span_offset +
                                        texture_binding_count);
        std::memcpy(
            texture_binding_layouts_.data() + new_uid.vector_span_offset,
            texture_bindings, texture_binding_layout_bytes);
        texture_binding_layout_map_.emplace(texture_binding_layout_hash,
                                            new_uid);
      }
    }
    if (bindless_sampler_count) {
      auto found_range =
          bindless_sampler_layout_map_.equal_range(sampler_binding_layout_uid);
      for (auto it = found_range.first; it != found_range.second; ++it) {
        if (it->second.vector_span_length != bindless_sampler_count) {
          continue;
        }
        sampler_binding_layout_uid = it->second.uid;
        const uint32_t* vector_bindless_sampler_layout =
            bindless_sampler_layouts_.data() + it->second.vector_span_offset;
        for (uint32_t i = 0; i < bindless_sampler_count; ++i) {
          if (vector_bindless_sampler_layout[i] !=
              sampler_bindings[i].bindless_descriptor_index) {
            sampler_binding_layout_uid = kLayoutUIDEmpty;
            break;
          }
        }
        if (sampler_binding_layout_uid != kLayoutUIDEmpty) {
          break;
        }
      }
      if (sampler_binding_layout_uid == kLayoutUIDEmpty) {
        sampler_binding_layout_uid = bindless_sampler_layout_map_.size();
        LayoutUID new_uid;
        static_assert(
            kLayoutUIDEmpty == 0,
            "Layout UID is size + 1 because it's assumed that 0 is the UID for "
            "an empty layout");
        new_uid.uid = sampler_binding_layout_uid + 1;
        new_uid.vector_span_offset = bindless_sampler_layouts_.size();
        new_uid.vector_span_length = sampler_binding_count;
        bindless_sampler_layouts_.resize(new_uid.vector_span_offset +
                                         sampler_binding_count);
        uint32_t* vector_bindless_sampler_layout =
            bindless_sampler_layouts_.data() + new_uid.vector_span_offset;
        for (uint32_t i = 0; i < bindless_sampler_count; ++i) {
          vector_bindless_sampler_layout[i] =
              sampler_bindings[i].bindless_descriptor_index;
        }
        bindless_sampler_layout_map_.emplace(bindless_sampler_layout_hash,
                                             new_uid);
      }
    }
  }
  shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);
  shader.SetSamplerBindingLayoutUserUID(sampler_binding_layout_uid);

  // Create a version of the shader with early depth/stencil forced by Xenia
  // itself when it's safe to do so or when EARLY_Z_ENABLE is set in
  // RB_DEPTHCONTROL.
  if (shader.type() == xenos::ShaderType::kPixel && !edram_rov_used_ &&
      !shader.writes_depth()) {
    shader.SetForcedEarlyZShaderObject(
        std::move(DxbcShaderTranslator::ForceEarlyDepthStencil(
            shader.translated_binary().data())));
  }

  // Disassemble the shader for dumping.
  auto& provider = command_processor_.GetD3D12Context().GetD3D12Provider();
  if (cvars::d3d12_dxbc_disasm_dxilconv) {
    shader.DisassembleDxbc(provider, cvars::d3d12_dxbc_disasm, dxbc_converter,
                           dxc_utils, dxc_compiler);
  } else {
    shader.DisassembleDxbc(provider, cvars::d3d12_dxbc_disasm);
  }

  // Dump shader files if desired.
  if (!cvars::dump_shaders.empty()) {
    shader.Dump(cvars::dump_shaders,
                (shader.type() == xenos::ShaderType::kPixel)
                    ? (edram_rov_used_ ? "d3d12_rov" : "d3d12_rtv")
                    : "d3d12");
  }

  return shader.is_valid();
}

bool PipelineCache::GetCurrentStateDescription(
    D3D12Shader* vertex_shader, D3D12Shader* pixel_shader,
    xenos::PrimitiveType primitive_type, xenos::IndexFormat index_format,
    bool early_z,
    const RenderTargetCache::PipelineRenderTarget render_targets[5],
    PipelineRuntimeDescription& runtime_description_out) {
  PipelineDescription& description_out = runtime_description_out.description;

  const auto& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  // Initialize all unused fields to zero for comparison/hashing.
  std::memset(&runtime_description_out, 0, sizeof(runtime_description_out));

  // Root signature.
  runtime_description_out.root_signature =
      command_processor_.GetRootSignature(vertex_shader, pixel_shader);
  if (runtime_description_out.root_signature == nullptr) {
    return false;
  }

  // Shaders.
  runtime_description_out.vertex_shader = vertex_shader;
  description_out.vertex_shader_hash = vertex_shader->ucode_data_hash();
  if (pixel_shader) {
    runtime_description_out.pixel_shader = pixel_shader;
    description_out.pixel_shader_hash = pixel_shader->ucode_data_hash();
  }

  // Index buffer strip cut value.
  if (pa_su_sc_mode_cntl.multi_prim_ib_ena) {
    // Not using 0xFFFF with 32-bit indices because in index buffers it will be
    // 0xFFFF0000 anyway due to endianness.
    description_out.strip_cut_index = index_format == xenos::IndexFormat::kInt32
                                          ? PipelineStripCutIndex::kFFFFFFFF
                                          : PipelineStripCutIndex::kFFFF;
  } else {
    description_out.strip_cut_index = PipelineStripCutIndex::kNone;
  }

  // Host vertex shader type and primitive topology.
  Shader::HostVertexShaderType host_vertex_shader_type =
      GetHostVertexShaderTypeIfValid();
  if (host_vertex_shader_type == Shader::HostVertexShaderType(-1)) {
    return false;
  }
  description_out.host_vertex_shader_type = host_vertex_shader_type;
  if (host_vertex_shader_type == Shader::HostVertexShaderType::kVertex) {
    switch (primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kPoint);
        break;
      case xenos::PrimitiveType::kLineList:
      case xenos::PrimitiveType::kLineStrip:
      case xenos::PrimitiveType::kLineLoop:
      // Quads are emulated as line lists with adjacency.
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::k2DLineStrip:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kLine);
        break;
      default:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kTriangle);
        break;
    }
    switch (primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.geometry_shader = PipelineGeometryShader::kPointList;
        break;
      case xenos::PrimitiveType::kRectangleList:
        description_out.geometry_shader =
            PipelineGeometryShader::kRectangleList;
        break;
      case xenos::PrimitiveType::kQuadList:
        description_out.geometry_shader = PipelineGeometryShader::kQuadList;
        break;
      default:
        description_out.geometry_shader = PipelineGeometryShader::kNone;
        break;
    }
  } else {
    description_out.primitive_topology_type_or_tessellation_mode =
        uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode);
  }

  bool primitive_polygonal = xenos::IsPrimitivePolygonal(
      host_vertex_shader_type != Shader::HostVertexShaderType::kVertex,
      primitive_type);

  // Rasterizer state.
  // Because Direct3D 12 doesn't support per-side fill mode and depth bias, the
  // values to use depends on the current culling state.
  // If front faces are culled, use the ones for back faces.
  // If back faces are culled, it's the other way around.
  // If culling is not enabled, assume the developer wanted to draw things in a
  // more special way - so if one side is wireframe or has a depth bias, then
  // that's intentional (if both sides have a depth bias, the one for the front
  // faces is used, though it's unlikely that they will ever be different -
  // SetRenderState sets the same offset for both sides).
  // Points fill mode (0) also isn't supported in Direct3D 12, but assume the
  // developer didn't want to fill the whole primitive and use wireframe (like
  // Xenos fill mode 1).
  // Here we also assume that only one side is culled - if two sides are culled,
  // the D3D12 command processor will drop such draw early.
  bool cull_front, cull_back;
  float poly_offset = 0.0f, poly_offset_scale = 0.0f;
  if (primitive_polygonal) {
    description_out.front_counter_clockwise = pa_su_sc_mode_cntl.face == 0;
    cull_front = pa_su_sc_mode_cntl.cull_front != 0;
    cull_back = pa_su_sc_mode_cntl.cull_back != 0;
    if (cull_front) {
      description_out.cull_mode = PipelineCullMode::kFront;
    } else if (cull_back) {
      description_out.cull_mode = PipelineCullMode::kBack;
    } else {
      description_out.cull_mode = PipelineCullMode::kNone;
    }
    // With ROV, the depth bias is applied in the pixel shader because
    // per-sample depth is needed for MSAA.
    if (!cull_front) {
      // Front faces aren't culled.
      // Direct3D 12, unfortunately, doesn't support point fill mode.
      if (pa_su_sc_mode_cntl.polymode_front_ptype !=
          xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
      if (!edram_rov_used_ && pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset = regs[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET].f32;
        poly_offset_scale = regs[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE].f32;
      }
    }
    if (!cull_back) {
      // Back faces aren't culled.
      if (pa_su_sc_mode_cntl.polymode_back_ptype !=
          xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
      // Prefer front depth bias because in general, front faces are the ones
      // that are rendered (except for shadow volumes).
      if (!edram_rov_used_ && pa_su_sc_mode_cntl.poly_offset_back_enable &&
          poly_offset == 0.0f && poly_offset_scale == 0.0f) {
        poly_offset = regs[XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET].f32;
        poly_offset_scale = regs[XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE].f32;
      }
    }
    if (pa_su_sc_mode_cntl.poly_mode == xenos::PolygonModeEnable::kDisabled) {
      description_out.fill_mode_wireframe = 0;
    }
  } else {
    // Filled front faces only, without culling.
    cull_front = false;
    cull_back = false;
    if (!edram_rov_used_ && pa_su_sc_mode_cntl.poly_offset_para_enable) {
      poly_offset = regs[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET].f32;
      poly_offset_scale = regs[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE].f32;
    }
  }
  if (!edram_rov_used_) {
    // Conversion based on the calculations in Call of Duty 4 and the values it
    // writes to the registers, and also on:
    // https://github.com/mesa3d/mesa/blob/54ad9b444c8e73da498211870e785239ad3ff1aa/src/gallium/drivers/radeonsi/si_state.c#L943
    // Dividing the scale by 2 - Call of Duty 4 sets the constant bias of
    // 1/32768 for decals, however, it's done in two steps in separate places:
    // first it's divided by 65536, and then it's multiplied by 2 (which is
    // consistent with what si_create_rs_state does, which multiplies the offset
    // by 2 if it comes from a non-D3D9 API for 24-bit depth buffers) - and
    // multiplying by 2 to the number of significand bits. Tested mostly in Call
    // of Duty 4 (vehicledamage map explosion decals) and Red Dead Redemption
    // (shadows - 2^17 is not enough, 2^18 hasn't been tested, but 2^19
    // eliminates the acne).
    if (regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
        xenos::DepthRenderTargetFormat::kD24FS8) {
      poly_offset *= float(1 << 19);
    } else {
      poly_offset *= float(1 << 23);
    }
    // Using ceil here just in case a game wants the offset but passes a value
    // that is too small - it's better to apply more offset than to make depth
    // fighting worse or to disable the offset completely (Direct3D 12 takes an
    // integer value).
    description_out.depth_bias = int32_t(std::ceil(std::abs(poly_offset))) *
                                 (poly_offset < 0.0f ? -1 : 1);
    // "slope computed in subpixels (1/12 or 1/16)" - R5xx Acceleration.
    description_out.depth_bias_slope_scaled =
        poly_offset_scale * (1.0f / 16.0f);
  }
  if (cvars::d3d12_tessellation_wireframe &&
      host_vertex_shader_type != Shader::HostVertexShaderType::kVertex) {
    description_out.fill_mode_wireframe = 1;
  }
  description_out.depth_clip = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  if (edram_rov_used_) {
    description_out.rov_msaa = regs.Get<reg::RB_SURFACE_INFO>().msaa_samples !=
                               xenos::MsaaSamples::k1X;
  } else {
    // Depth/stencil. No stencil, always passing depth test and no depth writing
    // means depth disabled.
    if (render_targets[4].format != DXGI_FORMAT_UNKNOWN) {
      auto rb_depthcontrol = regs.Get<reg::RB_DEPTHCONTROL>();
      if (rb_depthcontrol.z_enable) {
        description_out.depth_func = rb_depthcontrol.zfunc;
        description_out.depth_write = rb_depthcontrol.z_write_enable;
      } else {
        description_out.depth_func = xenos::CompareFunction::kAlways;
      }
      if (rb_depthcontrol.stencil_enable) {
        description_out.stencil_enable = 1;
        bool stencil_backface_enable =
            primitive_polygonal && rb_depthcontrol.backface_enable;
        // Per-face masks not supported by Direct3D 12, choose the back face
        // ones only if drawing only back faces.
        Register stencil_ref_mask_reg;
        if (stencil_backface_enable && cull_front) {
          stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
        } else {
          stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
        }
        auto stencil_ref_mask =
            regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg);
        description_out.stencil_read_mask = stencil_ref_mask.stencilmask;
        description_out.stencil_write_mask = stencil_ref_mask.stencilwritemask;
        description_out.stencil_front_fail_op = rb_depthcontrol.stencilfail;
        description_out.stencil_front_depth_fail_op =
            rb_depthcontrol.stencilzfail;
        description_out.stencil_front_pass_op = rb_depthcontrol.stencilzpass;
        description_out.stencil_front_func = rb_depthcontrol.stencilfunc;
        if (stencil_backface_enable) {
          description_out.stencil_back_fail_op = rb_depthcontrol.stencilfail_bf;
          description_out.stencil_back_depth_fail_op =
              rb_depthcontrol.stencilzfail_bf;
          description_out.stencil_back_pass_op =
              rb_depthcontrol.stencilzpass_bf;
          description_out.stencil_back_func = rb_depthcontrol.stencilfunc_bf;
        } else {
          description_out.stencil_back_fail_op =
              description_out.stencil_front_fail_op;
          description_out.stencil_back_depth_fail_op =
              description_out.stencil_front_depth_fail_op;
          description_out.stencil_back_pass_op =
              description_out.stencil_front_pass_op;
          description_out.stencil_back_func =
              description_out.stencil_front_func;
        }
      }
      // If not binding the DSV, ignore the format in the hash.
      if (description_out.depth_func != xenos::CompareFunction::kAlways ||
          description_out.depth_write || description_out.stencil_enable) {
        description_out.depth_format =
            regs.Get<reg::RB_DEPTH_INFO>().depth_format;
      }
    } else {
      description_out.depth_func = xenos::CompareFunction::kAlways;
    }
    if (early_z) {
      description_out.force_early_z = 1;
    }

    // Render targets and blending state. 32 because of 0x1F mask, for safety
    // (all unknown to zero).
    uint32_t color_mask = command_processor_.GetCurrentColorMask(pixel_shader);
    static const PipelineBlendFactor kBlendFactorMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcColor,
        /*  5 */ PipelineBlendFactor::kInvSrcColor,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDestColor,
        /*  9 */ PipelineBlendFactor::kInvDestColor,
        /* 10 */ PipelineBlendFactor::kDestAlpha,
        /* 11 */ PipelineBlendFactor::kInvDestAlpha,
        // CONSTANT_COLOR
        /* 12 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_COLOR
        /* 13 */ PipelineBlendFactor::kInvBlendFactor,
        // CONSTANT_ALPHA
        /* 14 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_ALPHA
        /* 15 */ PipelineBlendFactor::kInvBlendFactor,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
    };
    // Like kBlendFactorMap, but with color modes changed to alpha. Some
    // pipelines aren't created in Prey because a color mode is used for alpha.
    static const PipelineBlendFactor kBlendFactorAlphaMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcAlpha,
        /*  5 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDestAlpha,
        /*  9 */ PipelineBlendFactor::kInvDestAlpha,
        /* 10 */ PipelineBlendFactor::kDestAlpha,
        /* 11 */ PipelineBlendFactor::kInvDestAlpha,
        /* 12 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_COLOR
        /* 13 */ PipelineBlendFactor::kInvBlendFactor,
        // CONSTANT_ALPHA
        /* 14 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_ALPHA
        /* 15 */ PipelineBlendFactor::kInvBlendFactor,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
    };
    for (uint32_t i = 0; i < 4; ++i) {
      if (render_targets[i].format == DXGI_FORMAT_UNKNOWN) {
        break;
      }
      PipelineRenderTarget& rt = description_out.render_targets[i];
      rt.used = 1;
      uint32_t guest_rt_index = render_targets[i].guest_render_target;
      auto color_info = regs.Get<reg::RB_COLOR_INFO>(
          reg::RB_COLOR_INFO::rt_register_indices[guest_rt_index]);
      rt.format =
          RenderTargetCache::GetBaseColorFormat(color_info.color_format);
      rt.write_mask = (color_mask >> (guest_rt_index * 4)) & 0xF;
      if (rt.write_mask) {
        auto blendcontrol = regs.Get<reg::RB_BLENDCONTROL>(
            reg::RB_BLENDCONTROL::rt_register_indices[guest_rt_index]);
        rt.src_blend = kBlendFactorMap[uint32_t(blendcontrol.color_srcblend)];
        rt.dest_blend = kBlendFactorMap[uint32_t(blendcontrol.color_destblend)];
        rt.blend_op = blendcontrol.color_comb_fcn;
        rt.src_blend_alpha =
            kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_srcblend)];
        rt.dest_blend_alpha =
            kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_destblend)];
        rt.blend_op_alpha = blendcontrol.alpha_comb_fcn;
      } else {
        rt.src_blend = PipelineBlendFactor::kOne;
        rt.dest_blend = PipelineBlendFactor::kZero;
        rt.blend_op = xenos::BlendOp::kAdd;
        rt.src_blend_alpha = PipelineBlendFactor::kOne;
        rt.dest_blend_alpha = PipelineBlendFactor::kZero;
        rt.blend_op_alpha = xenos::BlendOp::kAdd;
      }
    }
  }

  return true;
}

ID3D12PipelineState* PipelineCache::CreateD3D12Pipeline(
    const PipelineRuntimeDescription& runtime_description) {
  const PipelineDescription& description = runtime_description.description;

  if (runtime_description.pixel_shader != nullptr) {
    XELOGGPU("Creating graphics pipeline with VS {:016X}, PS {:016X}",
             runtime_description.vertex_shader->ucode_data_hash(),
             runtime_description.pixel_shader->ucode_data_hash());
  } else {
    XELOGGPU("Creating graphics pipeline with VS {:016X}",
             runtime_description.vertex_shader->ucode_data_hash());
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC state_desc;
  std::memset(&state_desc, 0, sizeof(state_desc));

  // Root signature.
  state_desc.pRootSignature = runtime_description.root_signature;

  // Index buffer strip cut value.
  switch (description.strip_cut_index) {
    case PipelineStripCutIndex::kFFFF:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
      break;
    case PipelineStripCutIndex::kFFFFFFFF:
      state_desc.IBStripCutValue =
          D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
      break;
    default:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
      break;
  }

  // Primitive topology, vertex, hull, domain and geometry shaders.
  if (!runtime_description.vertex_shader->is_translated()) {
    XELOGE("Vertex shader {:016X} not translated",
           runtime_description.vertex_shader->ucode_data_hash());
    assert_always();
    return nullptr;
  }
  Shader::HostVertexShaderType host_vertex_shader_type =
      description.host_vertex_shader_type;
  if (runtime_description.vertex_shader->host_vertex_shader_type() !=
      host_vertex_shader_type) {
    XELOGE(
        "Vertex shader {:016X} translated into the wrong host shader "
        "type",
        runtime_description.vertex_shader->ucode_data_hash());
    assert_always();
    return nullptr;
  }
  if (host_vertex_shader_type == Shader::HostVertexShaderType::kVertex) {
    state_desc.VS.pShaderBytecode =
        runtime_description.vertex_shader->translated_binary().data();
    state_desc.VS.BytecodeLength =
        runtime_description.vertex_shader->translated_binary().size();
    PipelinePrimitiveTopologyType primitive_topology_type =
        PipelinePrimitiveTopologyType(
            description.primitive_topology_type_or_tessellation_mode);
    switch (primitive_topology_type) {
      case PipelinePrimitiveTopologyType::kPoint:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        break;
      case PipelinePrimitiveTopologyType::kLine:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
      case PipelinePrimitiveTopologyType::kTriangle:
        state_desc.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
      default:
        assert_unhandled_case(primitive_topology_type);
        return nullptr;
    }
    switch (description.geometry_shader) {
      case PipelineGeometryShader::kPointList:
        state_desc.GS.pShaderBytecode = primitive_point_list_gs;
        state_desc.GS.BytecodeLength = sizeof(primitive_point_list_gs);
        break;
      case PipelineGeometryShader::kRectangleList:
        state_desc.GS.pShaderBytecode = primitive_rectangle_list_gs;
        state_desc.GS.BytecodeLength = sizeof(primitive_rectangle_list_gs);
        break;
      case PipelineGeometryShader::kQuadList:
        state_desc.GS.pShaderBytecode = primitive_quad_list_gs;
        state_desc.GS.BytecodeLength = sizeof(primitive_quad_list_gs);
        break;
      default:
        break;
    }
  } else {
    state_desc.VS.pShaderBytecode = tessellation_vs;
    state_desc.VS.BytecodeLength = sizeof(tessellation_vs);
    state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    xenos::TessellationMode tessellation_mode = xenos::TessellationMode(
        description.primitive_topology_type_or_tessellation_mode);
    switch (tessellation_mode) {
      case xenos::TessellationMode::kDiscrete:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = discrete_triangle_hs;
            state_desc.HS.BytecodeLength = sizeof(discrete_triangle_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = discrete_quad_hs;
            state_desc.HS.BytecodeLength = sizeof(discrete_quad_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kContinuous:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = continuous_triangle_hs;
            state_desc.HS.BytecodeLength = sizeof(continuous_triangle_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = continuous_quad_hs;
            state_desc.HS.BytecodeLength = sizeof(continuous_quad_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kAdaptive:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = adaptive_triangle_hs;
            state_desc.HS.BytecodeLength = sizeof(adaptive_triangle_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = adaptive_quad_hs;
            state_desc.HS.BytecodeLength = sizeof(adaptive_quad_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      default:
        assert_unhandled_case(tessellation_mode);
        return nullptr;
    }
    state_desc.DS.pShaderBytecode =
        runtime_description.vertex_shader->translated_binary().data();
    state_desc.DS.BytecodeLength =
        runtime_description.vertex_shader->translated_binary().size();
  }

  // Pixel shader.
  if (runtime_description.pixel_shader != nullptr) {
    if (!runtime_description.pixel_shader->is_translated()) {
      XELOGE("Pixel shader {:016X} not translated",
             runtime_description.pixel_shader->ucode_data_hash());
      assert_always();
      return nullptr;
    }
    const auto& forced_early_z_shader =
        runtime_description.pixel_shader->GetForcedEarlyZShaderObject();
    if (description.force_early_z && forced_early_z_shader.size() != 0) {
      state_desc.PS.pShaderBytecode = forced_early_z_shader.data();
      state_desc.PS.BytecodeLength = forced_early_z_shader.size();
    } else {
      state_desc.PS.pShaderBytecode =
          runtime_description.pixel_shader->translated_binary().data();
      state_desc.PS.BytecodeLength =
          runtime_description.pixel_shader->translated_binary().size();
    }
  } else if (edram_rov_used_) {
    state_desc.PS.pShaderBytecode = depth_only_pixel_shader_.data();
    state_desc.PS.BytecodeLength = depth_only_pixel_shader_.size();
  }

  // Rasterizer state.
  state_desc.SampleMask = UINT_MAX;
  state_desc.RasterizerState.FillMode = description.fill_mode_wireframe
                                            ? D3D12_FILL_MODE_WIREFRAME
                                            : D3D12_FILL_MODE_SOLID;
  switch (description.cull_mode) {
    case PipelineCullMode::kFront:
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
      break;
    case PipelineCullMode::kBack:
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
      break;
    default:
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      break;
  }
  state_desc.RasterizerState.FrontCounterClockwise =
      description.front_counter_clockwise ? TRUE : FALSE;
  state_desc.RasterizerState.DepthBias = description.depth_bias;
  state_desc.RasterizerState.DepthBiasClamp = 0.0f;
  state_desc.RasterizerState.SlopeScaledDepthBias =
      description.depth_bias_slope_scaled * float(resolution_scale_);
  state_desc.RasterizerState.DepthClipEnable =
      description.depth_clip ? TRUE : FALSE;
  if (edram_rov_used_) {
    // Only 1, 4, 8 and (not on all GPUs) 16 are allowed, using sample 0 as 0
    // and 3 as 1 for 2x instead (not exactly the same sample positions, but
    // still top-left and bottom-right - however, this can be adjusted with
    // programmable sample positions).
    state_desc.RasterizerState.ForcedSampleCount = description.rov_msaa ? 4 : 1;
  }

  // Sample description.
  state_desc.SampleDesc.Count = 1;

  if (!edram_rov_used_) {
    // Depth/stencil.
    if (description.depth_func != xenos::CompareFunction::kAlways ||
        description.depth_write) {
      state_desc.DepthStencilState.DepthEnable = TRUE;
      state_desc.DepthStencilState.DepthWriteMask =
          description.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL
                                  : D3D12_DEPTH_WRITE_MASK_ZERO;
      // Comparison functions are the same in Direct3D 12 but plus one (minus
      // one, bit 0 for less, bit 1 for equal, bit 2 for greater).
      state_desc.DepthStencilState.DepthFunc =
          D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) +
                                uint32_t(description.depth_func));
    }
    if (description.stencil_enable) {
      state_desc.DepthStencilState.StencilEnable = TRUE;
      state_desc.DepthStencilState.StencilReadMask =
          description.stencil_read_mask;
      state_desc.DepthStencilState.StencilWriteMask =
          description.stencil_write_mask;
      // Stencil operations are the same in Direct3D 12 too but plus one.
      state_desc.DepthStencilState.FrontFace.StencilFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_front_fail_op));
      state_desc.DepthStencilState.FrontFace.StencilDepthFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_front_depth_fail_op));
      state_desc.DepthStencilState.FrontFace.StencilPassOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_front_pass_op));
      state_desc.DepthStencilState.FrontFace.StencilFunc =
          D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) +
                                uint32_t(description.stencil_front_func));
      state_desc.DepthStencilState.BackFace.StencilFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_back_fail_op));
      state_desc.DepthStencilState.BackFace.StencilDepthFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_back_depth_fail_op));
      state_desc.DepthStencilState.BackFace.StencilPassOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_back_pass_op));
      state_desc.DepthStencilState.BackFace.StencilFunc =
          D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) +
                                uint32_t(description.stencil_back_func));
    }
    if (state_desc.DepthStencilState.DepthEnable ||
        state_desc.DepthStencilState.StencilEnable) {
      state_desc.DSVFormat =
          RenderTargetCache::GetDepthDXGIFormat(description.depth_format);
    }
    // TODO(Triang3l): EARLY_Z_ENABLE (needs to be enabled in shaders, but alpha
    // test is dynamic - should be enabled anyway if there's no alpha test,
    // discarding and depth output).

    // Render targets and blending.
    state_desc.BlendState.IndependentBlendEnable = TRUE;
    static const D3D12_BLEND kBlendFactorMap[] = {
        D3D12_BLEND_ZERO,          D3D12_BLEND_ONE,
        D3D12_BLEND_SRC_COLOR,     D3D12_BLEND_INV_SRC_COLOR,
        D3D12_BLEND_SRC_ALPHA,     D3D12_BLEND_INV_SRC_ALPHA,
        D3D12_BLEND_DEST_COLOR,    D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_DEST_ALPHA,    D3D12_BLEND_INV_DEST_ALPHA,
        D3D12_BLEND_BLEND_FACTOR,  D3D12_BLEND_INV_BLEND_FACTOR,
        D3D12_BLEND_SRC_ALPHA_SAT,
    };
    static const D3D12_BLEND_OP kBlendOpMap[] = {
        D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT,     D3D12_BLEND_OP_MIN,
        D3D12_BLEND_OP_MAX, D3D12_BLEND_OP_REV_SUBTRACT,
    };
    for (uint32_t i = 0; i < 4; ++i) {
      const PipelineRenderTarget& rt = description.render_targets[i];
      if (!rt.used) {
        break;
      }
      ++state_desc.NumRenderTargets;
      state_desc.RTVFormats[i] =
          RenderTargetCache::GetColorDXGIFormat(rt.format);
      if (state_desc.RTVFormats[i] == DXGI_FORMAT_UNKNOWN) {
        assert_always();
        return nullptr;
      }
      D3D12_RENDER_TARGET_BLEND_DESC& blend_desc =
          state_desc.BlendState.RenderTarget[i];
      // Treat 1 * src + 0 * dest as disabled blending (there are opaque
      // surfaces drawn with blending enabled, but it's 1 * src + 0 * dest, in
      // Call of Duty 4 - GPU performance is better when not blending.
      if (rt.src_blend != PipelineBlendFactor::kOne ||
          rt.dest_blend != PipelineBlendFactor::kZero ||
          rt.blend_op != xenos::BlendOp::kAdd ||
          rt.src_blend_alpha != PipelineBlendFactor::kOne ||
          rt.dest_blend_alpha != PipelineBlendFactor::kZero ||
          rt.blend_op_alpha != xenos::BlendOp::kAdd) {
        blend_desc.BlendEnable = TRUE;
        blend_desc.SrcBlend = kBlendFactorMap[uint32_t(rt.src_blend)];
        blend_desc.DestBlend = kBlendFactorMap[uint32_t(rt.dest_blend)];
        blend_desc.BlendOp = kBlendOpMap[uint32_t(rt.blend_op)];
        blend_desc.SrcBlendAlpha =
            kBlendFactorMap[uint32_t(rt.src_blend_alpha)];
        blend_desc.DestBlendAlpha =
            kBlendFactorMap[uint32_t(rt.dest_blend_alpha)];
        blend_desc.BlendOpAlpha = kBlendOpMap[uint32_t(rt.blend_op_alpha)];
      }
      blend_desc.RenderTargetWriteMask = rt.write_mask;
    }
  }

  // Create the D3D12 pipeline state object.
  auto device =
      command_processor_.GetD3D12Context().GetD3D12Provider().GetDevice();
  ID3D12PipelineState* state;
  if (FAILED(device->CreateGraphicsPipelineState(&state_desc,
                                                 IID_PPV_ARGS(&state)))) {
    if (runtime_description.pixel_shader != nullptr) {
      XELOGE("Failed to create graphics pipeline with VS {:016X}, PS {:016X}",
             runtime_description.vertex_shader->ucode_data_hash(),
             runtime_description.pixel_shader->ucode_data_hash());
    } else {
      XELOGE("Failed to create graphics pipeline with VS {:016X}",
             runtime_description.vertex_shader->ucode_data_hash());
    }
    return nullptr;
  }
  std::wstring name;
  if (runtime_description.pixel_shader != nullptr) {
    name = fmt::format(L"VS {:016X}, PS {:016X}",
                       runtime_description.vertex_shader->ucode_data_hash(),
                       runtime_description.pixel_shader->ucode_data_hash());
  } else {
    name = fmt::format(L"VS {:016X}",
                       runtime_description.vertex_shader->ucode_data_hash());
  }
  state->SetName(name.c_str());
  return state;
}

void PipelineCache::StorageWriteThread() {
  ShaderStoredHeader shader_header;
  // Don't leak anything in unused bits.
  std::memset(&shader_header, 0, sizeof(shader_header));

  std::vector<uint32_t> ucode_guest_endian;
  ucode_guest_endian.reserve(0xFFFF);

  bool flush_shaders = false;
  bool flush_pipelines = false;

  while (true) {
    if (flush_shaders) {
      flush_shaders = false;
      assert_not_null(shader_storage_file_);
      fflush(shader_storage_file_);
    }
    if (flush_pipelines) {
      flush_pipelines = false;
      assert_not_null(pipeline_storage_file_);
      fflush(pipeline_storage_file_);
    }

    std::pair<const Shader*, reg::SQ_PROGRAM_CNTL> shader_pair = {};
    PipelineStoredDescription pipeline_description;
    bool write_pipeline = false;
    {
      std::unique_lock<std::mutex> lock(storage_write_request_lock_);
      if (storage_write_thread_shutdown_) {
        return;
      }
      if (!storage_write_shader_queue_.empty()) {
        shader_pair = storage_write_shader_queue_.front();
        storage_write_shader_queue_.pop_front();
      } else if (storage_write_flush_shaders_) {
        storage_write_flush_shaders_ = false;
        flush_shaders = true;
      }
      if (!storage_write_pipeline_queue_.empty()) {
        std::memcpy(&pipeline_description,
                    &storage_write_pipeline_queue_.front(),
                    sizeof(pipeline_description));
        storage_write_pipeline_queue_.pop_front();
        write_pipeline = true;
      } else if (storage_write_flush_pipelines_) {
        storage_write_flush_pipelines_ = false;
        flush_pipelines = true;
      }
      if (!shader_pair.first && !write_pipeline) {
        storage_write_request_cond_.wait(lock);
        continue;
      }
    }

    const Shader* shader = shader_pair.first;
    if (shader) {
      shader_header.ucode_data_hash = shader->ucode_data_hash();
      shader_header.ucode_dword_count = shader->ucode_dword_count();
      shader_header.type = shader->type();
      shader_header.host_vertex_shader_type = shader->host_vertex_shader_type();
      shader_header.sq_program_cntl = shader_pair.second;
      assert_not_null(shader_storage_file_);
      fwrite(&shader_header, sizeof(shader_header), 1, shader_storage_file_);
      if (shader_header.ucode_dword_count) {
        ucode_guest_endian.resize(shader_header.ucode_dword_count);
        // Need to swap because the hash is calculated for the shader with guest
        // endianness.
        xe::copy_and_swap(ucode_guest_endian.data(), shader->ucode_dwords(),
                          shader_header.ucode_dword_count);
        fwrite(ucode_guest_endian.data(),
               shader_header.ucode_dword_count * sizeof(uint32_t), 1,
               shader_storage_file_);
      }
    }

    if (write_pipeline) {
      assert_not_null(pipeline_storage_file_);
      fwrite(&pipeline_description, sizeof(pipeline_description), 1,
             pipeline_storage_file_);
    }
  }
}

void PipelineCache::CreationThread(size_t thread_index) {
  while (true) {
    Pipeline* pipeline_to_create = nullptr;

    // Check if need to shut down or set the completion event and dequeue the
    // pipeline if there is any.
    {
      std::unique_lock<std::mutex> lock(creation_request_lock_);
      if (thread_index >= creation_threads_shutdown_from_ ||
          creation_queue_.empty()) {
        if (creation_completion_set_event_ && creation_threads_busy_ == 0) {
          // Last pipeline in the queue created - signal the event if requested.
          creation_completion_set_event_ = false;
          creation_completion_event_->Set();
        }
        if (thread_index >= creation_threads_shutdown_from_) {
          return;
        }
        creation_request_cond_.wait(lock);
        continue;
      }
      // Take the pipeline from the queue and increment the busy thread count
      // until the pipeline is created - other threads must be able to dequeue
      // requests, but can't set the completion event until the pipelines are
      // fully created (rather than just started creating).
      pipeline_to_create = creation_queue_.front();
      creation_queue_.pop_front();
      ++creation_threads_busy_;
    }

    // Create the D3D12 pipeline state object.
    pipeline_to_create->state =
        CreateD3D12Pipeline(pipeline_to_create->description);

    // Pipeline created - the thread is not busy anymore, safe to set the
    // completion event if needed (at the next iteration, or in some other
    // thread).
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      --creation_threads_busy_;
    }
  }
}

void PipelineCache::CreateQueuedPipelinesOnProcessorThread() {
  assert_false(creation_threads_.empty());
  while (true) {
    Pipeline* pipeline_to_create;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      if (creation_queue_.empty()) {
        break;
      }
      pipeline_to_create = creation_queue_.front();
      creation_queue_.pop_front();
    }
    pipeline_to_create->state =
        CreateD3D12Pipeline(pipeline_to_create->description);
  }
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
