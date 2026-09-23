#pragma once

// DX11_V225: the shared D3D11 shader limits live in d3d11_shader_caps.h (which the
// build generates with the API-agnostic `caps` namespace). This header just forwards
// so the `#include "../d3d11/d3d11_caps.h"` references resolve to it.
#include "d3d11_shader_caps.h"
