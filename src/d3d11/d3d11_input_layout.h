#pragma once

#include "d3d11_device_child.h"

#include "../dxbc/dxbc_enums.h"

#include <vector>
#include <cstring>

namespace dxvk {
  
  class D3D11Device;

  // Per-element descriptor stored alongside vertex attribute data for RTX geometry routing.
  struct D3D11RtxSemantic {
    char     name[32];   // Upper-cased semantic name (POSITION, NORMAL, TEXCOORD, COLOR)
    uint32_t index;      // Semantic index
    uint32_t registerId; // Vertex shader input register bound to this element
    uint32_t inputSlot;  // D3D11 vertex buffer input slot
    uint32_t byteOffset; // Byte offset within each vertex stride
    uint32_t componentCount; // Number of components declared in the shader signature
    VkFormat format;     // Translated Vulkan vertex format
    DxbcScalarType componentType = DxbcScalarType::Uint32;
    DxbcSystemValue systemValue = DxbcSystemValue::None;
    bool     perInstance; // true if D3D11_INPUT_PER_INSTANCE_DATA
  };

  class D3D11InputLayout : public D3D11DeviceChild<ID3D11InputLayout> {
    
  public:
    
    D3D11InputLayout(
            D3D11Device*          pDevice,
            uint32_t              numAttributes,
      const DxvkVertexAttribute*  pAttributes,
            uint32_t              numBindings,
      const DxvkVertexBinding*    pBindings);
    
    ~D3D11InputLayout();
    
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                riid,
            void**                ppvObject) final;
    
    void BindToContext(
      const Rc<DxvkContext>&      ctx);
    
    bool Compare(
      const D3D11InputLayout*     pOther) const;
    void SetRtxSemantics(std::vector<D3D11RtxSemantic>&& semantics) {
      m_rtxSemantics = std::move(semantics);
    }

    const std::vector<D3D11RtxSemantic>& GetRtxSemantics() const {
      return m_rtxSemantics;
    }
    
  private:
    
    std::vector<DxvkVertexAttribute> m_attributes;
    std::vector<DxvkVertexBinding>   m_bindings;
    std::vector<D3D11RtxSemantic>    m_rtxSemantics;
  };
  
}
