#include "../util/util_kenshi_telemetry.h"
#pragma once

#include "../dxvk/dxvk_cs.h"
#include "../dxvk/dxvk_device.h"

#include "d3d11_device_child.h"
#include "d3d11_interfaces.h"
#include "d3d11_resource.h"
#include "../util/util_kenshi_terrain_bounds.h"
#include "../util/util_kenshi_prepared_terrain.h"
#include <memory>

namespace dxvk {
  
  class D3D11Device;
  class D3D11DeviceContext;


  /**
   * \brief Buffer map mode
   */
  enum D3D11_COMMON_BUFFER_MAP_MODE {
    D3D11_COMMON_BUFFER_MAP_MODE_NONE,
    D3D11_COMMON_BUFFER_MAP_MODE_DIRECT,
  };


  /**
   * \brief Stream output buffer offset
   *
   * A byte offset into the buffer that
   * stores the byte offset where new
   * data will be written to.
   */
  struct D3D11SOCounter {
    uint32_t byteOffset;
  };
  
  
  class D3D11Buffer : public D3D11DeviceChild<ID3D11Buffer> {
    static constexpr VkDeviceSize BufferSliceAlignment = 64;
  public:
    
    D3D11Buffer(
            D3D11Device*                pDevice,
      const D3D11_BUFFER_DESC*          pDesc);
    ~D3D11Buffer();
    
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void**  ppvObject) final;
    
    void STDMETHODCALLTYPE GetType(
            D3D11_RESOURCE_DIMENSION *pResourceDimension) final;
    
    UINT STDMETHODCALLTYPE GetEvictionPriority() final;
    
    void STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) final;
    
    void STDMETHODCALLTYPE GetDesc(
            D3D11_BUFFER_DESC *pDesc) final;
    
    bool CheckViewCompatibility(
            UINT                BindFlags,
            DXGI_FORMAT         Format) const;

    const D3D11_BUFFER_DESC* Desc() const {
      return &m_desc;
    }

    D3D11_COMMON_BUFFER_MAP_MODE GetMapMode() const {
      return m_mapMode;
    }

    Rc<DxvkBuffer> GetBuffer() const {
      return m_buffer;
    }
    
    DxvkBufferSlice GetBufferSlice() const {
      return DxvkBufferSlice(m_buffer, 0, m_desc.ByteWidth);
    }
    
    DxvkBufferSlice GetBufferSlice(VkDeviceSize offset) const {
      VkDeviceSize size = m_desc.ByteWidth;

      return likely(offset < size)
        ? DxvkBufferSlice(m_buffer, offset, size - offset)
        : DxvkBufferSlice();
    }
    
    DxvkBufferSlice GetBufferSlice(VkDeviceSize offset, VkDeviceSize length) const {
      VkDeviceSize size = m_desc.ByteWidth;

      return likely(offset < size)
        ? DxvkBufferSlice(m_buffer, offset, std::min(length, size - offset))
        : DxvkBufferSlice();
    }

    DxvkBufferSlice GetSOCounter() {
      return m_soCounter != nullptr
        ? DxvkBufferSlice(m_soCounter)
        : DxvkBufferSlice();
    }
    
    DxvkBufferSliceHandle AllocSlice() {
      return m_buffer->allocSlice();
    }
    
    DxvkBufferSliceHandle DiscardSlice() {
      m_mapped = m_buffer->allocSlice();
      return m_mapped;
    }

    DxvkBufferSliceHandle GetMappedSlice() const {
      return m_mapped;
    }
    bool HasSequenceNumber() const {
      return m_mapMode != D3D11_COMMON_BUFFER_MAP_MODE_NONE
          && !(m_desc.MiscFlags & D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS)
          && !(m_desc.BindFlags);
    }

    void TrackSequenceNumber(uint64_t Seq) {
      m_seq = Seq;
    }

    uint64_t GetSequenceNumber() {
      return HasSequenceNumber() ? m_seq
        : DxvkCsThread::SynchronizeAll;
    }

    /**
     * \brief Normalizes buffer description
     * 
     * \param [in] pDesc Buffer description
     * \returns \c S_OK if the parameters are valid
     */
    static HRESULT NormalizeBufferProperties(
            D3D11_BUFFER_DESC*      pDesc);

  public:

    // DX11_V319_INDEX_SHADOW: CPU-readable copy of a static index buffer's
    // contents.
    //
    // The RT submit path must know the highest index a draw references, to size
    // the vertex range it copies. It reads the index data through
    // DxvkBuffer::mapPtr, which returns null for a DEVICE_LOCAL allocation - and
    // a static index buffer is always device-local. Without the maximum it falls
    // back to "assume the whole vertex buffer", which the exact-capture path
    // cannot flatten, so the draw is DROPPED: logged as
    // "skipped unsafe uncaptured draw: reason=gpu-index-flatten-required".
    // Dropped geometry is invisible, which is what "the geometry is transparent"
    // looks like in-game. Granny Chapter Two hit this on 100% of its indexed
    // draws (indexCpuVisible=0, wholeVbFallback=1, 48/48).
    //
    // The data is already in hand at creation time (it is hashed there for the
    // content cookie), so keeping it costs one copy of the index buffer and no
    // GPU readback, no stall, and no change to how the buffer is used.
    void SetIndexShadow(const void* data, size_t bytes) {
      m_indexShadow.resize(bytes);
      std::memcpy(m_indexShadow.data(), data, bytes);
      prepared_terrain::written(this);
    }

    // Index shadow for buffers created empty and filled later (OGRE allocates first, writes after).
    // Without it the maximum index is unknown, the vertex range falls back to the whole buffer, and
    // unvalidated indices reach BLAS triangle generation and fault the GPU.
    // Costs one memcpy of data already in system memory; no readback. The shadow is sized to the
    // whole buffer on first touch, so partial updates work and GetIndexShadow bounds-checks queries.
    void UpdateIndexShadow(size_t offset, const void* data, size_t bytes) {
      if (data == nullptr || bytes == 0)
        return;
      if (!(m_desc.BindFlags & D3D11_BIND_INDEX_BUFFER))
        return;

      // Same ceiling as the creation-time path, for the same reason: never
      // let a pathological buffer double its footprint in system memory.
      constexpr size_t kMaxIndexShadowBytes = 32ull << 20;
      const size_t bufferSize = m_desc.ByteWidth;
      if (bufferSize == 0 || bufferSize > kMaxIndexShadowBytes)
        return;
      if (offset > bufferSize || bytes > bufferSize - offset)
        return;

      if (m_indexShadow.size() < bufferSize)
        m_indexShadow.resize(bufferSize, 0);
      std::memcpy(m_indexShadow.data() + offset, data, bytes);
      prepared_terrain::written(this);

      // Diagnostic: log the buffer identity on store, to match against the one the submit path reads.
      static uint32_t sShadowStoreLogs = 0;
      if (bufferSize >= 4096 && sShadowStoreLogs < 32u) {
        ++sShadowStoreLogs;
        KENSHI_DIAGNOSTIC_INFO(dxvk::str::format(
          "[D3D11Buffer][index-shadow] stored: buffer=0x",
          std::hex, reinterpret_cast<uintptr_t>(this), std::dec,
          " width=", bufferSize, " offset=", offset, " bytes=", bytes,
          " shadowNow=", m_indexShadow.size()));
      }
    }

    // How many bytes of shadow exist, for diagnostics. Zero means this buffer
    // was never shadowed at all, which is a different failure from "shadowed
    // but the requested range falls outside it".
    size_t GetIndexShadowSize() const {
      return m_indexShadow.size();
    }

    // Returns nullptr when no shadow exists or the requested range is not fully
    // covered - callers must treat that exactly like an unreadable buffer.
    const void* GetIndexShadow(VkDeviceSize offset, VkDeviceSize bytes) const {
      if (m_indexShadow.empty() || bytes == 0)
        return nullptr;
      if (offset > m_indexShadow.size() || bytes > m_indexShadow.size() - offset)
        return nullptr;
      return m_indexShadow.data() + offset;
    }

    // Vertex shadow: the index-shadow mechanism for POSITION data. Its consumer is the object-space
    // bounding box anti-culling needs; Kenshi's vertex buffers are device-local, so posBuffer.mapPtr()
    // is null. Without a box, anti-culling cannot keep an object alive once the game stops drawing it.
    void UpdateVertexShadow(size_t offset, const void* data, size_t bytes) {
      if (data == nullptr || bytes == 0)
        return;
      if (!(m_desc.BindFlags & D3D11_BIND_VERTEX_BUFFER))
        return;

      // Same ceiling and reasoning as the index path.
      constexpr size_t kMaxVertexShadowBytes = 32ull << 20;
      const size_t bufferSize = m_desc.ByteWidth;
      if (bufferSize == 0 || bufferSize > kMaxVertexShadowBytes)
        return;
      if (offset > bufferSize || bytes > bufferSize - offset)
        return;

      if (m_vertexShadow.size() < bufferSize)
        m_vertexShadow.resize(bufferSize, 0);
      std::memcpy(m_vertexShadow.data() + offset, data, bytes);
      ++m_vertexShadowRevision;
      prepared_terrain::written(this);
    }

    const void* GetVertexShadow(size_t offset, size_t bytes) const {
      if (m_vertexShadow.empty() || bytes == 0)
        return nullptr;
      if (offset > m_vertexShadow.size() || bytes > m_vertexShadow.size() - offset)
        return nullptr;
      return m_vertexShadow.data() + offset;
    }

    size_t GetVertexShadowSize() const {
      return m_vertexShadow.size();
    }

    uint64_t GetVertexShadowRevision() const {
      return m_vertexShadowRevision;
    }

    terrain_bounds::Cache& GetKenshiTerrainBoundsCache() {
      if (!m_kenshiTerrainBoundsCache)
        m_kenshiTerrainBoundsCache = std::make_unique<terrain_bounds::Cache>();
      return *m_kenshiTerrainBoundsCache;
    }

    Rc<DxvkBuffer> FindKenshiBloodProjection(uint64_t key) const {
      for (const auto& entry : m_kenshiBloodProjectionCache) {
        if (entry.key == key)
          return entry.buffer;
      }
      return nullptr;
    }

    void StoreKenshiBloodProjection(uint64_t key, const Rc<DxvkBuffer>& buffer) {
      for (auto& entry : m_kenshiBloodProjectionCache) {
        if (entry.key == key) {
          entry.buffer = buffer;
          return;
        }
      }
      // Bounded: one entry per (buffer, draw range, layout, revision). Sub-meshes
      // each mint one, and a shadow update mints a fresh generation of all of
      // them, so this must not grow without limit. Oldest-out is right here -
      // a stale revision's entries are never looked up again.
      constexpr size_t kMaxKenshiBloodProjections = 96;
      if (m_kenshiBloodProjectionCache.size() >= kMaxKenshiBloodProjections)
        m_kenshiBloodProjectionCache.erase(m_kenshiBloodProjectionCache.begin());
      m_kenshiBloodProjectionCache.push_back({ key, buffer });
    }

  private:

    D3D11_BUFFER_DESC             m_desc;
    D3D11_COMMON_BUFFER_MAP_MODE  m_mapMode;
    std::vector<uint8_t>          m_indexShadow;

    // Kept for the life of the buffer: one buffer serves many draws at different offsets.
    std::vector<uint8_t>          m_vertexShadow;
    uint64_t                      m_vertexShadowRevision = 0;
    // Owned by the source buffer: no stale pointer identity or retained resource.
    std::unique_ptr<terrain_bounds::Cache> m_kenshiTerrainBoundsCache;

    struct KenshiBloodProjectionCacheEntry {
      uint64_t key;
      Rc<DxvkBuffer> buffer;
    };
    std::vector<KenshiBloodProjectionCacheEntry> m_kenshiBloodProjectionCache;


    Rc<DxvkBuffer>                m_buffer;
    Rc<DxvkBuffer>                m_soCounter;
    DxvkBufferSliceHandle         m_mapped;
    uint64_t                      m_seq = 0ull;

    D3D11DXGIResource             m_resource;
    BOOL CheckFormatFeatureSupport(
            VkFormat              Format,
            VkFormatFeatureFlags  Features) const;
    
    VkMemoryPropertyFlags GetMemoryFlags() const;

    Rc<DxvkBuffer> CreateSoCounterBuffer();

    D3D11_COMMON_BUFFER_MAP_MODE DetermineMapMode();

  };


  /**
   * \brief Retrieves buffer from resource pointer
   * 
   * \param [in] pResource The resource to query
   * \returns Pointer to buffer, or \c nullptr
   */
  D3D11Buffer* GetCommonBuffer(
          ID3D11Resource*       pResource);
  
}
