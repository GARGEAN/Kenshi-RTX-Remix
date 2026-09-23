#pragma once

// The constant-buffer binding enums (D3D11ConstantBuffers) and binding-slot helpers
// live in d3d11_resource_slot.h. This header exists so the
// "#include ../d3d11/d3d11_constant_set.h" reference resolves and forwards to the
// canonical definitions. DX11 has no D3D11-style constant-set state; the binding
// indices only lay out the Vulkan descriptor slots.
#include "d3d11_resource_slot.h"
