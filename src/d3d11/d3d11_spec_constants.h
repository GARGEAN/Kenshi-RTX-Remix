#pragma once

// DX11_V225: dxvk_context.cpp references D3D11SpecConstantId.
// It is the API-agnostic RTX specialization-constant ID set (RtxSpecConstantId).
// Alias it here so the existing D3D11SpecConstantId::* references resolve without
// duplicating the enum (which would risk drift from the canonical definition).

#include "../dxvk/rtx_render/rtx_spec_constants.h"

namespace dxvk {
  using D3D11SpecConstantId = RtxSpecConstantId;
}
