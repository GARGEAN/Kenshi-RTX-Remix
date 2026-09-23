#pragma once

#include <string>
#include <vector>

#include "dxbc_common.h"
#include "dxbc_reader.h"

namespace dxvk {

  /**
   * \brief Shader input resource type
   *
   * Mirrors D3D_SHADER_INPUT_TYPE. Only the values the capture layer
   * distinguishes are named; anything else is reported numerically.
   */
  enum class DxbcResourceKind : uint32_t {
    CBuffer         = 0,
    TBuffer         = 1,
    Texture         = 2,
    Sampler         = 3,
    UavRwTyped      = 4,
    Structured      = 5,
    UavRwStructured = 6,
    ByteAddress     = 7,
    UavRwByteAddress = 8,
  };

  /**
   * \brief A resource bound by the shader
   *
   * One entry of the RDEF bound-resource table: the HLSL-level name the
   * shader author gave the resource, plus where it binds.
   */
  struct DxbcResourceBinding {
    std::string      name;
    DxbcResourceKind kind      = DxbcResourceKind::Texture;
    uint32_t         bindPoint = 0;
    uint32_t         bindCount = 0;
    uint32_t         space     = 0;
  };

  /**
   * \brief A variable inside a constant buffer
   *
   * Byte offset and size are relative to the start of the owning buffer,
   * which is what the capture layer needs to hash a named constant range.
   */
  struct DxbcConstantVariable {
    std::string name;
    uint32_t    offset = 0;
    uint32_t    size   = 0;
    bool        used   = false;
  };

  /**
   * \brief A constant buffer declared by the shader
   */
  struct DxbcConstantBufferInfo {
    std::string                       name;
    uint32_t                          size = 0;
    std::vector<DxbcConstantVariable> variables;
  };

  /**
   * \brief Resource definition chunk (RDEF)
   *
   * Carries the reflection data D3D11 shaders embed: the names of bound
   * textures/samplers and of every constant-buffer variable. The capture
   * layer uses these names to tell material inputs apart from engine-wide
   * ones, which is what lets a material instance be identified by the
   * texture set and constants it actually overrides.
   *
   * This is the D3D11 counterpart of the constant table older APIs embedded.
   * Parsing is best-effort: a malformed or truncated chunk yields an empty
   * (but valid) object rather than throwing, since reflection data is
   * optional and may be stripped from shipped shaders.
   */
  class DxbcRdef : public RcObject {

  public:

    explicit DxbcRdef(DxbcReader reader);
    ~DxbcRdef();

    const std::vector<DxbcResourceBinding>& resourceBindings() const {
      return m_bindings;
    }

    const std::vector<DxbcConstantBufferInfo>& constantBuffers() const {
      return m_constantBuffers;
    }

    /**
     * \brief Finds a bound resource by bind point and kind
     * \returns The binding, or nullptr when the slot is not declared
     */
    const DxbcResourceBinding* findBinding(
            DxbcResourceKind kind,
            uint32_t         bindPoint) const;

    /**
     * \brief Whether the chunk carried usable reflection data
     */
    bool isValid() const {
      return m_valid;
    }

  private:

    std::vector<DxbcResourceBinding>    m_bindings;
    std::vector<DxbcConstantBufferInfo> m_constantBuffers;
    bool                                m_valid = false;

  };

}
