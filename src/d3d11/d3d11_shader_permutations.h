#pragma once

#include <array>
#include <cstdint>

#include "../util/rc/util_rc_ptr.h"

namespace dxvk {

  class DxvkShader;

  // DX11_V225: specialization permutations the shader compiler can emit for a single
  // shader. DX11 only needs the base shader plus a flat-shaded pixel-shader variant
  // (for D3DSHADE_FLAT-style provoking-vertex emulation).
  namespace D3D11ShaderPermutations {
    enum D3D11ShaderPermutation : uint32_t {
      None = 0,
      FlatShade,
      Count
    };
  }

  using D3D11Permutations = std::array<Rc<DxvkShader>, D3D11ShaderPermutations::Count>;

}
