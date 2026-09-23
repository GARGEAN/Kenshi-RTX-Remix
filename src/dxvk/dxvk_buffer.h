/*
* Copyright (c) 2021-2024, NVIDIA CORPORATION. All rights reserved.
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
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "dxvk_descriptor.h"
#include "dxvk_format.h"
#include "dxvk_hash.h"
#include "dxvk_memory.h"
#include "dxvk_resource.h"
#include "dxvk_memory_tracker.h"

namespace dxvk {

  /**
   * \brief GPU virtual address registry
   *
   * DX11_V319_FAULT_ADDRESS_NAMING: VK_EXT_device_fault reports the GPU virtual
   * addresses involved in a fault, but a bare address names nothing. A real
   * report from Little Nightmares II read:
   *
   *   [device-fault] GPU fault report: ''
   *   [device-fault]   address[0]: type=instruction-pointer-fault gpuVA=0xf00b50800
   *   [device-fault]   address[1]: type=read-invalid gpuVA=0x111ee00000 precision=4096
   *
   * which is unactionable: nothing in the process could say what lived at
   * 0x111ee00000. This registry keeps the address range of every buffer whose
   * device address has been taken, so the fault handler can resolve a faulting
   * address to the allocation that owns it - and, just as usefully, report
   * "no live allocation" when the address belongs to something already freed,
   * which is the signature of a lifetime bug.
   *
   * Only buffers someone actually asked the device address of are tracked.
   * That is deliberate: those are exactly the buffers reachable by the GPU
   * through a raw pointer - acceleration-structure build inputs above all -
   * which is the population a page fault can come from. Descriptor-bound
   * resources cannot fault this way.
   */
  class DxvkGpuAddressRegistry {

  public:

    static DxvkGpuAddressRegistry& get();

    /// Tracking is off until a device with VK_EXT_device_fault enables it;
    /// without that extension the map could never be read and is pure cost.
    void setEnabled(bool enabled) {
      m_enabled.store(enabled, std::memory_order_relaxed);
    }

    bool isEnabled() const {
      return m_enabled.load(std::memory_order_relaxed);
    }

    void add(VkDeviceAddress address, VkDeviceSize size, const char* name);
    void remove(VkDeviceAddress address);

    /**
     * \brief Describes the allocation containing an address
     *
     * \returns "<name> (base+offset, size)" for a live allocation, or a
     *          statement that no live allocation covers it.
     */
    std::string describe(VkDeviceAddress address) const;

  private:

    struct Entry {
      VkDeviceSize size = 0;
      std::string  name;
    };

    // Keep the last kFreedHistory removals so describe() can name the object a fault address falls in and
    // say how long ago it was freed. Bounded, only allocated when tracking is enabled (VK_EXT_device_fault),
    // and read only from the fault path.
    struct FreedEntry {
      VkDeviceAddress base   = 0;
      VkDeviceSize    size   = 0;
      uint64_t        serial = 0;
      std::string     name;
    };

    static constexpr size_t kFreedHistory = 4096;

    /// Caller must hold m_mutex. Empty string when no freed range covers it.
    std::string describeFreedLocked(VkDeviceAddress address) const;

    mutable dxvk::mutex               m_mutex;
    std::map<VkDeviceAddress, Entry>  m_entries;
    std::vector<FreedEntry>           m_freed;
    size_t                            m_freedNext = 0;
    uint64_t                          m_freedSerial = 0;
    std::atomic<bool>                 m_enabled = { false };

  };

  /**
   * \brief Buffer create info
   * 
   * The properties of a buffer that are
   * passed to \ref DxvkDevice::createBuffer
   */
  struct DxvkBufferCreateInfo {
    /// Size of the buffer, in bytes
    VkDeviceSize size;
    
    /// Buffer usage flags
    VkBufferUsageFlags usage;
    
    /// Pipeline stages that can access
    /// the contents of the buffer.
    VkPipelineStageFlags stages;
    
    /// Allowed access patterns
    VkAccessFlags access;

    // NV-DXVK start: Add additional alignment requirements for the Buffer's allocation.
    /// The required alignment the buffer should be allocated with. Note this will potentially
    /// increase the alignment over the memory requirements of the buffer which may be detrimental
    /// to some types of allocations (as it may waste more space), but this alignment may be nessecary
    /// when the buffer's usage/stages/access flags do not ensure an alignment in the Vulkan specification
    /// for an intended use case. This alignment must be within the maximum alignment any Vulkan object
    /// is required to be aligned to though, so do not use for any alignments other than things specified
    /// by the specification.
    VkDeviceSize requiredAlignmentOverride = 1;
    // NV-DXVK end

    // Shared handle info
    DxvkSharedHandleInfo sharing;
  };
  
  
  /**
   * \brief Buffer view create info
   * 
   * The properties of a buffer view that
   * are to \ref DxvkDevice::createBufferView
   */
  struct DxvkBufferViewCreateInfo {
    /// Buffer data format, like image data
    VkFormat format;
    
    /// Offset of the buffer region to include in the view
    VkDeviceSize rangeOffset;
    
    /// Size of the buffer region to include in the view
    VkDeviceSize rangeLength;
  };


  /**
   * \brief Buffer info
   * 
   * Stores a Vulkan buffer handle and the
   * memory object that is bound to the buffer.
   */
  struct DxvkBufferHandle {
    VkBuffer      buffer = VK_NULL_HANDLE;
    DxvkMemory    memory;
  };
  

  /**
   * \brief Buffer slice info
   * 
   * Stores the Vulkan buffer handle, offset
   * and length of the slice, and a pointer
   * to the mapped region..
   */
  struct DxvkBufferSliceHandle {
    VkBuffer      handle = VK_NULL_HANDLE;
    VkDeviceSize  offset = 0;
    VkDeviceSize  length = VK_WHOLE_SIZE;
    void*         mapPtr;

    bool operator==(DxvkBufferSliceHandle const& rhs) const {
      return eq(rhs);
    }

    bool eq(const DxvkBufferSliceHandle& other) const {
      return handle == other.handle
          && offset == other.offset
          && length == other.length;
    }

    size_t hash() const {
      DxvkHashState result;
      result.add(std::hash<VkBuffer>()(handle));
      result.add(std::hash<VkDeviceSize>()(offset));
      result.add(std::hash<VkDeviceSize>()(length));
      return result;
    }
  };

  /**
   * \brief Virtual buffer resource
   * 
   * A simple buffer resource that stores linear,
   * unformatted data. Can be accessed by the host
   * if allocated on an appropriate memory type.
   */
  class DxvkBuffer : public DxvkResource {
    friend class DxvkBufferView;
  public:
    
    DxvkBuffer(
            DxvkDevice*           device,
      const DxvkBufferCreateInfo& createInfo,
            DxvkMemoryAllocator&  memAlloc,
            VkMemoryPropertyFlags memFlags,
            DxvkMemoryStats::Category category,
            const char* name);
    
    ~DxvkBuffer();
    
    /**
     * \brief Buffer properties
     * \returns Buffer properties
     */
    const DxvkBufferCreateInfo& info() const {
      return m_info;
    }
    
    /**
     * \brief Memory type flags
     * 
     * Use this to determine whether a
     * buffer is mapped to host memory.
     * \returns Vulkan memory flags
     */
    VkMemoryPropertyFlags memFlags() const {
      return m_memFlags;
    }
    
    /**
     * \brief Map pointer
     * 
     * If the buffer has been created on a host-visible
     * memory type, the buffer memory is mapped and can
     * be accessed by the host.
     * \param [in] offset Byte offset into mapped region
     * \returns Pointer to mapped memory region
     */
    void* mapPtr(VkDeviceSize offset) const {
      // NV-DXVK start:
      return (m_physSlice.mapPtr != nullptr) ? reinterpret_cast<char*>(m_physSlice.mapPtr) + offset : nullptr;
      // NV-DXVK end
    }
    
    /**
     * \brief Retrieves slice handle
     * \returns Buffer slice handle
     */
    DxvkBufferSliceHandle getSliceHandle() const {
      return m_physSlice;
    }

    /**
     * \brief Retrieves sub slice handle
     * 
     * \param [in] offset Offset into buffer
     * \param [in] length Sub slice length
     * \returns Buffer slice handle
     */
    DxvkBufferSliceHandle getSliceHandle(VkDeviceSize offset, VkDeviceSize length) const {
      DxvkBufferSliceHandle result;
      result.handle = m_physSlice.handle;
      result.offset = m_physSlice.offset + offset;
      result.length = length;
      result.mapPtr = mapPtr(offset);
      return result;
    }

    /**
     * \brief Retrieves descriptor info
     * 
     * \param [in] offset Buffer slice offset
     * \param [in] length Buffer slice length
     * \returns Buffer slice descriptor
     */
    DxvkDescriptorInfo getDescriptor(VkDeviceSize offset, VkDeviceSize length) const {
      DxvkDescriptorInfo result;
      result.buffer.buffer = m_physSlice.handle;
      result.buffer.offset = m_physSlice.offset + offset;
      result.buffer.range  = length;
      return result;
    }

    /**
     * \brief Retrieves dynamic offset
     * 
     * \param [in] offset Offset into the buffer
     * \returns Offset for dynamic descriptors
     */
    VkDeviceSize getDynamicOffset(VkDeviceSize offset) const {
      return m_physSlice.offset + offset;
    }
    
    /**
     * \brief Replaces backing resource
     * 
     * Replaces the underlying buffer and implicitly marks
     * any buffer views using this resource as dirty. Do
     * not call this directly as this is called implicitly
     * by the context's \c invalidateBuffer method.
     * \param [in] slice The new backing resource
     * \returns Previous buffer slice
     */
    DxvkBufferSliceHandle rename(const DxvkBufferSliceHandle& slice) {
      // DX11_V319_FAULT_ADDRESS_NAMING: the old range stops belonging to this
      // buffer here, so drop it before the address is forgotten. Leaving it
      // registered would let a later fault be blamed on a buffer that no longer
      // owns that memory - worse than no name at all.
      DxvkGpuAddressRegistry::get().remove(m_deviceAddress);

      // Our cached device address may no longer be valid
      m_deviceAddress = 0;

      return std::exchange(m_physSlice, slice);
    }
    
    /**
     * \brief Transform feedback vertex stride
     * 
     * Used when drawing after transform feedback,
     * \returns The current xfb vertex stride
     */
    uint32_t getXfbVertexStride() const {
      return m_vertexStride;
    }
    
    /**
     * \brief Set transform feedback vertex stride
     * 
     * When the buffer is used as a transform feedback
     * buffer, this will be set to the vertex stride
     * defined by the geometry shader.
     * \param [in] stride Vertex stride
     */
    void setXfbVertexStride(uint32_t stride) {
      m_vertexStride = stride;
    }
    
    /**
     * \brief Allocates new buffer slice
     * \returns The new buffer slice
     */
    DxvkBufferSliceHandle allocSlice() {
      std::unique_lock<sync::Spinlock> freeLock(m_freeMutex);
      
      // If no slices are available, swap the two free lists.
      if (unlikely(m_freeSlices.empty())) {
        std::unique_lock<sync::Spinlock> swapLock(m_swapMutex);
        std::swap(m_freeSlices, m_nextSlices);
      }

      // If there are still no slices available, create a new
      // backing buffer and add all slices to the free list.
      if (unlikely(m_freeSlices.empty())) {
        if (likely(!m_lazyAlloc)) {
          DxvkBufferHandle handle = allocBuffer(m_physSliceCount, m_category);

          for (uint32_t i = 0; i < m_physSliceCount; i++)
            pushSlice(handle, i);

          m_buffers.push_back(std::move(handle));
          m_physSliceCount = std::min(m_physSliceCount * 2, m_physSliceMaxCount);

          // NV-DXVK start: Implement memory profiler
          m_tracker.updateSize(256 * m_physSliceCount); // 256 is the byte size of a slice according to DxvkBuffer
          // NV-DXVK end
        } else {
          for (uint32_t i = 1; i < m_physSliceCount; i++)
            pushSlice(m_buffer, i);

          m_lazyAlloc = false;
        }
      }
      
      // Take the first slice from the queue
      DxvkBufferSliceHandle result = m_freeSlices.back();
      m_freeSlices.pop_back();
      return result;
    }
    
    /**
     * \brief Frees a buffer slice
     * 
     * Marks the slice as free so that it can be used for
     * subsequent allocations. Called automatically when
     * the slice is no longer needed by the GPU.
     * \param [in] slice The buffer slice to free
     */
    void freeSlice(const DxvkBufferSliceHandle& slice) {
      // Add slice to a separate free list to reduce lock contention.
      std::unique_lock<sync::Spinlock> swapLock(m_swapMutex);
      m_nextSlices.push_back(slice);
    }

    VkBuffer getBufferRaw() {
      return m_physSlice.handle;
    }

    VkDeviceAddress getDeviceAddress();

    // NV-DXVK start: buffer clones for orphaned slices
    /**
     * \brief Creates a clone of the buffer
     *
     * Clones may be used in rendering like normal buffers but must NOT
     * be used to allocate slices since they do not own memory and
     * actual buffer objects.
     *
     * Note: do NOT use unless you know exactly what this method does!
     */
    Rc<DxvkBuffer> clone();
    // NV-DXVK end


    // NV-DXVK start: get access to VkDeviceMemory
    const DxvkBufferHandle& getBufferHandle() { 
      return m_buffer;
    }
    // NV-DXVK end

  protected:
    DxvkDevice*             m_device;
    DxvkBufferCreateInfo    m_info;
    DxvkMemoryAllocator*    m_memAlloc;
    VkMemoryPropertyFlags   m_memFlags;
    
    DxvkBufferHandle        m_buffer;
    DxvkBufferSliceHandle   m_physSlice;
    VkDeviceAddress         m_deviceAddress = 0;

    uint32_t                m_vertexStride = 0;

    alignas(CACHE_LINE_SIZE)
    sync::Spinlock          m_freeMutex;

    uint32_t                m_lazyAlloc = false;
    VkDeviceSize            m_physSliceLength   = 0;
    VkDeviceSize            m_physSliceStride   = 0;
    VkDeviceSize            m_physSliceCount    = 1;
    VkDeviceSize            m_physSliceMaxCount = 1;

    std::vector<DxvkBufferHandle>       m_buffers;
    std::vector<DxvkBufferSliceHandle>  m_freeSlices;

    alignas(CACHE_LINE_SIZE)
    sync::Spinlock                      m_swapMutex;
    std::vector<DxvkBufferSliceHandle>  m_nextSlices;

    DxvkMemoryStats::Category m_category;
    GpuMemoryTracker m_tracker;
    // DX11_V319_FAULT_ADDRESS_NAMING: GpuMemoryTracker only keeps the name when
    // rtx.profiler.memory.enable is set, which is exactly the gap that left a
    // faulting address unattributable. Keep an independent copy so a GPU fault
    // can be named without asking the user to relaunch with the profiler on.
    // Populated only while the address registry is active, so a build that can
    // never read it pays nothing.
    std::string m_gpuAddressName;

    void pushSlice(const DxvkBufferHandle& handle, uint32_t index) {
      DxvkBufferSliceHandle slice;
      slice.handle = handle.buffer;
      slice.length = m_physSliceLength;
      slice.offset = m_physSliceStride * index;
      slice.mapPtr = handle.memory.mapPtr(slice.offset);
      m_freeSlices.push_back(slice);
    }

    DxvkBufferHandle allocBuffer(
      VkDeviceSize sliceCount, DxvkMemoryStats::Category category) const;

    VkDeviceSize computeSliceAlignment() const;

  // NV-DXVK start: stable content cookie for the DX11 bridge
  public:
    /**
     * \brief Content-derived identity for geometry hashing
     *
     * Set at creation time from a buffer's initial data so the DX11 bridge
     * can hash geometry in buffers whose memory the CPU cannot read (static
     * DEVICE_LOCAL meshes). Stable across runs and across GPU vendors,
     * unlike the pointer-based fallback. Zero means unset.
     */
    uint64_t contentCookie() const {
      return m_contentCookie.load(std::memory_order_relaxed);
    }

    void setContentCookie(uint64_t cookie) {
      m_contentCookie.store(cookie, std::memory_order_relaxed);
    }

  private:
    std::atomic<uint64_t> m_contentCookie = { 0ull };
  // NV-DXVK end

  // NV-DXVK start: buffer clones for orphaned slices
  private:
    /**
     * \brief Clonning constructor
     *
     * To be used ONLY for cloning.
     */ 
    DxvkBuffer(DxvkBuffer& parent);

    /**
     * \brief Parent buffer
     *
     * When buffer is a clone, parent member
     * will be referencing the parent buffer object.
     * Parent is a nullptr otherwise.
     */
    Rc<DxvkBuffer> m_parent;
  // NV-DXVK end
  };
  
  
  /**
   * \brief Buffer slice
   * 
   * Stores the buffer and a sub-range of the buffer.
   * Slices are considered equal if the buffer and
   * the buffer range are the same.
   */
  class DxvkBufferSlice {
    
  public:
    
    DxvkBufferSlice() { }
    
    DxvkBufferSlice(
      const Rc<DxvkBuffer>& buffer,
            VkDeviceSize    rangeOffset,
            VkDeviceSize    rangeLength)
    : m_buffer(buffer),
      m_offset(rangeOffset),
      m_length(rangeLength) { }
    
    explicit DxvkBufferSlice(const Rc<DxvkBuffer>& buffer)
    : DxvkBufferSlice(buffer, 0, buffer->info().size) { }

    DxvkBufferSlice(const DxvkBufferSlice& ) = default;
    DxvkBufferSlice(      DxvkBufferSlice&&) = default;

    DxvkBufferSlice& operator = (const DxvkBufferSlice& other) {
      if (m_buffer != other.m_buffer)
        m_buffer = other.m_buffer;
      m_offset = other.m_offset;
      m_length = other.m_length;
      return *this;
    }

    DxvkBufferSlice& operator = (DxvkBufferSlice&&) = default;

    /**
     * \brief Buffer slice offset and length
     * \returns Buffer slice offset and length
     */
    size_t offset() const { return m_offset; }
    size_t length() const { return m_length; }

    /**
     * \brief Underlying buffer
     * \returns The virtual buffer
     */
    const Rc<DxvkBuffer>& buffer() const {
      return m_buffer;
    }
    
    /**
     * \brief Buffer info
     * 
     * Retrieves the properties of the underlying
     * virtual buffer. Should not be used directly
     * by client APIs.
     * \returns Buffer properties
     */
    const DxvkBufferCreateInfo& bufferInfo() const {
      return m_buffer->info();
    }
    
    /**
     * \brief Buffer sub slice
     * 
     * Takes a sub slice from this slice.
     * \param [in] offset Sub slice offset
     * \param [in] length Sub slice length
     * \returns The sub slice object
     */
    DxvkBufferSlice subSlice(VkDeviceSize offset, VkDeviceSize length) const {
      return DxvkBufferSlice(m_buffer, m_offset + offset, length);
    }
    
    /**
     * \brief Checks whether the slice is valid
     * 
     * A buffer slice that does not point to any virtual
     * buffer object is considered undefined and cannot
     * be used for any operations.
     * \returns \c true if the slice is defined
     */
    bool defined() const {
      return m_buffer != nullptr;
    }
    
    /**
     * \brief Retrieves buffer slice handle
     * 
     * Returns the buffer handle and offset
     * \returns Buffer slice handle
     */
    DxvkBufferSliceHandle getSliceHandle() const {
      return m_buffer != nullptr
        ? m_buffer->getSliceHandle(m_offset, m_length)
        : DxvkBufferSliceHandle();
    }

    /**
     * \brief Retrieves sub slice handle
     * 
     * \param [in] offset Offset into buffer
     * \param [in] length Sub slice length
     * \returns Buffer slice handle
     */
    DxvkBufferSliceHandle getSliceHandle(VkDeviceSize offset, VkDeviceSize length) const {
      return m_buffer != nullptr
        ? m_buffer->getSliceHandle(m_offset + offset, length)
        : DxvkBufferSliceHandle();
    }

    /**
     * \brief Retrieves descriptor info
     * \returns Buffer slice descriptor
     */
    DxvkDescriptorInfo getDescriptor() const {
      return m_buffer->getDescriptor(m_offset, m_length);
    }

    /**
     * \brief Retrieves dynamic offset
     * 
     * Used for descriptor set binding.
     * \returns Buffer slice offset
     */
    VkDeviceSize getDynamicOffset() const {
      return m_buffer->getDynamicOffset(m_offset);
    }
    
    /**
     * \brief Pointer to mapped memory region
     * 
     * \param [in] offset Offset into the slice
     * \returns Pointer into mapped buffer memory
     */
    void* mapPtr(VkDeviceSize offset) const  {
      return m_buffer != nullptr
        ? m_buffer->mapPtr(m_offset + offset)
        : nullptr;
    }

    /**
     * \brief Checks whether two slices are equal
     * 
     * Two slices are considered equal if they point to
     * the same memory region within the same buffer.
     * \param [in] other The slice to compare to
     * \returns \c true if the two slices are the same
     */
    bool matches(const DxvkBufferSlice& other) const {
      return this->m_buffer == other.m_buffer
          && this->m_offset == other.m_offset
          && this->m_length == other.m_length;
    }

    /**
     * \brief Checks whether two slices are from the same buffer
     *
     * This returns \c true if the two slices are taken
     * from the same buffer, but may have different ranges.
     * \param [in] other The slice to compare to
     * \returns \c true if the buffer objects are the same
     */
    bool matchesBuffer(const DxvkBufferSlice& other) const {
      return this->m_buffer == other.m_buffer;
    }

    /**
     * \brief Checks whether two slices have the same range
     * 
     * This returns \c true if the two slices have the same
     * offset and size, even if the buffers are different.
     * May be useful if the buffers are know to be the same.
     * \param [in] other The slice to compare to
     * \returns \c true if the buffer objects are the same
     */
    bool matchesRange(const DxvkBufferSlice& other) const {
      return this->m_offset == other.m_offset
          && this->m_length == other.m_length;
    }

    VkDeviceAddress getDeviceAddress() const {
      return m_buffer->getDeviceAddress() + m_offset;
    }

  private:
    
    Rc<DxvkBuffer> m_buffer = nullptr;
    VkDeviceSize   m_offset = 0;
    VkDeviceSize   m_length = 0;
  };
  
  
  /**
   * \brief Buffer view
   * 
   * Allows the application to interpret buffer
   * contents like formatted pixel data. These
   * buffer views are used as texel buffers.
   */
  class DxvkBufferView : public DxvkResource {
    
  public:
    
    DxvkBufferView(
      const Rc<vk::DeviceFn>&         vkd,
      const Rc<DxvkBuffer>&           buffer,
      const DxvkBufferViewCreateInfo& info);
    
    ~DxvkBufferView();
    
    /**
     * \brief Buffer view handle
     * \returns Buffer view handle
     */
    VkBufferView handle() const {
      return m_bufferView;
    }
    
    /**
     * \brief Element cound
     * 
     * Number of typed elements contained
     * in the buffer view. Depends on the
     * buffer view format.
     * \returns Element count
     */
    VkDeviceSize elementCount() const {
      auto format = imageFormatInfo(m_info.format);
      return m_info.rangeLength / format->elementSize;
    }
    
    /**
     * \brief Buffer view properties
     * \returns Buffer view properties
     */
    const DxvkBufferViewCreateInfo& info() const {
      return m_info;
    }
    
    /**
     * \brief Underlying buffer object
     * \returns Underlying buffer object
     */
    const Rc<DxvkBuffer>& buffer() const {
      return m_buffer;
    }
    
    /**
     * \brief Underlying buffer info
     * \returns Underlying buffer info
     */
    const DxvkBufferCreateInfo& bufferInfo() const {
      return m_buffer->info();
    }
    
    /**
     * \brief View format info
     * \returns View format info
     */
    const DxvkFormatInfo* formatInfo() const {
      return imageFormatInfo(m_info.format);
    }

    /**
     * \brief Retrieves buffer slice handle
     * \returns Buffer slice handle
     */
    DxvkBufferSliceHandle getSliceHandle() const {
      return m_buffer->getSliceHandle(
        m_info.rangeOffset,
        m_info.rangeLength);
    }
    
    /**
     * \brief Underlying buffer slice
     * \returns Slice backing the view
     */
    DxvkBufferSlice slice() const {
      return DxvkBufferSlice(m_buffer,
        m_info.rangeOffset,
        m_info.rangeLength);
    }
    
    /**
     * \brief Updates the buffer view
     * 
     * If the buffer has been invalidated ever since
     * the view was created, the view is invalid as
     * well and needs to be re-created. Call this
     * prior to using the buffer view handle.
     */
    void updateView() {
      DxvkBufferSliceHandle slice = getSliceHandle();

      if (!m_bufferSlice.eq(slice))
        this->updateBufferView(slice);
    }
    
  private:
    
    Rc<vk::DeviceFn>          m_vkd;
    DxvkBufferViewCreateInfo  m_info;
    Rc<DxvkBuffer>            m_buffer;

    DxvkBufferSliceHandle     m_bufferSlice;
    VkBufferView              m_bufferView;

    std::unordered_map<
      DxvkBufferSliceHandle,
      VkBufferView,
      DxvkHash, DxvkEq> m_views;
    
    VkBufferView createBufferView(
      const DxvkBufferSliceHandle& slice);
    
    void updateBufferView(
      const DxvkBufferSliceHandle& slice);
    
  };
  

  // NV-DXVK start: implement acceleration structures
  class DxvkAccelStructure : public DxvkBuffer {
    VkAccelerationStructureKHR accelStructureRef = VK_NULL_HANDLE;

  public:
    DxvkAccelStructure(
            DxvkDevice* device,
      const DxvkBufferCreateInfo& createInfo,
            DxvkMemoryAllocator& memAlloc,
            VkMemoryPropertyFlags memFlags,
            VkAccelerationStructureTypeKHR accelType,
      const char* name);

    ~DxvkAccelStructure();

    const VkAccelerationStructureKHR& getAccelStructure() const {
      return accelStructureRef;
    }

    VkDeviceAddress getAccelDeviceAddress() const;
  };
  // NV-DXVK end


  /**
   * \brief Buffer slice tracker
   * 
   * Stores a list of buffer slices that can be
   * freed. Useful when buffers have been renamed
   * and the original slice is no longer needed.
   */
  class DxvkBufferTracker {
    
  public:
    
    DxvkBufferTracker();
    ~DxvkBufferTracker();
    
    /**
     * \brief Add buffer slice for tracking
     *
     * The slice will be returned to the
     * buffer on the next call to \c reset.
     * \param [in] buffer The parent buffer
     * \param [in] slice The buffer slice
     */
    void freeBufferSlice(const Rc<DxvkBuffer>& buffer, const DxvkBufferSliceHandle& slice) {
      m_entries.push_back({ buffer, slice });
    }
    
    /**
     * \brief Returns tracked buffer slices
     */
    void reset();
    
  private:
    
    struct Entry {
      Rc<DxvkBuffer>        buffer;
      DxvkBufferSliceHandle slice;
    };
    
    std::vector<Entry> m_entries;
    
  };
  
}
