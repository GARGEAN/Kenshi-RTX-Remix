#pragma once

#include <cstdint>

namespace dxvk {

  // DX11_V225: register-count/offset layout of the shader-constant buffer used by
  // the shader compiler. DX11 packs the legacy int/float/bitmask constant regions
  // into a single buffer; these helpers give the byte offsets of each region.
  struct D3D11ConstantLayout {
    uint32_t floatCount   = 0;
    uint32_t intCount     = 0;
    uint32_t boolCount    = 0;
    uint32_t bitmaskCount = 0;

    uint32_t floatSize()   const { return floatCount * 4 * sizeof(float); }
    uint32_t intSize()     const { return intCount   * 4 * sizeof(int); }
    uint32_t bitmaskSize() const {
      return bitmaskCount != 1
        ? bitmaskCount * sizeof(uint32_t)
        : 0;
    }

    uint32_t intOffset()     const { return 0; }
    uint32_t floatOffset()   const { return intOffset() + intSize(); }
    uint32_t bitmaskOffset() const { return floatOffset() + floatSize(); }

    uint32_t totalSize()     const { return floatSize() + intSize() + bitmaskSize(); }
  };

}
