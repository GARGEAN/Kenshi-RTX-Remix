#pragma once

#include <memory>
#include <mutex>
#include <array>
#include <unordered_map>
#include <vector>

#include "../dxbc/dxbc_module.h"
#include "../dxvk/dxvk_device.h"

#include "../util/sha1/sha1_util.h"

#include "../util/util_env.h"

#include "d3d11_device_child.h"
#include "d3d11_interfaces.h"

namespace dxvk {

  class D3D11Device;

  // A common DXBC vertex-shader transform pattern writes SV_Position with
  // four dp4 instructions whose constant-buffer operands are the four rows or
  // columns of object-to-clip. Recording the exact registers avoids guessing
  // camera/world matrices from unrelated affine constants at draw time.
  struct D3D11PositionTransformMatrixBinding {
    uint32_t constantBufferSlot = 0;
    std::array<uint32_t, 4> constantRegisters = { 0, 0, 0, 0 };
  };

  struct D3D11PositionTransformBinding {
    bool valid = false;
    uint32_t matrixCount = 0;
    // Application order: matrices[0] consumes the original POSITION input;
    // matrices[1], when present, consumes matrices[0]'s homogeneous result.
    std::array<D3D11PositionTransformMatrixBinding, 2> matrices;
  };

  // Creation-time, shader-exact list of constant-buffer registers read by a
  // vertex shader. Optimized shaders often do not contain a recognizable
  // four-DP4 matrix chain, but their declared DXBC operands still tell us
  // precisely which constants can affect SV_Position. Hashing this profile at
  // draw time avoids replaying post-VS capture because unrelated bytes in a
  // large engine cbuffer changed. Dynamic indexing is represented by one
  // whole-buffer dependency for the affected slot.
  struct D3D11ConstantBufferDependency {
    uint32_t slot = 0;
    uint32_t constantRegister = 0;
    bool wholeBuffer = false;
  };

  struct D3D11ConstantBufferDependencyProfile {
    std::vector<D3D11ConstantBufferDependency> dependencies;
    bool complete = false;
  };

  // DX11_V280_TEXCOORD_CAPTURE: shared state for building a stream-output
  // "capture" variant of a game vertex shader on demand. Some engines carry no
  // TEXCOORD stream in the input layout - the UVs exist only as VERTEX SHADER
  // OUTPUTS (computed from other attributes, instance data, or constants). For
  // those draws the capture layer replays the vertex range through DXVK's
  // xfb passthrough pipeline (the same mechanism that backs D3D11
  // CreateGeometryShaderWithStreamOutput) and reads the VS's texcoord output
  // back as a real per-vertex stream. The bytecode and compile options are
  // retained at shader creation; the passthrough GS is compiled lazily on the
  // first draw that actually needs it and the bytecode is then released.
  // D3D11CommonShader objects are copied by value (module cache + COM wrapper),
  // so this state is shared via shared_ptr: one compile serves all copies.
  struct D3D11TexcoordCaptureVariant {
    Rc<DxvkShader> shader;
    bool           attempted = false;
  };

  struct D3D11TexcoordCaptureState {
    dxvk::mutex        mutex;
    std::vector<char>  bytecode;
    DxbcOptions        options;
    // The output signature's best texcoord-like element, used only when the
    // bound pixel shader proves no specific semantic for the sampled slot.
    std::string        semanticName;
    uint32_t           semanticIndex = 0;
    // One xfb variant per captured semantic: the pixel shader may sample its albedo with a different
    // TEXCOORD than the lowest-index one (Kenshi's terrain: TEXCOORD0 is the world normal, the colour-map
    // UV is TEXCOORD3).
    std::unordered_map<uint64_t, D3D11TexcoordCaptureVariant> variants;
  };

  struct D3D11TexcoordSemantic {
    std::string semanticName;
    uint32_t    semanticIndex = 0;
  };

  struct D3D11PositionCaptureVariant {
    Rc<DxvkShader> shader;
    bool           attempted = false;
  };

  struct D3D11SampledTexcoordSemantic {
    std::string semanticName;
    uint32_t    semanticIndex = 0;
    uint32_t    componentIndex = 0;
    bool        valid = false;
    uint8_t     projectionAxes = 0; // TEXCOORD0: YZ=1, XZ=2, XY=4
  };

  struct D3D11SampledSamplerSlot {
    uint32_t samplerSlot = 0;
    bool     valid = false;
    bool     ambiguous = false;
  };

  // A pixel shader that projects a map over the world computes its texture coordinate from an
  // interpolated world position, so there is no vertex attribute to capture and no input semantic to
  // prove, and the texture would be rejected (Kenshi's distant terrain, `$distantColour`). The profile
  // records where the shader's affine lives by reflected variable name; the values are read per draw and
  // the UV is reproduced with TexGenMode::WorldPositions plus the surface's texture transform.
  struct D3D11WorldProjectedUvProfile {
    bool     valid = false;
    uint32_t resourceSlot = 0;      // sampled texture this coordinate feeds
    uint32_t rectByteOffset = 0;    // float4: originX, originZ, invSizeX, invSizeZ
    uint32_t offsetByteOffset = 0;  // float3 added to the world position first
    bool     hasWorldOffset = false;
  };

  // A shader-exact cutout contract recovered from DXBC dataflow. The profile is
  // valid only for the canonical `discard_nz(alpha - threshold < 0)` form, so
  // unrelated clip/control-map logic remains outside the legacy alpha path.
  struct D3D11OpacityCutoutProfile {
    bool     valid = false;
    uint32_t alphaResourceSlot = 0;
    uint32_t thresholdConstantBufferSlot = 0;
    uint32_t thresholdConstantRegister = 0;
    uint32_t thresholdConstantComponent = 0;
  };

  // DX11_V290_POST_VS_POSITION_CAPTURE: some D3D11 engines perform skinning,
  // morphing, instancing, and object/world/view transforms entirely inside the
  // vertex shader, then expose a resulting pre-projection world/view position
  // as a non-system POSITION output. The IA POSITION stream alone is insufficient
  // for ray tracing in those games. Retain the original VS long enough to build
  // a transform-feedback passthrough shader that captures POSITIONn.xyz.
  enum class D3D11CapturedPositionSpace : uint8_t {
    View,
    World,
  };

  struct D3D11PositionCaptureState {
    dxvk::mutex        mutex;
    std::vector<char>  bytecode;
    DxbcOptions        options;
    // Stable application shader identity used by the capture logger.  Keeping
    // it with the shared state also makes lazy GS compilation attributable
    // after the original D3D11 shader wrapper has been copied or released.
    std::string        shaderName;
    std::string        semanticName;
    uint32_t           semanticIndex = 0;
    D3D11CapturedPositionSpace positionSpace = D3D11CapturedPositionSpace::View;
    // SV_Position is the only vertex output guaranteed to contain the exact
    // geometry the rasterizer consumed. When true, stream output stores xyzw
    // clip coordinates and the RT interleaver unprojects them to view space.
    bool               homogeneousClipSpace = false;
    // When the vertex shader also exposes a TEXCOORD output, capture it in the
    // same interleaved transform-feedback record as SV_Position. This keeps UVs
    // in the exact post-index-expansion domain consumed by the rasterizer and
    // avoids a second full vertex-shader replay.
    std::string        texcoordSemanticName;
    uint32_t           texcoordSemanticIndex = 0;
    // All non-system float2-or-larger VS outputs. The bound pixel shader's
    // sample dataflow selects one at draw time; TEXCOORD0 is only the fallback
    // when a shader does not expose a traceable texture-coordinate input.
    std::vector<D3D11TexcoordSemantic> texcoordSemantics;
    // Exact constant-buffer matrix proven by DXBC dataflow to transform this
    // captured output into SV_Position. At draw time the DX11 layer factors it
    // against the active projection to recover captured-position-to-view,
    // avoiding semantic-name guesses about object/world/view space.
    D3D11PositionTransformBinding clipTransform;
    bool               loadedFromProfile = false;
    std::unordered_map<uint64_t, D3D11PositionCaptureVariant> variants;
  };

  /**
   * \brief Common shader object
   * 
   * Stores the compiled SPIR-V shader and the SHA-1
   * hash of the original DXBC shader, which can be
   * used to identify the shader.
   */
  class D3D11CommonShader {
    
  public:
    
    D3D11CommonShader();
    D3D11CommonShader(
            D3D11Device*    pDevice,
      const DxvkShaderKey*  pShaderKey,
      const DxbcModuleInfo* pDxbcModuleInfo,
      const void*           pShaderBytecode,
            size_t          BytecodeLength);
    ~D3D11CommonShader();

    Rc<DxvkShader> GetShader() const {
      return m_shader;
    }

    Rc<DxvkBuffer> GetIcb() const {
      return m_buffer;
    }

    std::string GetName() const {
      return m_shader->debugName();
    }

    // DX11_V277_REAL_SHADER_MODEL: the actual shader model parsed from the
    // DXBC version token at creation (e.g. 5.0 for vs_5_0/ps_5_0). Modern
    // D3D11 games ship SM 5.x shaders; the capture previously stamped every
    // draw as SM 4.0, misreporting the pipeline to Remix's shader heuristics.
    uint32_t GetShaderModelMajor() const {
      return m_shaderModelMajor;
    }

    uint32_t GetShaderModelMinor() const {
      return m_shaderModelMinor;
    }

    // XXH3 hash of the original DXBC bytecode, computed once at shader creation.
    // Identical to XXH3_64bits(pShaderBytecode, BytecodeLength), so all hash-keyed
    // caches and user-tagged hashes stay bit-compatible across runs.
    XXH64_hash_t GetBytecodeHash() const {
      return m_bytecodeHash;
    }

    // DXBC reflection (RDEF): names of bound resources and constant-buffer
    // variables, parsed once at creation. Null when the shader shipped with
    // reflection stripped, which callers must tolerate.
    const DxbcRdef* GetReflection() const {
      return m_reflection.ptr();
    }

    // Native Kenshi VS contracts: 1 = proven UV*tiling, 2 = triplanar,
    // 3 = terrain feature. Computed once, never by per-draw disassembly.
    uint8_t GetKenshiProjection() const { return m_kenshiProjection; }
    bool SamplesTriplanar(uint32_t slot) const {
      return slot < m_sampledTexcoordSemantics.size()
          && m_sampledTexcoordSemantics[slot].projectionAxes == 7u;
    }

    // True when this shader's reflection declares a shader-resource (texture)
    // binding at the given slot. Lets the capture layer restrict material
    // identity to slots the shader actually samples, rather than every slot the
    // engine happens to leave bound.
    bool DeclaresTextureBinding(uint32_t slot) const {
      return m_reflection != nullptr
          && m_reflection->findBinding(DxbcResourceKind::Texture, slot) != nullptr;
    }

    // DX11_V280_TEXCOORD_CAPTURE: true when this is a vertex shader whose
    // output signature declares a texcoord-like element, i.e. a stream-out
    // capture variant CAN be built for it (cheap check, no compilation).
    bool HasTexcoordCaptureCandidate() const {
      return m_texcoordCapture != nullptr;
    }

    bool HasPositionCaptureCandidate() const {
      return m_positionCapture != nullptr;
    }

    D3D11CapturedPositionSpace GetPositionCaptureSpace() const {
      return m_positionCapture != nullptr
        ? m_positionCapture->positionSpace
        : D3D11CapturedPositionSpace::View;
    }

    bool IsPositionCaptureHomogeneousClipSpace() const {
      return m_positionCapture != nullptr
          && m_positionCapture->homogeneousClipSpace;
    }

    bool PositionCaptureIncludesTexcoord() const {
      return m_positionCapture != nullptr
          && !m_positionCapture->texcoordSemantics.empty();
    }

    // Returns the PS input semantic proven to feed sampling from the given
    // resource slot. This metadata belongs to pixel shaders; callers may ask
    // any common shader and receive false when no mapping exists.
    bool GetSampledTexcoordSemantic(uint32_t resourceSlot,
                                    std::string& semanticName,
                                    uint32_t& semanticIndex,
                                    uint32_t& componentIndex) const;

    bool HasCompleteSampledResourceProfile() const {
      return m_sampledResourceProfileComplete;
    }

    bool SamplesResourceSlot(uint32_t resourceSlot) const {
      return resourceSlot < m_sampledResourceSlots.size()
          && m_sampledResourceSlots[resourceSlot];
    }

    bool GetSampledSamplerSlot(uint32_t resourceSlot,
                               uint32_t& samplerSlot) const {
      if (resourceSlot >= m_sampledSamplerSlots.size()
       || !m_sampledSamplerSlots[resourceSlot].valid)
        return false;
      samplerSlot = m_sampledSamplerSlots[resourceSlot].samplerSlot;
      return true;
    }

    // Resolves a requested PS input semantic and component pair against this
    // VS's actual output signature. An explicit resource-slot contract is
    // exact-or-fail; only the legacy no-contract path may use the shader's
    // generic texcoord-like fallback.
    bool ResolvePositionCaptureTexcoord(const std::string& requestedName,
                                        uint32_t requestedIndex,
                                        uint32_t requestedComponent,
                                        std::string& semanticName,
                                        uint32_t& semanticIndex,
                                        uint32_t& componentIndex) const;

    const D3D11PositionTransformBinding* GetPositionCaptureClipTransformBinding() const {
      return m_positionCapture != nullptr && m_positionCapture->clipTransform.valid
        ? &m_positionCapture->clipTransform
        : nullptr;
    }

    // DX11_V281_FIXED_FUNCTION: true when this pixel shader's instruction
    // stream contains discard (HLSL clip()/discard) - DX10/11/12's actual
    // alpha-test mechanism, parsed once from the DXBC at creation.
    bool UsesDiscard() const {
      return m_usesDiscard;
    }

    // Which components of the COLOR0 input this pixel shader reads, as an xyzw bitmask (bit 0 = r ...
    // bit 3 = a), from the input signature's ReadWriteMask (see DxbcSgnEntry::usedMask). Zero for a
    // shader that ignores COLOR0 and for non-pixel stages. In Kenshi: 0 = no tint, 0x7 = albedo tint in
    // rgb, 0xF = tint plus a second-texture-set blend in alpha.
    uint32_t GetColor0UsedMask() const {
      return m_color0UsedMask;
    }

    // IA storage width does not say which skin influences the VS reads.
    bool UsesThreeExplicitSkinWeights() const {
      return m_skinWeightUsedMask == 7u && m_skinIndexUsedMask == 7u;
    }

    // Convenience: the shader multiplies its albedo by COLOR0.rgb.
    bool ReadsColor0RGB() const {
      return (m_color0UsedMask & 0x7u) != 0u;
    }

    const D3D11OpacityCutoutProfile* GetOpacityCutoutProfile() const {
      return m_opacityCutoutProfile.valid ? &m_opacityCutoutProfile : nullptr;
    }

    // Which blendMap channels this terrain pixel shader consumes, derived from its instruction stream at
    // creation. 0 for anything that is not a multi-biome terrain permutation, or whose shape the
    // derivation does not recognise.
    uint32_t GetKenshiTerrainBlendChannelMask() const {
      return m_kenshiTerrainBlendChannelMask;
    }

    const D3D11WorldProjectedUvProfile* GetWorldProjectedUvProfile() const {
      return m_worldProjectedUv.valid ? &m_worldProjectedUv : nullptr;
    }

    const D3D11PositionTransformBinding* GetPositionTransformBinding() const {
      return m_positionTransform.valid ? &m_positionTransform : nullptr;
    }

    const D3D11ConstantBufferDependencyProfile& GetConstantBufferDependencyProfile() const {
      return m_constantBufferDependencies;
    }

    // Lazily compiles and returns the xfb passthrough GS that captures the
    // named VS texcoord output (2 components, buffer 0, stride 8,
    // rasterization discarded). Returns nullptr if compilation failed or no
    // candidate exists. Thread-safe; compiles at most once per (shader,
    // semantic) pair. An empty name selects the output-signature default.
    Rc<DxvkShader> GetTexcoordCaptureShader(const std::string& semanticName = std::string(),
                                             uint32_t semanticIndex = 0,
                                             uint32_t componentIndex = 0,
                                             uint32_t componentCount = 2) const;

    // The output-signature default this shader would capture with no pixel
    // shader contract - the lowest-index texcoord-like output.
    bool GetDefaultTexcoordCaptureSemantic(std::string& semanticName,
                                           uint32_t& semanticIndex) const {
      if (m_texcoordCapture == nullptr)
        return false;
      semanticName = m_texcoordCapture->semanticName;
      semanticIndex = m_texcoordCapture->semanticIndex;
      return true;
    }

    // Lazily compiles the pure stream-output GS used to capture the game VS's
    // shader-computed pre-projection POSITIONn.xyz stream.
    Rc<DxvkShader> GetPositionCaptureShader(const std::string& texcoordSemanticName,
                                            uint32_t texcoordSemanticIndex,
                                            uint32_t texcoordComponentIndex) const;

  private:

    Rc<DxvkShader> m_shader;
    Rc<DxvkBuffer> m_buffer;

    // DX11_V277_REAL_SHADER_MODEL: the version token parsed from each DXBC
    // container overwrites these. The fallback is the highest model D3D11 can
    // legally express (SM 5.1 at FL 12_x). SM 6.x is DXIL and exists only on
    // D3D12 - no D3D11 device, real or wrapped, can report or consume it.
    uint32_t m_shaderModelMajor = 5;
    uint32_t m_shaderModelMinor = 1;

    // XXH3 of the original DXBC container, computed once at shader creation so
    // per-draw RTX lookups never rehash the bytecode. See GetBytecodeHash().
    XXH64_hash_t m_bytecodeHash = 0;

    // Reflection chunk retained from the DXBC module. See GetReflection().
    Rc<DxbcRdef> m_reflection;

    // DX11_V281_FIXED_FUNCTION (parsed for pixel shaders only)
    bool m_usesDiscard = false;
    // Pixel shaders only; see GetColor0UsedMask(). Placed next to m_usesDiscard to use that bool's tail
    // padding.
    uint8_t m_color0UsedMask = 0;
    uint8_t m_skinWeightUsedMask = 0;
    uint8_t m_skinIndexUsedMask = 0;
    D3D11OpacityCutoutProfile m_opacityCutoutProfile;
    uint8_t m_kenshiProjection = 0;
    // Pixel shaders only.
    uint8_t m_kenshiTerrainBlendChannelMask = 0;
    D3D11WorldProjectedUvProfile m_worldProjectedUv;

    std::array<D3D11SampledTexcoordSemantic,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> m_sampledTexcoordSemantics = {};
    std::array<bool,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> m_sampledResourceSlots = {};
    std::array<D3D11SampledSamplerSlot,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> m_sampledSamplerSlots = {};
    bool m_sampledResourceProfileComplete = false;

    D3D11PositionTransformBinding m_positionTransform;
    D3D11ConstantBufferDependencyProfile m_constantBufferDependencies;

    // DX11_V280_TEXCOORD_CAPTURE (null for non-VS or VS without texcoord output)
    std::shared_ptr<D3D11TexcoordCaptureState> m_texcoordCapture;

    // DX11_V290_POST_VS_POSITION_CAPTURE (null unless the output signature has
    // a conservative position candidate such as POSITION1/VIEWPOSITION).
    std::shared_ptr<D3D11PositionCaptureState> m_positionCapture;

  };
  
  
  /**
   * \brief Common shader interface
   * 
   * Implements methods for all D3D11*Shader
   * interfaces and stores the actual shader
   * module object.
   */
  template<typename D3D11Interface>
  class D3D11Shader : public D3D11DeviceChild<D3D11Interface> {

  public:
    
    D3D11Shader(D3D11Device* device, const D3D11CommonShader& shader)
    : D3D11DeviceChild<D3D11Interface>(device),
      m_shader(shader) { }
    
    ~D3D11Shader() { }
    
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) final {
      *ppvObject = nullptr;
      
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(D3D11Interface)) {
        *ppvObject = ref(this);
        return S_OK;
      }
      
      Logger::warn("D3D11Shader::QueryInterface: Unknown interface query");
      return E_NOINTERFACE;
    }
    
    const D3D11CommonShader* GetCommonShader() const {
      return &m_shader;
    }

  private:
    
    D3D11CommonShader m_shader;
    
  };
  
  using D3D11VertexShader   = D3D11Shader<ID3D11VertexShader>;
  using D3D11HullShader     = D3D11Shader<ID3D11HullShader>;
  using D3D11DomainShader   = D3D11Shader<ID3D11DomainShader>;
  using D3D11GeometryShader = D3D11Shader<ID3D11GeometryShader>;
  using D3D11PixelShader    = D3D11Shader<ID3D11PixelShader>;
  using D3D11ComputeShader  = D3D11Shader<ID3D11ComputeShader>;
  
  
  /**
   * \brief Shader module set
   * 
   * Some applications may compile the same shader multiple
   * times, so we should cache the resulting shader modules
   * and reuse them rather than creating new ones. This
   * class is thread-safe.
   */
  class D3D11ShaderModuleSet {
    
  public:
    
    D3D11ShaderModuleSet();
    ~D3D11ShaderModuleSet();
    
    HRESULT GetShaderModule(
            D3D11Device*        pDevice,
      const DxvkShaderKey*      pShaderKey,
      const DxbcModuleInfo*     pDxbcModuleInfo,
      const void*               pShaderBytecode,
            size_t              BytecodeLength,
            D3D11CommonShader*  pShader);
    
  private:
    
    dxvk::mutex m_mutex;
    
    std::unordered_map<
      DxvkShaderKey,
      D3D11CommonShader,
      DxvkHash, DxvkEq> m_modules;
    
  };
  
}
