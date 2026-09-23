/*
 * Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#pragma once

// DX11_V225: native DX11/DXGI texture & volume helpers. This is the DX11 bridge,
// so all surface/format math is expressed with DXGI_FORMAT, D3D11_TEXTURE2D_DESC
// and D3D11_MAPPED_SUBRESOURCE. No D3D11 (D3DFORMAT / D3DFMT_* / D3DSURFACE_DESC /
// D3DLOCKED_RECT) types are used.

#include "util_bridgecommand.h"
#include "util_common.h"

#include <d3d11.h>
#include <dxgiformat.h>
#include <algorithm>
#include <functional>

namespace bridge_util {

  // Width/height (in texels) of a single block for the given format. Block
  // compressed (BCn) formats use 4x4 texel blocks; everything else is 1.
  static uint32_t getBlockSize(const DXGI_FORMAT& format) {
    switch (format) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
      return 4;
    default:
      return 1;
    }
  }

  // Bytes per pixel for uncompressed formats, or bytes per 4x4 block for block
  // compressed (BCn) formats.
  static uint32_t getBytesFromFormat(const DXGI_FORMAT& format) {
    switch (format) {
    // 128-bit / 16 bytes per texel
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
      return 16;

    // 96-bit / 12 bytes per texel
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
      return 12;

    // 64-bit / 8 bytes per texel, plus the 16-byte-per-block BCn formats
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    // BCn blocks that are 16 bytes per 4x4 block
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
      return 8;

    // BCn blocks that are 8 bytes per 4x4 block
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
      // Note: getBlockSize() returns 4 for these, so the row/stride math treats
      // them as one 8-byte unit per 4x4 block (matching DXVK's block packing).
      return 8;

    // 32-bit / 4 bytes per texel
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_R8G8_B8G8_UNORM:
    case DXGI_FORMAT_G8R8_G8B8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return 4;

    // 16-bit / 2 bytes per texel
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
      return 2;

    // 8-bit / 1 byte per texel
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_P8:
      return 1;

    case DXGI_FORMAT_UNKNOWN:
      return 0;

    default:
      Logger::err("Unknown DXGI_FORMAT passed for conversion: " + std::to_string(static_cast<int>(format)));
      throw;
      return 0;
    }
  }

  // Num pixels OR num compressed blocks along one axis.
  static inline uint32_t calcStride(const uint32_t numPixels, const DXGI_FORMAT format) {
    const uint32_t effectivePixelsPerUnit = getBlockSize(format);
    // If effectivePixelsPerUnit == 4, numPixels is compressed (BCn 4x4 blocks).
    return ((numPixels + effectivePixelsPerUnit - 1) / effectivePixelsPerUnit);
  }

  static inline uint32_t calcRowSize(const uint32_t width, const DXGI_FORMAT format) {
    const uint32_t numPixelsInRow = calcStride(width, format);
    const uint32_t bytesPerPixel = getBytesFromFormat(format);
    return std::max(caps::MinSurfacePitch, numPixelsInRow * bytesPerPixel);
  }

  static inline uint32_t calcTotalSizeOfRect(const uint32_t width, const uint32_t height, const DXGI_FORMAT format) {
    const uint32_t numRows = calcStride(height, format);
    const uint32_t rowSize = calcRowSize(width, format);
    return numRows * rowSize;
  }

  static inline size_t calcImageByteOffset(const int32_t pitch, const RECT& rect, const DXGI_FORMAT format) {
    const uint32_t y = calcStride(rect.top, format);  // unit: unitless ('y' is a row ID); pitch unit: bytes
    const uint32_t x = calcStride(rect.left, format); // unit: pixels (or blocks)
    const uint32_t bytesPerPixel = getBytesFromFormat(format);
    return y * pitch + bytesPerPixel * x;
  }

  struct RectDecompInfo {
    size_t baseX, baseY, width, height;
  };
  static RectDecompInfo getDecomposedRectInfo(const D3D11_TEXTURE2D_DESC& desc, const RECT* pRect) {
    RectDecompInfo decomp;
    decomp.baseX  = (pRect) ? pRect->left : 0;
    decomp.baseY  = (pRect) ? pRect->top : 0;
    decomp.width  = (pRect) ? pRect->right - pRect->left : desc.Width;
    decomp.height = (pRect) ? pRect->bottom - pRect->top : desc.Height;
    return decomp;
  }
}

// DX11_V225: iterate the rows of a mapped D3D11 subresource. MAPPED is a
// D3D11_MAPPED_SUBRESOURCE (pData / RowPitch).
#define FOR_EACH_RECT_ROW(MAPPED, HEIGHT, FORMAT, DO_THIS_TO_ptr)         \
{                                                                        \
  const uint32_t columnStride = bridge_util::calcStride(HEIGHT, FORMAT); \
  for (uint32_t y = 0; y < columnStride; y++) {                          \
    auto ptr = (PBYTE) (MAPPED).pData + y * (MAPPED).RowPitch;           \
    {                                                                    \
      DO_THIS_TO_ptr                                                     \
    }                                                                    \
  }                                                                      \
}
