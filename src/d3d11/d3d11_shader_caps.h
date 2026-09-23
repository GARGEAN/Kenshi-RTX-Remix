#pragma once

// DX11_V219_FULL_DX11_ONLY_NO_DX11
//
// The DXBC shared compiler utilities need a caps namespace for array bounds
// and register limits.  This DX11 fork must not depend on src/d3d11.
// This header provides those shared compiler limits from the DX11 side using
// D3D11 SDK limits where possible and shader-model compatibility limits
// where the value belongs to the legacy bytecode model being decoded.

#include <cstdint>

#ifndef D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
#define D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT 8
#endif
#ifndef D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
#define D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT 16
#endif
#ifndef D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
#define D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT 128
#endif
#ifndef D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
#define D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT 32
#endif
#ifndef D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
#define D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION 16384
#endif
#ifndef D3D11_REQ_TEXTURECUBE_DIMENSION
#define D3D11_REQ_TEXTURECUBE_DIMENSION 16384
#endif
#ifndef D3D11_COMMONSHADER_CONSTANT_BUFFER_REGISTER_COUNT
#define D3D11_COMMONSHADER_CONSTANT_BUFFER_REGISTER_COUNT 15
#endif
#ifndef D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT
#define D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT 4096
#endif
#ifndef D3D11_CLIP_OR_CULL_DISTANCE_COUNT
#define D3D11_CLIP_OR_CULL_DISTANCE_COUNT 8
#endif

namespace dxvk::caps {
  constexpr uint32_t MaxClipPlanes                = D3D11_CLIP_OR_CULL_DISTANCE_COUNT;
  constexpr uint32_t MaxSamplers                  = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
  constexpr uint32_t MaxStreams                   = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
  constexpr uint32_t MaxSimultaneousTextures      = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
  constexpr uint32_t MaxTextureBlendStages        = MaxSimultaneousTextures;
  constexpr uint32_t TextureStageCount            = MaxSimultaneousTextures;
  constexpr uint32_t MaxSimultaneousRenderTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
  constexpr uint32_t MaxTextureDimension          = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  constexpr uint32_t MaxTextureCubeDimension      = D3D11_REQ_TEXTURECUBE_DIMENSION;
  constexpr uint32_t MaxTexturesVS                = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
  constexpr uint32_t MaxTexturesPS                = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
  constexpr uint32_t MaxTextures                  = MaxTexturesVS + MaxTexturesPS;

  // Shader-model compatibility limits used by the shared decoder.
  // These are not a dependency on a DX11 runtime path.
  constexpr uint32_t MaxFloatConstantsVS          = 256;
  constexpr uint32_t MaxSM1FloatConstantsPS       = 8;
  constexpr uint32_t MaxSM2FloatConstantsPS       = 32;
  constexpr uint32_t MaxSM3FloatConstantsPS       = 224;
  constexpr uint32_t MaxOtherConstants            = 16;
  constexpr uint32_t MaxFloatConstantsSoftware    = D3D11_COMMONSHADER_CONSTANT_BUFFER_REGISTER_COUNT * D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
  constexpr uint32_t MaxOtherConstantsSoftware    = 2048;
  constexpr uint32_t InputRegisterCount           = 16;
  constexpr uint32_t MaxMipLevels                 = 15;
  constexpr uint32_t MaxSubresources              = MaxMipLevels * 6;
  constexpr uint32_t MaxTransforms                = 10 + 256;
  constexpr uint32_t MaxEnabledLights             = 8;
}