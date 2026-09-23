/*
* Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
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
#ifndef SHARED_CONSTANTS_H
#define SHARED_CONSTANTS_H

// contains constants shared between shader and host code

static const uint8_t surfaceMaterialTypeOpaque = uint8_t(0u);
static const uint8_t surfaceMaterialTypeTranslucent = uint8_t(1u);
static const uint8_t surfaceMaterialTypeRayPortal = uint8_t(2u);
static const uint8_t surfaceMaterialTypeMask = uint8_t(0x3u);

#define COMMON_MATERIAL_FLAG_TYPE_MASK surfaceMaterialTypeMask
#define COMMON_MATERIAL_FLAG_TYPE_OFFSET(X) (2 + X)

// NOTE: Each material memory structure contains a set of flags.  The first 2 bits in that flag identify the material type (opaque, etc).
//       We must ensure all other material flags are written to byte addresses after these first two bits.  Use the COMMON_MATERIAL_FLAG_TYPE_OFFSET(x) 
//       macro, and ensure there is enough storage in the flags to represent desired bits accordingly.

// maximum value for thin film thickness in nanometers
#define OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS (1500.0f)
// bits for flags field in OpaqueSurfaceMaterial
#define OPAQUE_SURFACE_MATERIAL_FLAG_USE_THIN_FILM_LAYER (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(0))
#define OPAQUE_SURFACE_MATERIAL_FLAG_ALPHA_IS_THIN_FILM_THICKNESS (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(1))
#define OPAQUE_SURFACE_MATERIAL_FLAG_IGNORE_ALPHA_CHANNEL (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(2))
#define OPAQUE_SURFACE_MATERIAL_FLAG_IS_RAYTRACED_RENDER_TARGET (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(3))
#define OPAQUE_SURFACE_MATERIAL_FLAG_HAS_DISPLACEMENT (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(4))
#define OPAQUE_SURFACE_MATERIAL_FLAG_USE_SECONDARY_TEXTURE_FOR_OPACITY (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(5))
// Multiply the albedo by the secondary texture sampled at the terrain detail coordinate
// (see KenshiTerrainArgs).
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_TERRAIN_BLEND (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(6))
// Kenshi's character composite stores body and head colour in separate atlases.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_CHARACTER_HEAD (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(7))
// Host and shaders compile different copies of this file: the host resolves include/ first, the
// shaders use src/dxvk/shaders/rtx/utility/, and src/dxvk/rtx/utility/ is not compiled. Keep all
// three in sync - the flag values are a host/GPU contract.
// Neither normal flag set = octahedral (Remix's encoding, used by USD replacements). Kenshi's
// normal maps are tangent-space RGB; decoding them as octahedral rotates the relief by 45 degrees.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_NORMAL_RGB (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(9))
// Kenshi's DXT5nm variant: X in alpha, Y in green, Z reconstructed.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_NORMAL_DXT5NM (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(10))
// The diffuse texture's alpha is Kenshi's gloss, scaled by glossMult. Set only where the pixel
// shader declares glossMult, which proves the alpha is gloss rather than coverage. The multiplier
// rides in roughnessConstant.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_GLOSS_IN_ALPHA (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(11))
// The pixel shader blends two complete texture sets by vertex-colour alpha:
//   albedo = lerp(diffuseMap2, base_map, COLOR0.a)
// (diffuseMap2/normalMap2/metalMap2, or base_map2/... naming). The second albedo rides in
// secondaryTextureIndex, which no material with this flag uses otherwise.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_DUAL_TEXTURE_SET (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(8))
// Kenshi's two-colour recolour (faction-tinted armour and equipment). A two-channel mask picks
// where each authored colour applies; each colour's weight sets how much source luminance survives:
//   lum   = dot(albedo, 1)/3
//   w1    = lerp(lum, 1, color1.a);  layer = lerp(albedo, color1.rgb*w1, mask.r)
//   w2    = lerp(lum, 1, color2.a);  albedo = lerp(layer, color2.rgb*w2, mask.g)
//   metalness = mask.b            (these materials bind no metal_map)
// Mask in secondaryTextureIndex, color1 in albedoOpacityConstant, color2 in emissiveColorConstant
// + emissiveIntensity (dead on a textured non-emissive material; emission is forced off).
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_COLOR_MASK (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(12))
// Worn-clothing layer of the character composite:
//   vestLum  = dot(vest.rgb, 1)/3
//   clothing = lerp(vest.rgb, colour.rgb * vestLum, mask.r)
//   albedo   = lerp(skinComposite, clothing, vestNormal.a)
// vestNormal.a is a per-texel coverage mask (clothing and skin share one mesh and draw). Vest
// diffuse in metallicTextureIndex, vest normal in roughnessTextureIndex, recolour mask in
// emissiveColorTextureIndex, clothing colour in albedoOpacityConstant.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_CHARACTER_VEST (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(13))
// This flag field is FULL: `flags` is a uint16 and COMMON_MATERIAL_FLAG_TYPE_OFFSET adds 2, so
// offsets 0-13 occupy bits 2-15. A 15th flag compiles but never reaches the GPU. Per-draw markers
// (e.g. under-construction buildings) go in RtSurface flags0 instead.


#define OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_HAS_HEIGHT_TEXTURE (1 << 0)
#define OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_USE_THIN_FILM_LAYER (1 << 1)
// flags overlap with type field when in gbuffer, which occupies last 2 bits.
#define OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_MASK 0x3F


// Note: Bits for flags field in TranslucentSurfaceMaterial and TranslucentSurfaceMaterialInteraction
// If set, then the texture bound to transmittanceOrDiffuseTextureIndex is an albedo map for the diffuse layer
#define TRANSLUCENT_SURFACE_MATERIAL_FLAG_USE_DIFFUSE_LAYER (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(0))

#endif // ifndef SHARED_CONSTANTS_H
