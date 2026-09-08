/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>

#include <rex/assert.h>
#include <rex/math.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

#include <spirv-tools/optimizer.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>

// Run SPIRV-Tools' performance passes over a shader before the driver sees it.
//
// Qualcomm's Adreno driver compiles SPIR-V with its own LLVM stack, and older
// builds of it segfault on shapes an optimizer would have folded away. The
// same crash is reported against Filament (google/filament#5294 - a null
// dereference inside QGLCCompileToIRShader, with the note that unoptimized
// shaders are needed to reproduce it), Bevy, Flutter's Impeller and Unity:
// always Adreno, never Mali or desktop.
//
// A Retroid Pocket 5 - Adreno 650 on driver 0746.0 from September 2023 -
// faults inside vulkan.adreno.so about three seconds in, on the thread that
// compiles guest shaders. A Galaxy S23 FE (Adreno 730, current driver) never
// does. This exists to find out whether pre-optimising removes the shapes that
// older compiler cannot handle.
//
// Off by default: it costs compile time on every shader and buys a working
// driver nothing.
REXCVAR_DEFINE_BOOL(vulkan_spirv_optimize, false, "UI/Vulkan",
                    "Run the SPIR-V optimizer over shaders before creating them. Works around "
                    "older Adreno drivers that crash in their own shader compiler on "
                    "unoptimized SPIR-V.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex {
namespace ui {
namespace vulkan {
namespace util {

void FlushMappedMemoryRange(const VulkanDevice* const vulkan_device, const VkDeviceMemory memory,
                            const uint32_t memory_type, const VkDeviceSize offset,
                            const VkDeviceSize memory_size, const VkDeviceSize size) {
  assert_false(size != VK_WHOLE_SIZE && memory_size == VK_WHOLE_SIZE);
  assert_true(memory_size == VK_WHOLE_SIZE || offset <= memory_size);
  assert_true(memory_size == VK_WHOLE_SIZE || size <= memory_size - offset);
  if (!size || (vulkan_device->memory_types().host_coherent & (uint32_t(1) << memory_type))) {
    return;
  }
  VkMappedMemoryRange range;
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.pNext = nullptr;
  range.memory = memory;
  range.offset = offset;
  range.size = size;
  const VkDeviceSize non_coherent_atom_size = vulkan_device->properties().nonCoherentAtomSize;
  range.offset = offset / non_coherent_atom_size * non_coherent_atom_size;
  if (size != VK_WHOLE_SIZE) {
    range.size =
        std::min(rex::round_up(offset + size, non_coherent_atom_size), memory_size) - range.offset;
  }
  vulkan_device->functions().vkFlushMappedMemoryRanges(vulkan_device->device(), 1, &range);
}

bool CreateDedicatedAllocationBuffer(const VulkanDevice* const vulkan_device,
                                     const VkDeviceSize size, const VkBufferUsageFlags usage,
                                     const MemoryPurpose memory_purpose, VkBuffer& buffer_out,
                                     VkDeviceMemory& memory_out, uint32_t* const memory_type_out,
                                     VkDeviceSize* const memory_size_out) {
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = 0;
  buffer_create_info.size = size;
  buffer_create_info.usage = usage;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_create_info.queueFamilyIndexCount = 0;
  buffer_create_info.pQueueFamilyIndices = nullptr;
  VkBuffer buffer;
  if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements memory_requirements;
  dfn.vkGetBufferMemoryRequirements(device, buffer, &memory_requirements);
  uint32_t memory_type = ChooseMemoryType(vulkan_device->memory_types(),
                                          memory_requirements.memoryTypeBits, memory_purpose);
  if (memory_type == UINT32_MAX) {
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = memory_requirements.size;
  memory_allocate_info.memoryTypeIndex = memory_type;
  VkMemoryDedicatedAllocateInfo memory_dedicated_allocate_info;
  if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
    memory_dedicated_allocate_info.pNext = memory_allocate_info.pNext;
    memory_allocate_info.pNext = &memory_dedicated_allocate_info;
    memory_dedicated_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    memory_dedicated_allocate_info.pNext = nullptr;
    memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
    memory_dedicated_allocate_info.buffer = buffer;
  }
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  if (dfn.vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return false;
  }

  buffer_out = buffer;
  memory_out = memory;
  if (memory_type_out) {
    *memory_type_out = memory_type;
  }
  if (memory_size_out) {
    *memory_size_out = memory_allocate_info.allocationSize;
  }
  return true;
}

bool CreateDedicatedAllocationImage(const VulkanDevice* const vulkan_device,
                                    const VkImageCreateInfo& create_info,
                                    const MemoryPurpose memory_purpose, VkImage& image_out,
                                    VkDeviceMemory& memory_out, uint32_t* const memory_type_out,
                                    VkDeviceSize* const memory_size_out) {
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkImage image;
  if (dfn.vkCreateImage(device, &create_info, nullptr, &image) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements memory_requirements;
  dfn.vkGetImageMemoryRequirements(device, image, &memory_requirements);
  uint32_t memory_type = ChooseMemoryType(vulkan_device->memory_types(),
                                          memory_requirements.memoryTypeBits, memory_purpose);
  if (memory_type == UINT32_MAX) {
    dfn.vkDestroyImage(device, image, nullptr);
    return false;
  }

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = memory_requirements.size;
  memory_allocate_info.memoryTypeIndex = memory_type;
  VkMemoryDedicatedAllocateInfo memory_dedicated_allocate_info;
  if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
    memory_dedicated_allocate_info.pNext = memory_allocate_info.pNext;
    memory_allocate_info.pNext = &memory_dedicated_allocate_info;
    memory_dedicated_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    memory_dedicated_allocate_info.pNext = nullptr;
    memory_dedicated_allocate_info.image = image;
    memory_dedicated_allocate_info.buffer = VK_NULL_HANDLE;
  }
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
    dfn.vkDestroyImage(device, image, nullptr);
    return false;
  }

  if (dfn.vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return false;
  }

  image_out = image;
  memory_out = memory;
  if (memory_type_out) {
    *memory_type_out = memory_type;
  }
  if (memory_size_out) {
    *memory_size_out = memory_allocate_info.allocationSize;
  }
  return true;
}

VkPipeline CreateComputePipeline(const VulkanDevice* const vulkan_device,
                                 const VkPipelineLayout layout, const VkShaderModule shader,
                                 const VkSpecializationInfo* const specialization_info,
                                 const char* const entry_point) {
  VkComputePipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = nullptr;
  pipeline_create_info.flags = 0;
  pipeline_create_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_create_info.stage.pNext = nullptr;
  pipeline_create_info.stage.flags = 0;
  pipeline_create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_create_info.stage.module = shader;
  pipeline_create_info.stage.pName = entry_point;
  pipeline_create_info.stage.pSpecializationInfo = specialization_info;
  pipeline_create_info.layout = layout;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  VkPipeline pipeline;
  if (vulkan_device->functions().vkCreateComputePipelines(vulkan_device->device(), VK_NULL_HANDLE,
                                                          1, &pipeline_create_info, nullptr,
                                                          &pipeline) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

VkPipeline CreateComputePipeline(const VulkanDevice* const vulkan_device, VkPipelineLayout layout,
                                 const uint32_t* shader_code, size_t shader_code_size_bytes,
                                 const VkSpecializationInfo* specialization_info,
                                 const char* entry_point) {
  const VkShaderModule shader =
      CreateShaderModule(vulkan_device, shader_code, shader_code_size_bytes);
  if (shader == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  const VkPipeline pipeline =
      CreateComputePipeline(vulkan_device, layout, shader, specialization_info, entry_point);
  vulkan_device->functions().vkDestroyShaderModule(vulkan_device->device(), shader, nullptr);
  return pipeline;
}

bool OptimizeSpirv(const uint32_t* code, size_t code_size_bytes,
                   std::vector<uint32_t>& optimized_out) {
  if (!REXCVAR_GET(vulkan_spirv_optimize) || code == nullptr || code_size_bytes < 4) {
    return false;
  }
  // Match the module's own SPIR-V version, do not assume one.
  //
  // Hardcoding SPV_ENV_VULKAN_1_1 here made the first attempt at this a
  // no-op: every shader is SPIR-V 1.4, the optimizer rejected all of them
  // with "Invalid SPIR-V binary version 1.4 for target environment SPIR-V
  // 1.3", and the fallback quietly handed the driver the original bytes. The
  // build looked like it disproved the theory when it had never tested it.
  //
  // Word 1 of a SPIR-V module is its version, packed as 0x00MMmm00.
  const uint32_t spirv_version = code[1];
  spv_target_env env = SPV_ENV_VULKAN_1_0;
  switch (spirv_version) {
    case 0x00010000u: env = SPV_ENV_VULKAN_1_0; break;
    case 0x00010300u: env = SPV_ENV_VULKAN_1_1; break;
    case 0x00010400u: env = SPV_ENV_VULKAN_1_1_SPIRV_1_4; break;
    case 0x00010500u: env = SPV_ENV_VULKAN_1_2; break;
    case 0x00010600u: env = SPV_ENV_VULKAN_1_3; break;
    default:          env = SPV_ENV_VULKAN_1_1_SPIRV_1_4; break;
  }
  spvtools::Optimizer optimizer(env);
  optimizer.SetMessageConsumer(
      [](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
        if (message != nullptr) {
          REXLOG_WARN("SPIR-V optimizer: {}", message);
        }
      });
  optimizer.RegisterPerformancePasses();
  std::vector<uint32_t> result;
  if (!optimizer.Run(code, code_size_bytes / sizeof(uint32_t), &result) || result.empty()) {
    // Leaving the shader as it was is the safe outcome: it is exactly what
    // every driver has been handed until now.
    return false;
  }
  optimized_out = std::move(result);
  return true;
}

}  // namespace util
}  // namespace vulkan
}  // namespace ui
}  // namespace rex
