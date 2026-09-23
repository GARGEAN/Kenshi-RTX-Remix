#pragma once

// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
//
// DX11-owned fixed-function-style fog emulation.
// Direct3D 11 has no native fixed-function fog render-state API, so this API
// provides equivalent runtime state for the D3D11 capture path.
//
// No D3D9 headers. No D3D9 bridge.

#include <cmath>
#include <cstdint>

namespace dxvk {

enum Dx11FixedFogMode : uint32_t {
  DX11_FOG_NONE   = 0,
  DX11_FOG_LINEAR = 1,
  DX11_FOG_EXP    = 2,
  DX11_FOG_EXP2   = 3,
};

struct Dx11FogColor {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct Dx11FixedFogDesc {
  bool enabled = false;
  Dx11FixedFogMode mode = DX11_FOG_NONE;
  Dx11FogColor color = {};
  float start = 0.0f;
  float end = 1.0f;
  float density = 0.0f;
  bool rangeBased = false;
  bool clampFactor = true;
};

struct Dx11ExponentialHeightFogDesc {
  bool enabled = false;
  float density = 0.0f;
  float heightFalloff = 0.2f;
  float heightOffset = 0.0f;
  float startDistance = 0.0f;
  float cutoffDistance = 0.0f;
  float maxOpacity = 1.0f;
  Dx11FogColor inscatteringColor = {};
  Dx11FogColor oppositeInscatteringColor = {};
};

struct Dx11FogShaderConstants {
  float fogColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
  float params0[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
  float params1[4] = { 0.0f, 0.2f, 0.0f, 1.0f };
  uint32_t mode = DX11_FOG_NONE;
  uint32_t enabled = 0;
  uint32_t rangeBased = 0;
  uint32_t heightFogEnabled = 0;
};

class Dx11FixedFunctionFogApi {
public:
  void reset() {
    m_fixed = {};
    m_height = {};
  }

  void setEnabled(bool enabled) {
    m_fixed.enabled = enabled;
    if (!enabled)
      m_fixed.mode = DX11_FOG_NONE;
  }

  void setMode(Dx11FixedFogMode mode) {
    m_fixed.mode = mode;
    m_fixed.enabled = mode != DX11_FOG_NONE;
  }

  void setColor(float r, float g, float b, float a = 1.0f) {
    m_fixed.color = { r, g, b, a };
  }

  void setLinearRange(float start, float end) {
    m_fixed.start = start;
    m_fixed.end = end;
    m_fixed.mode = DX11_FOG_LINEAR;
    m_fixed.enabled = true;
  }

  void setDensity(float density) {
    m_fixed.density = density;
    if (m_fixed.mode == DX11_FOG_NONE)
      m_fixed.mode = DX11_FOG_EXP;
    m_fixed.enabled = true;
  }

  void setRangeBased(bool rangeBased) {
    m_fixed.rangeBased = rangeBased;
  }

  void setClampFactor(bool clampFactor) {
    m_fixed.clampFactor = clampFactor;
  }

  void setExponentialHeightFog(const Dx11ExponentialHeightFogDesc& desc) {
    m_height = desc;
  }

  const Dx11FixedFogDesc& fixedFog() const { return m_fixed; }
  const Dx11ExponentialHeightFogDesc& heightFog() const { return m_height; }

  static float saturate(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  }

  float evaluateFixedFactor(float distance) const {
    if (!m_fixed.enabled || m_fixed.mode == DX11_FOG_NONE)
      return 0.0f;

    float factor = 0.0f;
    switch (m_fixed.mode) {
    case DX11_FOG_LINEAR: {
      const float range = m_fixed.end - m_fixed.start;
      factor = range != 0.0f ? (distance - m_fixed.start) / range : 1.0f;
      break;
    }
    case DX11_FOG_EXP:
      factor = 1.0f - std::exp(-m_fixed.density * distance);
      break;
    case DX11_FOG_EXP2: {
      const float d = m_fixed.density * distance;
      factor = 1.0f - std::exp(-(d * d));
      break;
    }
    default:
      factor = 0.0f;
      break;
    }

    return m_fixed.clampFactor ? saturate(factor) : factor;
  }

  float evaluateHeightFactor(float cameraHeight, float sampleHeight, float distance) const {
    if (!m_height.enabled)
      return 0.0f;

    const float sample = sampleHeight - m_height.heightOffset;
    const float camera = cameraHeight - m_height.heightOffset;
    const float sampleDensity = std::exp(-m_height.heightFalloff * sample);
    const float cameraDensity = std::exp(-m_height.heightFalloff * camera);
    const float startFade = distance > m_height.startDistance ? 1.0f : 0.0f;

    float factor = (1.0f - std::exp(-m_height.density * sampleDensity * cameraDensity * distance)) * startFade;
    if (m_height.cutoffDistance > 0.0f && distance > m_height.cutoffDistance)
      factor = 0.0f;
    if (factor > m_height.maxOpacity)
      factor = m_height.maxOpacity;

    return saturate(factor);
  }

  Dx11FogShaderConstants buildConstants() const {
    Dx11FogShaderConstants c = {};
    c.fogColor[0] = m_fixed.color.r;
    c.fogColor[1] = m_fixed.color.g;
    c.fogColor[2] = m_fixed.color.b;
    c.fogColor[3] = m_fixed.color.a;

    const float range = m_fixed.end - m_fixed.start;
    c.params0[0] = m_fixed.start;
    c.params0[1] = m_fixed.end;
    c.params0[2] = m_fixed.density;
    c.params0[3] = range != 0.0f ? (1.0f / range) : 1.0f;

    c.params1[0] = m_height.heightOffset;
    c.params1[1] = m_height.heightFalloff;
    c.params1[2] = m_height.startDistance;
    c.params1[3] = m_height.maxOpacity;

    c.mode = static_cast<uint32_t>(m_fixed.mode);
    c.enabled = m_fixed.enabled ? 1u : 0u;
    c.rangeBased = m_fixed.rangeBased ? 1u : 0u;
    c.heightFogEnabled = m_height.enabled ? 1u : 0u;
    return c;
  }

private:
  Dx11FixedFogDesc m_fixed = {};
  Dx11ExponentialHeightFogDesc m_height = {};
};

}