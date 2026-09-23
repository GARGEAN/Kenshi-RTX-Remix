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
// DX11_V396_KENSHI_TERRAIN: multiply the albedo by the secondary texture sampled
// at the terrain detail coordinate (see KenshiTerrainArgs). `flags` is a
// uint16_t on both sides and this is bit 8, so it is the first of the free half.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_TERRAIN_BLEND (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(6))
// Kenshi's character composite stores body and head colour in separate atlases.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_CHARACTER_HEAD (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(7))
// DX11_V490_KENSHI_NORMAL_ENCODING. THIS is the copy the HOST code compiles
// against - `-I..\include` precedes `-I..\src\dxvk\shaders` on the command line,
// so `#include "rtx/utility/shared_constants.h"` from rtx_materials.h resolves
// here, while the SHADERS compile against src/dxvk/shaders/rtx/utility/. There
// is a third copy under src/dxvk/rtx/utility/ that nothing compiles. All must
// agree: the flag values are a host/GPU contract.
//
// Neither flag set = octahedral, Remix's own encoding, so USD replacements are
// untouched. Game normal maps are ordinary tangent-space RGB, and decoding one
// as octahedral rotates every perturbation by 45 degrees while still mapping
// flat to flat - which looks like working relief pointing the wrong way.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_NORMAL_RGB (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(9))
// Kenshi's DXT5nm variant: X in alpha, Y in green, Z reconstructed. Defined by
// creature.material alone.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_NORMAL_DXT5NM (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(10))
// DX11_V533_KENSHI_OBJECT_GLOSS: this material's diffuse texture carries Kenshi's
// GLOSS in its alpha channel, scaled by the shader's `glossMult`. Set only where
// the pixel shader actually declares that constant, which is what proves the
// alpha is gloss rather than coverage. The multiplier itself rides in
// roughnessConstant, whose ordinary meaning is dead once this flag is set.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_GLOSS_IN_ALPHA (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(11))
// DX11_V550_KENSHI_DUAL_TEXTURE_SET: this material's pixel shader carries TWO
// complete texture sets and blends between them with the vertex colour's alpha:
//   albedo = lerp(diffuseMap2, base_map, COLOR0.a)
// Ten of Kenshi's pixel shaders do this (`diffuseMap2`/`normalMap2`/`metalMap2`,
// or `base_map2`/... under the other naming). The second albedo rides in
// secondaryTextureIndex, which no material setting this flag uses for anything
// else - terrain and the character head multiplex the same field the same way.
// Offset 8 was the one gap left in the flag range.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_DUAL_TEXTURE_SET (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(8))
// DX11_V552_KENSHI_COLOR_MASK: Kenshi's two-colour recolour, the mechanism
// behind faction-tinted armour and equipment. A two-channel mask texture picks
// where each of two authored colours is applied, and each colour carries a
// weight that decides how much of the source texture's luminance survives:
//   lum   = dot(albedo, 1)/3
//   w1    = lerp(lum, 1, color1.a);  layer = lerp(albedo, color1.rgb*w1, mask.r)
//   w2    = lerp(lum, 1, color2.a);  albedo = lerp(layer, color2.rgb*w2, mask.g)
//   metalness = mask.b            (these materials bind no metal_map at all)
// The mask rides in secondaryTextureIndex, color1 in albedoOpacityConstant and
// color2 in emissiveColorConstant + emissiveIntensity - all dead on a textured,
// non-emissive material, and emission is forced off when this flag is set.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_COLOR_MASK (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(12))
// DX11_V554_KENSHI_CHARACTER_VEST: the worn-clothing layer of Kenshi's
// character composite - what makes a body's texture change with what it wears.
//   vestLum  = dot(vest.rgb, 1)/3
//   clothing = lerp(vest.rgb, colour.rgb * vestLum, mask.r)
//   albedo   = lerp(skinComposite, clothing, vestNormal.a)
// vestNormal.a is a per-texel COVERAGE mask, so clothing and skin share one
// mesh and one draw. Rides in three indices a character material never uses -
// vest diffuse in metallicTextureIndex, vest normal in roughnessTextureIndex,
// the recolour mask in emissiveColorTextureIndex - and the clothing colour in
// albedoOpacityConstant, dead on any textured material.
#define OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_CHARACTER_VEST (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(13))
// DX11_V760/V761: an under-construction building is marked on the SURFACE
// (RtSurface flags0 bit 5), not here. `flags` is a uint16 and
// COMMON_MATERIAL_FLAG_TYPE_OFFSET adds 2, so offsets 0-13 occupy bits 2-15 and
// this field is FULL. V760 defined offset 14 - bit 16 - which no compiler and no
// assert objected to and which simply never reached the GPU, so the shader never
// took the branch. Do not add another flag here without widening the field.


#define OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_HAS_HEIGHT_TEXTURE (1 << 0)
#define OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_USE_THIN_FILM_LAYER (1 << 1)
// flags overlap with type field when in gbuffer, which occupies last 2 bits.
#define OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_MASK 0x3F


// Note: Bits for flags field in TranslucentSurfaceMaterial and TranslucentSurfaceMaterialInteraction
// If set, then the texture bound to transmittanceOrDiffuseTextureIndex is an albedo map for the diffuse layer
#define TRANSLUCENT_SURFACE_MATERIAL_FLAG_USE_DIFFUSE_LAYER (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(0))

#endif // ifndef SHARED_CONSTANTS_H
