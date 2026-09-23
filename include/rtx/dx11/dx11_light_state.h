#pragma once

// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
//
// DX11-owned runtime light state for the D3D11 capture path.
//
// D3D11 does not expose a fixed-function D3DLIGHT API. This header provides a
// project-owned light description that can be filled from D3D11 shader constants,
// engine uniforms, scene analysis, draw metadata, and Remix light extraction.
// No D3D9 headers. No D3D9 bridge.

#include <cstdint>

namespace dxvk {

enum Dx11LightType : uint32_t {
  DX11_LIGHT_POINT       = 1,
  DX11_LIGHT_SPOT        = 2,
  DX11_LIGHT_DIRECTIONAL = 3,
  DX11_LIGHT_FORCE_DWORD = 0x7fffffff,
};

struct Dx11Vector3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Dx11LightColor {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct Dx11LightDesc {
  Dx11LightType Type = DX11_LIGHT_POINT;

  Dx11LightColor Diffuse  = { 1.0f, 1.0f, 1.0f, 1.0f };
  Dx11LightColor Specular = { 0.0f, 0.0f, 0.0f, 1.0f };
  Dx11LightColor Ambient  = { 0.0f, 0.0f, 0.0f, 1.0f };

  Dx11Vector3 Position  = {};
  Dx11Vector3 Direction = { 0.0f, -1.0f, 0.0f };

  float Range        = 0.0f;
  float Falloff      = 1.0f;
  float Attenuation0 = 1.0f;
  float Attenuation1 = 0.0f;
  float Attenuation2 = 0.0f;
  float Theta        = 0.0f;
  float Phi          = 0.0f;

  // DX11_V765_KENSHI_LIGHT_GC. Kenshi's OWN light-volume radius (falloff.w),
  // which is what the game culls the volume draw against. Deliberately not
  // Range: the bridge folds sqrt(power) * sqrt(kenshiLightIntensityScale) into
  // Range for brightness (V479), which runs it ~3.4-4x larger than the volume.
  // Zero for any light that did not come from Kenshi's light_fs harvest.
  float KenshiCullRadius = 0.0f;

  bool Enabled       = true;
};

struct Dx11LightShaderConstants {
  float position_range[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  float direction_type[4] = { 0.0f, -1.0f, 0.0f, 1.0f };
  float diffuse[4]        = { 1.0f, 1.0f, 1.0f, 1.0f };
  float specular[4]       = { 0.0f, 0.0f, 0.0f, 1.0f };
  float ambient[4]        = { 0.0f, 0.0f, 0.0f, 1.0f };
  float attenuation[4]    = { 1.0f, 0.0f, 0.0f, 1.0f };
  float cone[4]           = { 0.0f, 0.0f, 1.0f, 0.0f };
};

class Dx11LightStateApi {
public:
  static Dx11LightDesc makePoint(
      float x, float y, float z,
      float r, float g, float b,
      float range = 0.0f) {
    Dx11LightDesc light = {};
    light.Type = DX11_LIGHT_POINT;
    light.Position = { x, y, z };
    light.Diffuse = { r, g, b, 1.0f };
    light.Range = range;
    return light;
  }

  static Dx11LightDesc makeDirectional(
      float x, float y, float z,
      float r, float g, float b) {
    Dx11LightDesc light = {};
    light.Type = DX11_LIGHT_DIRECTIONAL;
    light.Direction = { x, y, z };
    light.Diffuse = { r, g, b, 1.0f };
    return light;
  }

  static Dx11LightDesc makeSpot(
      float px, float py, float pz,
      float dx, float dy, float dz,
      float r, float g, float b,
      float theta, float phi,
      float range = 0.0f) {
    Dx11LightDesc light = {};
    light.Type = DX11_LIGHT_SPOT;
    light.Position = { px, py, pz };
    light.Direction = { dx, dy, dz };
    light.Diffuse = { r, g, b, 1.0f };
    light.Theta = theta;
    light.Phi = phi;
    light.Range = range;
    return light;
  }

  static Dx11LightShaderConstants buildConstants(const Dx11LightDesc& light) {
    Dx11LightShaderConstants c = {};
    c.position_range[0] = light.Position.x;
    c.position_range[1] = light.Position.y;
    c.position_range[2] = light.Position.z;
    c.position_range[3] = light.Range;

    c.direction_type[0] = light.Direction.x;
    c.direction_type[1] = light.Direction.y;
    c.direction_type[2] = light.Direction.z;
    c.direction_type[3] = static_cast<float>(static_cast<uint32_t>(light.Type));

    c.diffuse[0] = light.Diffuse.r;
    c.diffuse[1] = light.Diffuse.g;
    c.diffuse[2] = light.Diffuse.b;
    c.diffuse[3] = light.Diffuse.a;

    c.specular[0] = light.Specular.r;
    c.specular[1] = light.Specular.g;
    c.specular[2] = light.Specular.b;
    c.specular[3] = light.Specular.a;

    c.ambient[0] = light.Ambient.r;
    c.ambient[1] = light.Ambient.g;
    c.ambient[2] = light.Ambient.b;
    c.ambient[3] = light.Ambient.a;

    c.attenuation[0] = light.Attenuation0;
    c.attenuation[1] = light.Attenuation1;
    c.attenuation[2] = light.Attenuation2;
    c.attenuation[3] = light.Enabled ? 1.0f : 0.0f;

    c.cone[0] = light.Theta;
    c.cone[1] = light.Phi;
    c.cone[2] = light.Falloff;
    c.cone[3] = 0.0f;

    return c;
  }
};

}