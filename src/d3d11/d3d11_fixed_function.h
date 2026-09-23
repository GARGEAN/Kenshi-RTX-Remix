#pragma once

#include <cstdint>

#include "../util/util_matrix.h"
#include "../util/util_vector.h"
// DX11_V225: caps::TextureStageCount for the shared PS state stage array.
#include "d3d11_caps.h"

namespace dxvk {

  // DX11_V225: DX11 has no fixed-function pipeline. When a captured draw has no
  // programmable vertex shader (or for terrain baking), the DX11 layer fills this
  // vertex constant block from the captured transform/lighting state. It is used
  // purely as a trivially-copyable CPU-side constant buffer (no D3D11 FF shader is
  // generated for it on DX11), so the layout only has to be self-consistent.
  struct D3D11FixedFunctionVS {
    // Per-light fixed-function lighting parameters.
    struct Light {
      Vector4  Diffuse   = Vector4(0.0f);
      Vector4  Specular  = Vector4(0.0f);
      Vector4  Ambient   = Vector4(0.0f);
      Vector4  Position  = Vector4(0.0f);
      Vector4  Direction = Vector4(0.0f);
      uint32_t Type         = 0;
      float    Range        = 0.0f;
      float    Falloff      = 0.0f;
      float    Attenuation0 = 0.0f;
      float    Attenuation1 = 0.0f;
      float    Attenuation2 = 0.0f;
      float    Theta        = 0.0f;
      float    Phi          = 0.0f;
    };

    static constexpr uint32_t MaxLights = 8;

    Matrix4 World        = Matrix4();
    Matrix4 View         = Matrix4();
    Matrix4 WorldView    = Matrix4();
    Matrix4 InverseView  = Matrix4();
    Matrix4 Projection   = Matrix4();
    // DX11-specific: the bridge can supply a direct world->projection matrix and a
    // sub-pixel jitter for temporal upscalers instead of relying on a FF pipeline.
    Matrix4 customWorldToProjection = Matrix4();

    Light   Lights[MaxLights];

    float   jitterX = 0.0f;
    float   jitterY = 0.0f;
  };

  // DX11_V225: named members of D3D11RtxVertexCaptureData (excludes the trailing
  // padding). The shader compiler asserts this count matches the SPIR-V struct it
  // builds for vertex capture.
  enum class D3D11RtxVertexCaptureMembers : uint32_t {
    normalTransform = 0,
    customWorldToProjection,
    invProj,
    viewToWorld,
    worldToObject,
    baseVertex,
    jitterX,
    jitterY,
    MemberCount
  };

  // DX11_V225: programmable-pipeline vertex-capture constant block. The shader
  // compiler builds a matching SPIR-V uniform block via offsetof(), so the field
  // order and types here are authoritative (matrices row-major, stride 16). Kept a
  // plain standard-layout / trivially-copyable POD so offsetof() is well-defined.
  struct D3D11RtxVertexCaptureData {
    Matrix4  normalTransform;
    Matrix4  customWorldToProjection;
    Matrix4  invProj;
    Matrix4  viewToWorld;
    Matrix4  worldToObject;
    uint32_t baseVertex;
    float    jitterX;
    float    jitterY;
    uint32_t padding;
  };

  // DX11_V225: field indices within a D3D11SharedPS::Stage. The shader compiler reads
  // the shared PS state as a flat float array indexed by
  // (D3D11SharedPSStages_Count * stage + field), so D3D11SharedPS::Stage MUST be
  // exactly D3D11SharedPSStages_Count floats laid out in this order.
  enum {
    D3D11SharedPSStages_TexturePreOffset  = 0,
    D3D11SharedPSStages_TextureScale      = 1,
    D3D11SharedPSStages_TexturePostOffset = 2,
    D3D11SharedPSStages_Count             = 3,
  };

  // DX11_V225: per-texture-stage shared pixel-shader constant state used by the
  // fixed-function-equivalent terrain baking path.
  struct D3D11SharedPS {
    struct Stage {
      float texturePreOffset;
      float textureScale;
      float texturePostOffset;
    };
    Stage Stages[caps::TextureStageCount];
  };

}
