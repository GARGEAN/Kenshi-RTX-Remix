#pragma once

#include <atomic>

#include "d3d11_buffer.h"
#include "d3d11_texture.h"

namespace dxvk {

  class D3D11Device;

  /**
   * \brief Resource initialization context
   * 
   * Manages a context which is used for resource
   * initialization. This includes initialization
   * with application-defined data, as well as
   * zero-initialization for buffers and images.
   */
  class D3D11Initializer {
    constexpr static size_t MaxTransferMemory    = 128 * 1024 * 1024;
    constexpr static size_t MaxTransferCommands  = 2048;
  public:

    D3D11Initializer(
            D3D11Device*                pParent);
    
    ~D3D11Initializer();

    void Flush();
    bool HasPendingWork() const;

    void InitBuffer(
            D3D11Buffer*                pBuffer,
      const D3D11_SUBRESOURCE_DATA*     pInitialData);
    
    void InitTexture(
            D3D11CommonTexture*         pTexture,
      const D3D11_SUBRESOURCE_DATA*     pInitialData);

    void InitUavCounter(
            D3D11UnorderedAccessView*   pUav);

    // DX11_V294_STABLE_HASH_SLOTS: same-descriptor slot allocator for the
    // dynamic-texture identity ordinal. Slots are recycled at texture
    // destruction so recreated atlases keep their hash across recreation.
    static uint32_t AcquireDynamicTextureSlot(uint64_t descriptorHash);
    static void ReleaseDynamicTextureSlot(uint64_t descriptorHash, uint32_t ordinal);

  private:

    dxvk::mutex       m_mutex;

    D3D11Device*      m_parent;
    Rc<DxvkDevice>    m_device;
    Rc<DxvkContext>   m_context;

    size_t            m_transferCommands  = 0;
    size_t            m_transferMemory    = 0;
    std::atomic<bool> m_hasPendingTransfers { false };

    void InitDeviceLocalBuffer(
            D3D11Buffer*                pBuffer,
      const D3D11_SUBRESOURCE_DATA*     pInitialData);

    void InitHostVisibleBuffer(
            D3D11Buffer*                pBuffer,
      const D3D11_SUBRESOURCE_DATA*     pInitialData);

    void InitDeviceLocalTexture(
            D3D11CommonTexture*         pTexture,
      const D3D11_SUBRESOURCE_DATA*     pInitialData);

    void InitHostVisibleTexture(
            D3D11CommonTexture*         pTexture,
      const D3D11_SUBRESOURCE_DATA*     pInitialData);

    // NV-DXVK start: DX11_V258_TEXTURE_HASH_STABILITY
    void InitTextureContentHash(
            D3D11CommonTexture*         pTexture,
      const D3D11_SUBRESOURCE_DATA*     pInitialData);
    // NV-DXVK end

    // NV-DXVK start: DX11_V288_STABLE_DYNAMIC_TEXTURE_HASH
    void InitDynamicTextureStableHash(
            D3D11CommonTexture*         pTexture);
    // NV-DXVK end

    void FlushImplicit();
    void FlushInternal();

  };

}
