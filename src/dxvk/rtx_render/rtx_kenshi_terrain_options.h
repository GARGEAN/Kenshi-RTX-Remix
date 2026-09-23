/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx_option.h"

namespace dxvk {

  struct KenshiTerrainOptions {
    friend class ImGUI;
    RTX_OPTION("rtx.kenshiTerrain", bool, antiCulling, true,
      "Keep nearby off-screen terrain available for shadows and lighting. Changes live; native LOD and loading remain unchanged.");
    RTX_OPTION("rtx.kenshiTerrain", float, antiCullingRadius, 10000.0f,
      "Camera-to-terrain bounding-box distance in game units. Changes live; minimum 100, no fixed upper limit (finite values only). "
      "Outside this radius normal frustum culling applies; visible distant terrain remains eligible.");

    // 0: full terrain compositor on every ray; 1: one tiled base layer on
    // indirect rays; 2: macro colour only on indirect rays.
    RTX_OPTION("rtx.kenshiTerrain", uint32_t, secondaryShadingMode, 0u,
      "Kenshi terrain shading quality for secondary ray hits. Primary terrain is always fully blended, and "
      "visibility rays never run the terrain compositor in any mode - they read opacity, which it does not affect. "
      "0 = full blending on indirect rays, 1 = one tiled base layer on indirect rays, 2 = macro colour only on indirect rays.");

    // DX11_V528_KENSHI_TERRAIN_BIOME_BLEND. Gated on the CPU: with this off the
    // blend mask is never written, so every terrain record is byte-identical to
    // the V399 single-set runtime and the selector cannot execute at all. That
    // makes it a true kill switch for a feature whose predecessor (V405-V414)
    // ended in device loss at save load.
    // DX11_V531_KENSHI_TERRAIN_GLOSS. Defaults to 0, i.e. off: the mapping from
    // Kenshi's deferred gloss to perceptual roughness is a judgement call, and
    // the material families that carry no gloss stay on the flat
    // rtx.legacyMaterial.roughnessConstant - so the two populations have to be
    // reconciled by eye rather than by a fixed number. Live: the value reaches
    // the shader through a constant, so the slider takes effect immediately.
    RTX_OPTION("rtx.kenshiTerrain", float, glossStrength, 0.0f,
      "How strongly Kenshi's own terrain gloss drives path-traced roughness. The game stores it in the alpha "
      "of each terrain detail layer and blends it with the same weights as the colour; this maps it as "
      "perceptual roughness = 1 - gloss. 0 keeps the flat legacy roughness constant, 1 uses the game's value "
      "outright. Terrain only - other material families do not expose a gloss the bridge can read yet.");

    RTX_OPTION("rtx.kenshiTerrain", bool, biomeBlend, true,
      "Cross-fade Kenshi's terrain between neighbouring biome material sets instead of stepping at the boundary. "
      "One set is chosen stochastically per hit from the game's own blendMap weights, so the path tracer resolves the "
      "transition. Set to False to restore the hard biome boundary.");
  };

}
