/*
* Copyright (c) 2021-2022, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include "dxvk_barrier.h"
#include "dxvk_buffer.h"
#include "dxvk_device.h"
#include "../util/util_kenshi_prepared_terrain.h"

#include <algorithm>
#include <numeric>

namespace dxvk {
  
  DxvkBuffer::DxvkBuffer(
          DxvkDevice*           device,
    const DxvkBufferCreateInfo& createInfo,
          DxvkMemoryAllocator&  memAlloc,
          VkMemoryPropertyFlags memFlags,
          DxvkMemoryStats::Category category,
          const char* name)
  : m_device        (device),
    m_info          (createInfo),
    m_memAlloc      (&memAlloc),
    m_memFlags      (memFlags),
    m_category      (category),
    // NV-DXVK start: Implement memory profiler
    m_tracker(name, GpuMemoryTracker::Buffer, category, { 1, 1, 1 }, VK_FORMAT_UNDEFINED) {
    // NV-DXVK end
    // DX11_V319_FAULT_ADDRESS_NAMING: see m_gpuAddressName.
    if (DxvkGpuAddressRegistry::get().isEnabled() && name != nullptr)
      m_gpuAddressName = name;
    // Align slices so that we don't violate any alignment
    // requirements imposed by the Vulkan device/driver
    VkDeviceSize sliceAlignment = computeSliceAlignment();
    m_physSliceLength = createInfo.size;
    m_physSliceStride = align(createInfo.size, sliceAlignment);
    m_physSliceCount  = std::max<VkDeviceSize>(1, 256 / m_physSliceStride);

    // Limit size of multi-slice buffers to reduce fragmentation
    constexpr VkDeviceSize MaxBufferSize = 256 << 10;

    m_physSliceMaxCount = MaxBufferSize >= m_physSliceStride
      ? MaxBufferSize / m_physSliceStride
      : 1;

    // Allocate the initial set of buffer slices
    m_buffer = allocBuffer(m_physSliceCount, category);

    DxvkBufferSliceHandle slice;
    slice.handle = m_buffer.buffer; 
    slice.offset = 0;
    slice.length = m_physSliceLength;
    slice.mapPtr = m_buffer.memory.mapPtr(0);

    m_physSlice = slice;
    m_lazyAlloc = m_physSliceCount > 1;

    // NV-DXVK start: Implement memory profiler
    m_tracker.finalize(m_physSliceStride, (m_buffer.memory.propertyFlags() & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0, (m_buffer.memory.propertyFlags() != memFlags));

    // NV-DXVK start: add debug names to buffer objects
    if (device->vkd()->vkSetDebugUtilsObjectNameEXT) {
      VkDebugUtilsObjectNameInfoEXT nameInfo;
      nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
      nameInfo.pNext = nullptr;
      nameInfo.objectType = VK_OBJECT_TYPE_BUFFER;
      nameInfo.objectHandle = (uint64_t) m_buffer.buffer;
      nameInfo.pObjectName = name;
      device->vkd()->vkSetDebugUtilsObjectNameEXT(device->vkd()->device(), &nameInfo);
    }
    // NV-DXVK end
  }


  DxvkBuffer::~DxvkBuffer() {
    prepared_terrain::retired(this);
    // NV-DXVK start: buffer clones for orphaned slices
    if (m_parent != nullptr) {
      // Clones own nothing. Bail out.
      return;
    }
    // NV-DXVK end

    // DX11_V319_FAULT_ADDRESS_NAMING: the memory is about to go away. Once it
    // is gone a fault inside this range must report "no live allocation covers
    // this address", which is precisely the use-after-free signal worth having.
    DxvkGpuAddressRegistry::get().remove(m_deviceAddress);

    const auto& vkd = m_device->vkd();

    for (const auto& buffer : m_buffers) {
      vkd->vkDestroyBuffer(vkd->device(), buffer.buffer, nullptr);
    }
    vkd->vkDestroyBuffer(vkd->device(), m_buffer.buffer, nullptr);
  }
  

  DxvkGpuAddressRegistry& DxvkGpuAddressRegistry::get() {
    static DxvkGpuAddressRegistry s_instance;
    return s_instance;
  }


  void DxvkGpuAddressRegistry::add(VkDeviceAddress address, VkDeviceSize size,
                                   const char* name) {
    if (!isEnabled() || address == 0)
      return;

    std::lock_guard<dxvk::mutex> lock(m_mutex);
    Entry& entry = m_entries[address];
    entry.size = size;
    entry.name = name != nullptr ? name : "<unnamed>";
  }


  void DxvkGpuAddressRegistry::remove(VkDeviceAddress address) {
    if (!isEnabled() || address == 0)
      return;

    std::lock_guard<dxvk::mutex> lock(m_mutex);

    // Record the range before it is gone, so a later fault inside it can still be attributed (see the
    // header).
    auto entry = m_entries.find(address);
    if (entry != m_entries.end()) {
      FreedEntry freed;
      freed.base   = address;
      freed.size   = entry->second.size;
      freed.serial = ++m_freedSerial;
      freed.name   = entry->second.name;

      if (m_freed.size() < kFreedHistory) {
        m_freed.push_back(std::move(freed));
      } else {
        m_freed[m_freedNext] = std::move(freed);
        m_freedNext = (m_freedNext + 1u) % kFreedHistory;
      }

      m_entries.erase(entry);
    }
  }


  std::string DxvkGpuAddressRegistry::describeFreedLocked(VkDeviceAddress address) const {
    // Most recent wins: the same page is reused, and the newest owner is the
    // one whose lifetime the fault actually straddles.
    const FreedEntry* best = nullptr;

    for (const auto& candidate : m_freed) {
      if (candidate.base == 0
       || address < candidate.base
       || address >= candidate.base + candidate.size)
        continue;

      if (best == nullptr || candidate.serial > best->serial)
        best = &candidate;
    }

    if (best == nullptr)
      return std::string();

    return str::format(" || RETIRED BUFFER BINDING FOUND (rename or destruction; physical lifetime unknown): '", best->name,
      "' base=0x", std::hex, best->base, std::dec,
      " size=", best->size,
      " offsetIntoAllocation=", address - best->base,
      " (retired ", m_freedSerial - best->serial,
      " allocations ago, of ", m_freed.size(), " remembered)");
  }


  std::string DxvkGpuAddressRegistry::describe(VkDeviceAddress address) const {
    if (!isEnabled())
      return "allocation naming disabled (VK_EXT_device_fault not enabled at device creation)";

    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (m_entries.empty())
      return "no tracked allocations";

    // The first entry starting after the address cannot contain it, so the
    // only candidate is the one immediately before it.
    auto candidate = m_entries.upper_bound(address);
    if (candidate == m_entries.begin()) {
      return str::format("below every tracked allocation (lowest base 0x",
        std::hex, m_entries.begin()->first, std::dec,
        ") - outside queried-buffer coverage; images/descriptors/shader code may be untracked",
        describeFreedLocked(address));
    }

    --candidate;
    const VkDeviceAddress base = candidate->first;
    const VkDeviceSize size = candidate->second.size;
    if (address >= base + size) {
      // Landing in the gap after an allocation is the most diagnostic outcome
      // of all: the GPU dereferenced a pointer into memory nothing live owns.
      return str::format("NO tracked queried-buffer range covers this address"
        " (nearest below: '", candidate->second.name, "' base=0x",
        std::hex, base, " end=0x", base + size, std::dec,
        ", overrun by ", address - (base + size),
        " bytes); cause unknown with incomplete coverage",
        describeFreedLocked(address));
    }

    return str::format("'", candidate->second.name, "' base=0x",
      std::hex, base, std::dec, " size=", size,
      " offsetIntoAllocation=", address - base);
  }


  VkDeviceAddress DxvkBuffer::getDeviceAddress() {
    const auto& vkd = m_device->vkd();

    if (m_deviceAddress == 0) {
      VkBufferDeviceAddressInfo bufferInfo { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
      bufferInfo.buffer = m_physSlice.handle;
      m_deviceAddress = vkd->vkGetBufferDeviceAddress(vkd->device(), &bufferInfo);
      m_deviceAddress += getDynamicOffset(0);

      // DX11_V319_FAULT_ADDRESS_NAMING: taking the device address is what makes
      // this buffer reachable by a raw GPU pointer, so this is the exact moment
      // it becomes capable of producing an unattributable page fault. rename()
      // drops the registration again along with the cached address.
      DxvkGpuAddressRegistry::get().add(
        m_deviceAddress, m_physSlice.length,
        m_gpuAddressName.empty() ? nullptr : m_gpuAddressName.c_str());
    }
    return m_deviceAddress;
  }

  DxvkBufferHandle DxvkBuffer::allocBuffer(VkDeviceSize sliceCount, DxvkMemoryStats::Category category) const {
    const auto& vkd = m_device->vkd();

    const bool isAccelerationStructure = m_info.usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    VkBufferCreateInfo info;
    info.sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.pNext                 = nullptr;
    info.flags                 = 0;
    info.size                  = m_physSliceStride * sliceCount;
    info.usage                 = m_info.usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    // NV-DXVK start: include external memory handle type flags if specified
    VkExternalMemoryBufferCreateInfo extMemBufferCreateInfo = {};
    if (m_info.sharing.mode != DxvkSharedHandleMode::None) {
      extMemBufferCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
      extMemBufferCreateInfo.handleTypes = m_info.sharing.type;
      extMemBufferCreateInfo.pNext = nullptr;
      info.pNext = &extMemBufferCreateInfo;
    }
    // NV-DXVK end


    if (!isAccelerationStructure && m_device->features().vulkan12Features.bufferDeviceAddress)
    {
      info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    if (info.usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
    {
      info.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    info.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    info.queueFamilyIndexCount = 0;
    info.pQueueFamilyIndices   = nullptr;
    
    DxvkBufferHandle handle;

    if (vkd->vkCreateBuffer(vkd->device(),
          &info, nullptr, &handle.buffer) != VK_SUCCESS) {
      throw DxvkError(str::format(
        "DxvkBuffer: Failed to create buffer:"
        "\n  size:  ", info.size,
        "\n  usage: ", info.usage));
    }

    VkMemoryDedicatedRequirements dedicatedRequirements;
    dedicatedRequirements.sType                       = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    dedicatedRequirements.pNext                       = VK_NULL_HANDLE;
    dedicatedRequirements.prefersDedicatedAllocation  = VK_FALSE;
    dedicatedRequirements.requiresDedicatedAllocation = VK_FALSE;
    
    VkMemoryRequirements2 memReq;
    memReq.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    memReq.pNext = &dedicatedRequirements;
    
    VkBufferMemoryRequirementsInfo2 memReqInfo;
    memReqInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
    memReqInfo.buffer = handle.buffer;
    memReqInfo.pNext  = VK_NULL_HANDLE;
    
    VkMemoryDedicatedAllocateInfo dedMemoryAllocInfo;
    dedMemoryAllocInfo.sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedMemoryAllocInfo.pNext  = VK_NULL_HANDLE;
    dedMemoryAllocInfo.buffer = handle.buffer;
    dedMemoryAllocInfo.image  = VK_NULL_HANDLE;

    vkd->vkGetBufferMemoryRequirements2(
       vkd->device(), &memReqInfo, &memReq);

    // NV-DXVK start: include external memory handle type flags if specified
    VkExportMemoryAllocateInfo exportMemoryAllocateInfo = {};
    if (m_info.sharing.mode == DxvkSharedHandleMode::Export) {
      exportMemoryAllocateInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
      exportMemoryAllocateInfo.handleTypes = m_info.sharing.type;
      exportMemoryAllocateInfo.pNext = VK_NULL_HANDLE;
      dedMemoryAllocInfo.pNext = &exportMemoryAllocateInfo;
      dedicatedRequirements.requiresDedicatedAllocation = VK_TRUE;
      dedicatedRequirements.prefersDedicatedAllocation = VK_TRUE;
    } else if (m_info.sharing.mode == DxvkSharedHandleMode::Import) {
      // TODO: not yet implemented
      assert(false);
    }
    // NV-DXVK end

    // NV-DXVK start: Increase memory requirement alignment based on override requirement.
    // Note: This increase in alignment is safe to do as long as the override alignment is less than or equal to the maximum alignment
    // required by the Vulkan spec (since raw device memory allocation will only gaurentee alignment in such cases even if custom
    // sub-allocating logic can handle greater alignments).
    memReq.memoryRequirements.alignment = std::lcm(memReq.memoryRequirements.alignment, m_info.requiredAlignmentOverride);
    // NV-DXVK end

    // xxxnsubtil: avoid bad interaction with RtxStagingDataAlloc
    // when dedicated allocations are used, the implicit memory recycling in RtxStagingDataAlloc goes away for larger buffers,
    // which are often used for BVH builds; dedicated is not very meaningful for buffers, so ignore the hint if
    // dedicated memory is not strictly required
    if (!dedicatedRequirements.requiresDedicatedAllocation) {
      dedicatedRequirements.prefersDedicatedAllocation = VK_FALSE;
    }

    // Use high memory priority for GPU-writable resources
    bool isGpuWritable = (m_info.access & (
      VK_ACCESS_SHADER_WRITE_BIT |
      VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT)) != 0;
    
    DxvkMemoryFlags hints(DxvkMemoryFlag::GpuReadable);

    if (isGpuWritable)
      hints.set(DxvkMemoryFlag::GpuWritable);

    // Staging buffers that can't even be used as a transfer destinations
    // are likely short-lived, so we should put them on a separate memory
    // pool in order to avoid fragmentation
    if (DxvkBarrierSet::getAccessTypes(m_info.access) == DxvkAccess::Read)
      hints.set(DxvkMemoryFlag::Transient);

    // Ask driver whether we should be using a dedicated allocation
    handle.memory = m_memAlloc->alloc(&memReq.memoryRequirements,
      dedicatedRequirements, dedMemoryAllocInfo, m_memFlags, hints, category);
    
    if (vkd->vkBindBufferMemory(vkd->device(), handle.buffer,
        handle.memory.memory(), handle.memory.offset()) != VK_SUCCESS)
      throw DxvkError("DxvkBuffer: Failed to bind device memory");
    
    return handle;
  }


  VkDeviceSize DxvkBuffer::computeSliceAlignment() const {
    const auto& devInfo = m_device->properties().core.properties;

    VkDeviceSize result = sizeof(uint32_t);

    if (m_info.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
      result = std::max(result, devInfo.limits.minUniformBufferOffsetAlignment);

    if (m_info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
      result = std::max(result, devInfo.limits.minStorageBufferOffsetAlignment);

    if (m_info.usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR)
      result = std::max(result, (VkDeviceSize)m_device->properties().khrDeviceAccelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment);

    if (m_info.usage & VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT)
      result = std::max(result, VkDeviceSize(256u)); // https://vulkan.lunarg.com/doc/view/1.4.304.1/windows/antora/spec/latest/chapters/VK_EXT_opacity_micromap/micromaps.html#VUID-vkCmdBuildMicromapsEXT-pInfos-07515
    
    if (m_info.usage & (VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)) {
      result = std::max(result, devInfo.limits.minTexelBufferOffsetAlignment);
      result = std::max(result, VkDeviceSize(16));
    }

    if (m_info.usage & (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
     && m_info.size > (devInfo.limits.optimalBufferCopyOffsetAlignment / 2))
      result = std::max(result, devInfo.limits.optimalBufferCopyOffsetAlignment);

    // For some reason, Warhammer Chaosbane breaks otherwise
    if (m_info.usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
      result = std::max(result, VkDeviceSize(256));

    if (m_memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      result = std::max(result, devInfo.limits.nonCoherentAtomSize);
      result = std::max(result, VkDeviceSize(64));
    }

    return result;
  }

  // NV-DXVK start: buffer clones for orphaned slices
  DxvkBuffer::DxvkBuffer(DxvkBuffer& parent) 
    : m_device   (parent.m_device),
      m_info     (parent.m_info),
      m_memAlloc (parent.m_memAlloc),
      m_memFlags (parent.m_memFlags),
      m_category (parent.m_category){
    m_buffer.buffer = parent.m_buffer.buffer;

    m_physSlice = parent.m_physSlice;
    m_vertexStride = parent.m_vertexStride;

    m_parent = &parent;
  }

  Rc<DxvkBuffer> DxvkBuffer::clone() {
    if (m_parent != nullptr) {
      throw DxvkError("Refusing to clone a clone!");
    }
    return new DxvkBuffer(*this);
  }
  // NV-DXVK end


  
  DxvkBufferView::DxvkBufferView(
    const Rc<vk::DeviceFn>&         vkd,
    const Rc<DxvkBuffer>&           buffer,
    const DxvkBufferViewCreateInfo& info)
  : m_vkd(vkd), m_info(info), m_buffer(buffer),
    m_bufferSlice (getSliceHandle()),
    m_bufferView  (createBufferView(m_bufferSlice)) {
    
  }
  
  
  DxvkBufferView::~DxvkBufferView() {
    if (m_views.empty()) {
      m_vkd->vkDestroyBufferView(
        m_vkd->device(), m_bufferView, nullptr);
    } else {
      for (const auto& pair : m_views) {
        m_vkd->vkDestroyBufferView(
          m_vkd->device(), pair.second, nullptr);
      }
    }
  }
  
  
  VkBufferView DxvkBufferView::createBufferView(
    const DxvkBufferSliceHandle& slice) {
    VkBufferViewCreateInfo viewInfo;
    viewInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    viewInfo.pNext  = nullptr;
    viewInfo.flags  = 0;
    viewInfo.buffer = slice.handle;
    viewInfo.format = m_info.format;
    viewInfo.offset = slice.offset;
    viewInfo.range  = slice.length;
    
    VkBufferView result = VK_NULL_HANDLE;

    if (m_vkd->vkCreateBufferView(m_vkd->device(),
          &viewInfo, nullptr, &result) != VK_SUCCESS) {
      throw DxvkError(str::format(
        "DxvkBufferView: Failed to create buffer view:",
        "\n  Offset: ", viewInfo.offset,
        "\n  Range:  ", viewInfo.range,
        "\n  Format: ", viewInfo.format));
    }

    return result;
  }


  void DxvkBufferView::updateBufferView(
    const DxvkBufferSliceHandle& slice) {
    if (m_views.empty())
      m_views.insert({ m_bufferSlice, m_bufferView });
     
    m_bufferSlice = slice;
    
    auto entry = m_views.find(slice);
    if (entry != m_views.end()) {
      m_bufferView = entry->second;
    } else {
      m_bufferView = createBufferView(m_bufferSlice);
      m_views.insert({ m_bufferSlice, m_bufferView });
    }
  }
  
  // NV-DXVK start: implement acceleration structures
  DxvkAccelStructure::DxvkAccelStructure(
        DxvkDevice* device,
  const DxvkBufferCreateInfo& createInfo,
        DxvkMemoryAllocator& memAlloc,
        VkMemoryPropertyFlags memFlags,
        VkAccelerationStructureTypeKHR accelType,
  const char* name)
    : DxvkBuffer(device, createInfo, memAlloc, memFlags, DxvkMemoryStats::Category::RTXAccelerationStructure, name) {

    VkAccelerationStructureCreateInfoKHR accelCreateInfo {};
    accelCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    accelCreateInfo.pNext = nullptr;
    accelCreateInfo.createFlags = 0;
    accelCreateInfo.buffer = DxvkBuffer::getBufferRaw();
    accelCreateInfo.offset = 0;
    accelCreateInfo.size = createInfo.size;
    accelCreateInfo.type = accelType;

    if (m_device->vkd()->vkCreateAccelerationStructureKHR(m_device->handle(), &accelCreateInfo, nullptr, &accelStructureRef) != VK_SUCCESS) {
      throw DxvkError(str::format(
        "DxvkAccelStructure: Failed to create acceleration structure:"
        "\n  size:  ", accelCreateInfo.size,
        "\n  type: ", accelType));
    }
  }

  DxvkAccelStructure::~DxvkAccelStructure() {
    if (accelStructureRef != VK_NULL_HANDLE) {
      const auto& vkd = m_device->vkd();
      vkd->vkDestroyAccelerationStructureKHR(m_device->handle(), accelStructureRef, nullptr);
    }
  }

  VkDeviceAddress DxvkAccelStructure::getAccelDeviceAddress() const {
    VkAccelerationStructureDeviceAddressInfoKHR deviceAddressInfo {};
    deviceAddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    deviceAddressInfo.accelerationStructure = accelStructureRef;
    VkDeviceAddress deviceAddress = m_device->vkd()->vkGetAccelerationStructureDeviceAddressKHR(m_device->handle(), &deviceAddressInfo);
    deviceAddress += getDynamicOffset(0);
    return deviceAddress;
  }
  // NV-DXVK end

  DxvkBufferTracker:: DxvkBufferTracker() { }
  DxvkBufferTracker::~DxvkBufferTracker() { }
  
  
  void DxvkBufferTracker::reset() {
    std::sort(m_entries.begin(), m_entries.end(),
      [] (const Entry& a, const Entry& b) {
        return a.slice.handle < b.slice.handle;
      });

    for (const auto& e : m_entries)
      e.buffer->freeSlice(e.slice);
      
    m_entries.clear();
  }
  
}
