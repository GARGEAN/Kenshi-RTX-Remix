#pragma once

// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
//
// DX11 runtime material/fog state. Fog is handled by Dx11FixedFunctionFogApi.

#include "rtx/dx11/dx11_fixed_function_fog.h"

namespace dxvk {

struct Dx11MaterialColor {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct Dx11RuntimeMaterial {
  Dx11MaterialColor Diffuse  = { 1.0f, 1.0f, 1.0f, 1.0f };
  Dx11MaterialColor Ambient  = { 0.0f, 0.0f, 0.0f, 1.0f };
  Dx11MaterialColor Specular = { 0.0f, 0.0f, 0.0f, 1.0f };
  Dx11MaterialColor Emissive = { 0.0f, 0.0f, 0.0f, 1.0f };
  float Power = 0.0f;
};

using Dx11FogMode = Dx11FixedFogMode;
using Dx11FogState = Dx11FixedFogDesc;

}