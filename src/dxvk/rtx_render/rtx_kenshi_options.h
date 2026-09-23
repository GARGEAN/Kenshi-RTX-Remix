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
#include "../../util/util_kenshi_telemetry.h"

// RECONSTRUCTED 2026-08-25. This file was destroyed (truncated to 0 bytes) by an
// over-broad scripted cleanup edit, and no
// copy existed on disk. Recovery sources, all verified:
//
//   NAMES + DECLARATION ORDER + DESCRIPTIONS - recovered verbatim from the
//     registered option strings in d3d11.v514-color0-interleaved-layout.dll.
//     Declaration order came from the rdata offsets; each description sits
//     immediately BEFORE its own name, which is how the pairing was resolved.
//   TYPES - recovered from the call sites (every option is read into a typed
//     local or used as an if-condition, so all 29 are unambiguous).
//   DEFAULTS - six were recovered exactly. The rest could not be, and are
//     marked below. Audit any marked default before relying on behaviour that
//     depends on that option being UNSET; every option the user actually
//     tunes is pinned explicitly in game-test/dxvk.conf and is unaffected.

namespace dxvk {

  struct KenshiOptions {
    friend class ImGUI;

    static void telemetryChanged(DxvkDevice*) {
      kenshi_telemetry::publish(kenshiTelemetry());
    }
    RTX_OPTION_ARGS("rtx.dx11", bool, kenshiTelemetry, false,
      "Enable Kenshi project diagnostics and diagnostic capture controls.\n"
      "Off disables collection as well as diagnostic output. Errors and crash handling remain available.\n"
      "Leave off for normal gameplay. Detailed tools still require their individual controls.",
      args.onChangeCallback = &telemetryChanged);

    RTX_OPTION("rtx.dx11", bool, kenshiInstanceAntiCulling, true,
      "Keep nearby buildings and props available for off-screen shadows and lighting.\n"
      "Applies during gameplay. Off restores normal frustum culling for these objects.\n"
      "Static landscape decorations use Terrain Features; ground tiles use Terrain Anti-Culling.\n"
      "Missing rigid instances expire promptly in either mode, allowing a clean performance comparison.");

    RTX_OPTION("rtx.dx11", float, kenshiInstanceAntiCullingRadius, 1000.0f,
      "Off-screen retention radius in game units, measured from the camera to the nearest point of each object's bounding box.\n"
      "Applies during gameplay. Outside this radius, normal frustum culling applies; visible distant objects remain eligible.\n"
      "Larger values retain more geometry and cost more performance. Supported range: 100 to 10000.");

    RTX_OPTION("rtx.dx11", bool, kenshiTerrainFeatureAntiCulling, true,
      "Keep nearby off-screen static landscape decorations available for shadows and lighting.\n"
      "Includes rocks, cliffs, landmark formations, static debris and rigid scenery batches.\n"
      "Changes live. Off restores their native frustum culling. Wind-animated batches are excluded.");

    RTX_OPTION("rtx.dx11", float, kenshiTerrainFeatureAntiCullingRadius, 1000.0f,
      "Terrain-feature radius in game units from the camera to the nearest point of each object's bounding box.\n"
      "Independent of buildings and ground tiles. Changes live. Supported range: 100 to 10000.\n"
      "Larger values retain more geometry and can noticeably increase CPU, GPU and memory cost.");

    RTX_OPTION("rtx.dx11", bool, honourGameCullMode, true,
      "Carry the application's D3D11 rasterizer cull mode into ray tracing instead of forcing all captured geometry to be two-sided.\n"
      "Two-sided-everything hides one-sided geometry in reverse: a wall modelled with its normals facing inward (Kenshi's building interior shells) becomes opaque from the outside, so interiors cannot be seen into. Carrying the real cull mode restores the game's intent for primary rays while Remix's own rtx.enableCullingInSecondaryRays (off by default) keeps shadow rays uncelled, so those surfaces still block light.\n"
      "Geometry the game itself declares as two-sided (cull_hardware none) is unaffected, as are alpha-blended draws, which the instance manager keeps double-sided regardless. Set to False to restore the old blanket two-sided behaviour without rebuilding.");

    RTX_OPTION("rtx.dx11", bool, kenshiLightDrive, true,
      "Create real Remix lights from Kenshi's deferred light-volume draws (campfires, lamps, ceiling spotlights).\n"
      "Position, colour, radius, brightness, cone angles and cone exponent are all read from the light shader's own named constants, so every light in the game is covered by one rule and nothing is per-type tuned. The generic rtx.dx11.deferredLightVolumeCapture path cannot serve this game: Kenshi's light vertex shader exposes only a combined world-view-projection matrix, so no world transform can be recovered from it.\n"
      "Set to False to fall back to no local lights at all.");

    RTX_OPTION("rtx.dx11", float, kenshiLightIntensityScale, 4.0f,
      "Brightness multiplier for all lights recovered from Kenshi's light volumes. Linear: 2.0 is twice as bright.\n"
      "The single knob to turn if local lights read too dim or too hot. It does not affect the sun or the sky, and it does not change how bright the lights are relative to each other - use rtx.dx11.kenshiLightUsePower for that.");

    RTX_OPTION("rtx.dx11", bool, kenshiLightUsePower, true,
      "Let each light's own 'power' constant scale its brightness, so a dim lamp and a bright spotlight keep their relative strengths (measured range 0.56 to 0.79).\n"
      "Set to False to give every light the same brightness for its radius, which is worth trying if lights look inconsistent.");

    RTX_OPTION("rtx.dx11", uint32_t, kenshiLightMaxPerFrame, 64u,
      "Maximum lights harvested per frame. Kenshi submits only the light volumes that intersect the view, measured at 9 to 17; this is a guard against a pathological scene, not a tuning knob.");

    RTX_OPTION("rtx.dx11", float, skyLightFloor, 0.0f,
      "Minimum radiance the sky probe contributes as LIGHT, so night never goes darker than this. 0 disables it.\n"
      "Kenshi's own raster night ambient is a flat 20% of its daytime ambient and comes from a static cube rather than the sky, so it never falls to black the way a path-traced near-black night sky does. This reinstates that floor.\n"
      "Applied as a minimum after rtx.skyBrightness and rtx.skyProbeLightBrightness, so a daylit sky is already above it and is unaffected. The VISIBLE sky is never touched - only the light it casts.\n"
      "NOTE: this reaches only surfaces that can see the sky. It will not light an enclosed interior, whereas Kenshi's raster ambient cube does.");

    // DEFAULT INFERRED (see file header note).
    RTX_OPTION("rtx.dx11", bool, kenshiAmbientMapDrive, true,
      "Apply Kenshi's own per-region ambient map, so lighting varies between biomes as it does in raster.\n"
      "The map's alpha scales the sun (measured 0.19 to 0.66 across the world) and its rgb tints the ambient/sky light. Both are read from the texture the game itself binds, so this follows mods and needs no per-region data of its own.\n"
      "Set to False to light every region identically, which is the behaviour before this option existed.\n"
      "NOTE: enabling this makes the sun DIMMER in most of the world, since the map's alpha is below 1 nearly everywhere. Expect to raise rtx.dx11.kenshiSunRadiance afterwards.");

    // DEFAULT INFERRED (see file header note).
    // DX11_V533_KENSHI_OBJECT_GLOSS. Kenshi's lighting is a metallic workflow;
    // these feed its authored inputs into Remix's equivalents. Metalness is a
    // straight copy and defaults ON - the flat 0.1 it replaces is simply wrong.
    // Gloss keeps a strength, because the roughness it produces has to sit
    // alongside the material families the bridge cannot read yet.
    // DX11_V540_KENSHI_WETNESS. Weather-driven, so it only shows during rain or
    // near water - a value above zero changes nothing in dry weather inland.
    RTX_OPTION("rtx.dx11", float, kenshiWetness, 0.0f,
      "How strongly Kenshi's own wetness affects path-traced surfaces. The game raises gloss toward 0.5 and "
      "darkens albedo by up to 20% as its weather `wetness` rises and near its water level, on terrain, objects "
      "and characters alike.\n"
      "0 disables it; 1 reproduces the game's own strength; above 1 amplifies.\n"
      "\n"
      "Amplification is usually necessary. Kenshi's wetness peaks at about 0.64 in heavy rain (measured over a "
      "multi-hour run), and its own response curve barely responds below 0.7 - at the game's ceiling a rough "
      "surface's gloss lifts by 0.03, which is invisible. Around 1.5 turns that ceiling into the saturated wet look. "
      "The result is clamped, so no value here can push wetness past the game's own maximum.\n"
      "\n"
      "This is WEATHER state - with clear skies and no water nearby the game's own wetness is zero and no setting "
      "here will show anything.");

    // DX11_V546_COLOR0_TINT. Kenshi's per-vertex/per-instance material tint.
    // This is the universal layer of the game's material-subvariant system:
    // 35 of its 180 pixel shaders end albedo with `albedo * COLOR0.rgb`, and
    // between them they cover most of the objects in a frame.
    //
    // The multiply was already happening, but Remix treats vertex colour as
    // BAKED LIGHTING by default, which normalises the tint by its largest
    // channel and then mixes it 40% toward white - so an authored tint of
    // (0.55, 0.42, 0.30) reached the surface as (1.00, 0.86, 0.73). This makes
    // the treatment match what the data actually is.
    RTX_OPTION("rtx.dx11", bool, kenshiVertexTint, true,
      "Use Kenshi's own per-vertex material tint, the way the game does.\n"
      "Most of the game's object shaders finish their albedo with a multiply by the vertex colour - it is how "
      "Kenshi varies one mesh into many: recoloured armour, tinted building sets, region-appropriate props. "
      "Remix's default is to assume vertex colour is baked lighting and normalise it away, which turns every "
      "tint nearly white; this treats it as the authored albedo it is.\n"
      "Applied only to draws whose pixel shader actually reads COLOR0, read from the shader's own input "
      "signature, so materials the game does not tint are untouched.\n"
      "Set to False to restore the previous washed-out behaviour without rebuilding.");

    // DX11_V556_KENSHI_CHARACTER_GLOSS. Characters were the one family V533
    // could never reach: their shader declares no `glossMult`, so the flag that
    // proves "this alpha is gloss" was never set and they rendered with no gloss
    // at all. The game's own output settles both halves - o0.w is that alpha,
    // and `mov o0.z, l(0)` means a character is never metallic.
    RTX_OPTION("rtx.dx11", float, kenshiCharacterGloss, 1.0f,
      "Gain on Kenshi's own gloss for characters - skin, and the clothing they wear.\n"
      "The game keeps a character's gloss in the alpha of whichever layer covers the texel, and "
      "blends the two by the same coverage mask it uses for the colour, so skin and clothing each keep their "
      "own finish.\n"
      "0 disables the path entirely and leaves roughness exactly as it was, 1 is the faithful translation, "
      "above 1 amplifies.\n"
      "Unlike rtx.dx11.kenshiObjectGlossMultiplier this needs no glossMult constant - the character shader "
      "declares none, which is why object gloss never applied to characters and they had no gloss at all.");

    // DX11_V557/V573_KENSHI_DUST. Biome state, like wetness: in a region whose
    // shader-selected dust amount is zero this changes nothing however high it is set.
    RTX_OPTION("rtx.dx11", float, kenshiDust, 1.0f,
      "Strength of Kenshi's own biome dust - the region-coloured film on buildings, structures and props.\n"
      "The game tints a surface toward its region's dust colour and FLATTENS its normal, gated by a "
      "world-space noise texture, how up-facing the surface is, and its own gloss - so dust settles on tops "
      "and in hollows rather than uniformly.\n"
      "0 disables it, 1 is the game's own strength, above 1 amplifies.\n"
      "This is BIOME state: in a region whose dust amount is zero nothing here will show anything.");

    RTX_OPTION("rtx.dx11", bool, kenshiGameMetalness, true,
      "Use Kenshi's own metal map instead of a flat metallic constant. The game writes its red channel into "
      "its G-buffer metal channel and shades with the same lerp(0.04, albedo, metal) split Remix uses, so this "
      "is a direct copy rather than an approximation. Without it nothing in the game reads as metal.");
    // DX11_V536: a GAIN, not a blend. Kenshi's object gloss maps are bunched near
    // zero - measured on storages_D_HI.dds, 69% of blocks sit below 0.12 - so a
    // faithful translation leaves most of a surface at the legacy roughness and
    // the shiny minority barely separated. A multiply is the right tool for that
    // shape precisely because near-zero stays near-zero however hard it is
    // pushed, so matte stays matte while the tail separates quickly. The cost is
    // clipping: past 1/multiplier every texel reads equally glossy.
    //
    // A gamma would avoid the clipping but lifts the matte floor with everything
    // else, which makes the whole object glossy - the worse artefact of the two.
    RTX_OPTION("rtx.dx11", float, kenshiObjectGlossMultiplier, 0.0f,
      "Gain on Kenshi's per-object gloss before it becomes roughness. The game stores gloss in the alpha of the "
      "diffuse texture scaled by the shader's glossMult, and shades GGX with alpha = 1 - 0.99 * gloss.\n"
      "0 disables the path entirely (roughness is left exactly as it was), 1 is the faithful translation, and "
      "above 1 amplifies - useful because the game's maps are bunched near zero. Values past 1 clip the glossiest "
      "texels together.\n"
      "Applies only to materials whose pixel shader declares glossMult; on any other material the diffuse alpha is "
      "coverage, not gloss. Terrain has its own separate control and is unaffected.");

    RTX_OPTION("rtx.dx11", bool, kenshiNormalMaps, true,
      "Use the game's own normal maps. Kenshi binds one on nearly every surface material, but until now they were discarded - normal maps only ever reached Remix from USD replacements, so the game rendered with geometric normals alone (terrain excepted, which has its own path).\n"
      "The map is found by its name in the shader rather than by texture slot, so one rule covers every material. No tangent data is needed from the game: Remix derives a UV-aligned tangent frame per hit.\n"
      "Creatures are not included yet - their normal maps use a different encoding (X in alpha) and would light incorrectly if read as ordinary ones.\n"
      "Turn off if the game's own maps look worse than flat shading; they were authored in 2013 for a much flatter raster ambient.");

    RTX_OPTION("rtx.dx11", bool, kenshiNormalInvertGreen, false,
      "Invert the normal map's green channel on every material at once.\n"
      "Kenshi's own shaders disagree about this: skin.hlsl flips green before decoding and creature.hlsl flips it after, while objects.hlsl, foliage.hlsl and character.hlsl do not. The bridge follows each family's own rule, which is measured from the shipped bytecode and certain.\n"
      "What is NOT measured is the absolute convention - whether Remix's UV-derived bitangent runs the same way as the binormal stream Kenshi's exporter wrote. If every surface in the game reads inverted (bumps as dents, rivets as holes, lit from below), this flips the whole set in one move. If only armour and animals read inverted, the per-family rule is wrong instead and this will not help.\n"
      "Diagnostic control. The default follows the game.");

    // VERIFIED 2026-08-25 from the V515 dev menu ("Particle Billboards", grey dot
    // = default). The V498 journal note about particle routing "defaulted OFF" is stale.
    RTX_OPTION("rtx.dx11", bool, kenshiParticleCategory, true,
      "Treat Kenshi's dust, haze and effect billboards as particles, so they reach Remix's unordered transparency path instead of being committed as solid surfaces.\n"
      "Without this only ADDITIVE billboards reach that path (through their blend mode), while standard alpha-blended ones - including the world-spanning ambient haze - are resolved as opaque hits. Since their albedo has opacity premultiplied into it, a faint dust quad then renders as a near-black solid, which is what makes stacked dust blacken and show hard quad edges.\n"
      "Matched on the three particle PIXEL shaders specifically, so foliage and tree card batches, which share the same vertex shaders, keep exact geometry and are unaffected.\n"
      "Also restores particle soft-blending, which fades a billboard out as it approaches geometry instead of showing a hard intersection line.\n"
      "Selected by PROPERTY rather than by a list of shaders, so it covers every alpha-blended effect in the game - dust gusts, smoke, haze - in every region. Foliage is excluded because its cutouts write depth and alpha-test; terrain and sky are excluded explicitly.");

    // Default stated by the option's own description ("Off by default").
    RTX_OPTION("rtx.dx11", bool, kenshiParticleBillboards, false,
      "Let Remix rebuild Kenshi's particle billboards as orientation-corrected intersection primitives.\n"
      "Off by default. Kenshi's billboards are already built camera-facing by the CPU every frame, so the correction has nothing to do, and the rebuild path mutates the instance mask and dirties the BLAS on every frame in which the quad count changes - which for an OGRE BillboardSet is every frame.\n"
      "Turn on only to test that path; it is implicated in a GPU device fault at save load.");

    // Default stated by the option's own description ("1.0 is what the game computes").
    RTX_OPTION("rtx.dx11", float, kenshiFogVolumeDensityScale, 1.0f,
      "Multiplier on the density of Kenshi's local fog volumes. 1.0 is what the game computes; raise for thicker biome fog.");

    // DEFAULT INFERRED (see file header note).
    RTX_OPTION("rtx.dx11", bool, kenshiFogVolumes, true,
      "Recognise Kenshi's local fog volumes (the biome fog clouds) and keep their hull geometry out of the ray-traced scene.\n"
      "Those hulls are invisible helper meshes in the original renderer - the fog is computed from them analytically - so admitting them produces solid shells and grey blobs instead of fog. Off restores the previous behaviour.");

    // DEFAULT INFERRED (see file header note).
    RTX_OPTION("rtx.dx11", bool, kenshiFogDrive, true,
      "Reproduce Kenshi's own distance fog, using the fog colour and density the game itself computes for the current biome and weather.\n"
      "The near/far behaviour is the game's own curve rather than an exponential, so it should match what the raster renderer draws. The far half of the effect is tinted by the ray-traced sky in the view direction.\n"
      "Off renders with no distance fog at all, which is the behaviour before this option existed.");

    // DEFAULT INFERRED (see file header note).
    RTX_OPTION("rtx.dx11", float, kenshiFogSkyBlend, 1.0f,
      "Strength of the sky-tinted half of Kenshi's distance fog (the atmospheric scattering term), 0 disables it and leaves only the global fog colour.");

    // DEFAULT INFERRED (see file header note). The description states that 1.0
    // is the value which matches the visible sky.
    RTX_OPTION("rtx.dx11", float, kenshiFogBrightness, 1.0f,
      "Brightness of Kenshi's distance fog, on top of the automatic match to rtx.skyBrightness.\n"
      "The game's fog colour is a display-space value composited over a finished image, so it has to be brought into the path tracer's radiance space before use or it blows out to white. 1.0 matches the visible sky; raise or lower to taste.");

    // Default stated by the option's own description ("1.0 is what the game computes").
    RTX_OPTION("rtx.dx11", float, kenshiFogDensityScale, 1.0f,
      "Multiplier on Kenshi's own fog density. 1.0 is what the game computes; raise for thicker fog, lower for clearer air.");

    // Written automatically every frame; the default is inert.
    RTX_OPTION("rtx.dx11", Vector3, kenshiFogColour, Vector3(0.0f, 0.0f, 0.0f),
      "Fog colour recovered from Kenshi's own fog pass, already scaled by the game's sun term. Written automatically; setting it by hand has no lasting effect.");

    // Written automatically every frame; the default is inert.
    RTX_OPTION("rtx.dx11", Vector3, kenshiFogParams, Vector3(0.0f, 0.0f, 0.0f),
      "Fog parameters recovered from Kenshi's own fog pass: x = fog colour alpha, y = fog density, z = atmospheric ramp start. Written automatically; setting it by hand has no lasting effect.");

    // Written automatically every frame; the default is inert.
    RTX_OPTION("rtx.dx11", Vector3, kenshiFogHorizon, Vector3(0.0f, 0.0f, 0.0f),
      "Horizon colour recovered from Kenshi's own fog pass (horizonClouds.rgb). Written automatically; setting it by hand has no lasting effect.");

    // Written automatically every frame; the default is inert.
    RTX_OPTION("rtx.dx11", Vector3, kenshiFogExtra, Vector3(0.0f, 0.0f, 0.0f),
      "Extra fog values recovered from Kenshi's own fog pass: x = horizon colour blend, y = far clip distance. Written automatically; setting it by hand has no lasting effect.");

    // Written automatically every frame; the default is inert.
    RTX_OPTION("rtx.dx11", Vector3, kenshiSkyXCameraPos, Vector3(0.0f, 0.0f, 0.0f),
      "SkyX atmosphere-space camera position (uCameraPos). NOT the world camera - a small height above the inner radius. Written automatically.");

    // Written automatically every frame; the default is inert.
    RTX_OPTION("rtx.dx11", Vector3, kenshiSkyXInvWaveLength, Vector3(0.0f, 0.0f, 0.0f),
      "SkyX inverse wavelength^4 (uInvWaveLength), the term that makes Rayleigh scattering blue. Written automatically.");

    // Written automatically every frame; the default is inert.
    RTX_OPTION("rtx.dx11", Vector3, kenshiSkyXScatter, Vector3(0.0f, 0.0f, 0.0f),
      "SkyX scattering constants packed as (uKrESun, uKr4PI, uKm4PI). Written automatically.");

    // Written automatically every frame; the default is inert.
    RTX_OPTION("rtx.dx11", Vector3, kenshiSkyXScaleParams, Vector3(0.0f, 0.0f, 0.0f),
      "SkyX scale constants packed as (uScale, uScaleDepth, uScaleOverScaleDepth). Written automatically.");

    // Written automatically every frame; the default is inert.
    RTX_OPTION("rtx.dx11", Vector3, kenshiSkyXGeometry, Vector3(0.0f, 0.0f, 0.0f),
      "SkyX geometry constants packed as (uInnerRadius, uSkydomeRadius, uExposure). Written automatically.");

    // Written automatically every frame; the default is inert.
    RTX_OPTION("rtx.dx11", Vector3, kenshiFogSunDir, Vector3(0.0f, 0.0f, 0.0f),
      "Unclamped sun direction read from the fog pass (sunDirectionReal), used by the scattering phase function. Zero disables scattering. Written automatically.");

    // Written automatically every frame; the default is inert.
    RTX_OPTION("rtx.dx11", float, kenshiFogAtmoEnd, 0.0f,
      "Atmospheric ramp end distance recovered from Kenshi's own fog pass. Written automatically; setting it by hand has no lasting effect.");

    // DEFAULT INFERRED (see file header note). The user's tuned value is 0.05
    // and is pinned in dxvk.conf, so this default is not in use on that setup.
    RTX_OPTION("rtx.dx11", float, kenshiParticleLightIntensity, 1.0f,
      "Brightness of Kenshi's billboard particles - dust, smoke, haze.\n"
      "Their light comes from the volumetric radiance cache rather than from the sun directly, so they do not track the sun and sky scalars and can read brighter than the game's raster does. This scales only those particles: no other geometry, decal or reflection is affected, so it can be trimmed against raster without disturbing anything else.");

    // DX11_V634_KENSHI_RAIN_BRIGHTNESS. Rain is a forward LDR additive pass in
    // raster but becomes HDR emission before Remix's tone mapper, so its source
    // needs its own calibrated scale rather than a unit emission value.
    RTX_OPTION("rtx.dx11", float, kenshiRainBrightness, 0.05f,
      "Brightness of Kenshi's rain billboards after their captured sun-height factor.\n"
      "Rain is an additive forward raster contribution but becomes HDR emission in path tracing, so 1.0 is much too strong under Remix's tone mapper. 0.05 is the raster-matched default; 0 hides rain completely. This affects rain only, not dust, smoke, haze, lighting or reflections.");

    // Written automatically every frame when kenshiAmbientMapDrive is on; a
    // neutral tint is the correct inert default.
    RTX_OPTION("rtx.dx11", Vector3, kenshiAmbientTint, Vector3(1.0f, 1.0f, 1.0f),
      "Per-region ambient tint recovered from Kenshi's ambient map. Written automatically every frame when rtx.dx11.kenshiAmbientMapDrive is on; setting it by hand has no lasting effect.");

    // DX11_V521. Kill switch for the baked-projection blood path. ON by
    // default; the V517-V519 boot crash was a stale precompiled header, not
    // this code, and it is fixed. Set False to rule blood out without a rebuild.
    RTX_OPTION("rtx.dx11", bool, kenshiBlood, true,
      "Reproduce Kenshi's own character, creature and severed-limb blood in the path tracer.\n"
      "The cylindrical projection is baked once per mesh from the BIND-POSE position and normal buffers - the same inputs the game's own vertex shader reads - and carried on a dedicated per-mesh buffer. It costs nothing per frame and is independent of pose, camera and blood amount.\n"
      "Turn off if blood misprojects or costs memory; the rest of the character path is unaffected.");

    // DX11_V525. Kenshi's ground blood decals.
    RTX_OPTION("rtx.dx11", bool, kenshiTerrainBlood, true,
      "Reproduce Kenshi's ground blood decals on path-traced terrain.\n"
      "The game draws these by re-rendering the terrain mesh once per splat with a world-XZ projected texture; here they are collected as a small list of world rects and blended into the terrain albedo at the hit, so no extra geometry enters the ray-traced scene.\n"
      "Shares rtx.dx11.kenshiBloodRoughness with character blood.");

    // DX11_V522. Kenshi's raster has no PBR, so blood inherits skin roughness
    // and vanishes into it under path-traced lighting. Wet blood is a smooth
    // dielectric; this makes it read as wet.
    RTX_OPTION("rtx.dx11", float, kenshiBloodRoughness, 0.2f,
      "Perceptual roughness of blood on characters, creatures and severed limbs.\n"
      "Blended by the same influence that blends the blood colour, so it only affects texels blood actually covers. Low values read as wet and glossy; around 0.6 reads as dried. The game's own raster has no roughness for blood at all, which is why blood otherwise disappears into the skin under path-traced lighting.");

    // DX11_V580. Admit Kenshi's water instead of excluding it as a backdrop.
    RTX_OPTION("rtx.dx11", bool, kenshiWaterAdmit, false,
      "Admit Kenshi's water surfaces into the ray-traced scene instead of leaving them on the raster layer.\n"
      "The water vertex shader (c637a111) was excluded as a whole-map backdrop, so the game draws its water into a target the Remix composite then overwrites - cost with no image. Its mesh carries POSITION and an unused NORMAL and nothing else: every texture coordinate is computed in the vertex shader from world XZ, so with this on the water arrives with NO UVs and therefore no textures, shading as a plain white plane wherever terrain does not occlude it.\n"
      "Diagnostic for now, off by default. Each distinct water family logs its world placement once as [kenshi-water], which is what decides whether the near-water quads are coplanar with the whole-map distant grid.");

    // DX11_V582. The one look knob for water, until its normal map has a
    // coordinate of its own.
    RTX_OPTION("rtx.dx11", float, kenshiWaterRoughness, 0.05f,
      "Perceptual roughness of Kenshi's water surface. Low values read as a calm mirror; around 0.3 reads as choppy.\n"
      "The game's water is a forward-lit surface whose specular comes from two cubemaps and a planar reflection mask, none of which survive into path tracing - so on the legacy path water inherits rtx.legacyMaterial.roughnessConstant and shades as matte paint in the water's colour. This replaces that constant for water draws only.\n"
      "Carried on the same per-draw field as object gloss, so it is gated by rtx.dx11.kenshiObjectGlossMultiplier being non-zero. Requires rtx.dx11.kenshiWaterAdmit.");

    // DX11_V610. Minimum outgoing-ray clearance, relative to the flat plane.
    RTX_OPTION("rtx.dx11", float, kenshiWaterReflectionClearance, 0.25f,
      "Minimum geometric-plane clearance for water reflection rays, as a fraction of the flat water reflection's clearance.\n"
      "0.05 is the minimum safe value; 1 prevents a ripple from lowering the outgoing ray beneath the flat reflection angle. Ripples whose rays already satisfy the limit remain exactly unchanged. Requires rtx.dx11.kenshiWaterAdmit.");

    // DX11_V611. Live isolation of the rain-ring normal contribution.
    RTX_OPTION("rtx.dx11", bool, kenshiWaterRainRipples, true,
      "Apply Kenshi's animated rain-ring normals to path-traced water.\n"
      "Disable to isolate the rain rings from the base flow ripples without changing the visible rain weather effect.");

    // DX11_V621/V627. Water transmission strength and the simulated-depth mode.
    RTX_OPTION("rtx.dx11", float, kenshiWaterTransparency, 1.0f,
      "0 keeps water fully opaque. With Simulated Depth disabled, values above 0 select Remix's proper translucent-water material and scale its absorption distance. With Simulated Depth enabled, water stays in the opaque primary G-buffer and this value scales the distance over which the bottom fades into the biome-water colour; 1 reproduces Kenshi's own depth range and smaller values make water become opaque sooner. Requires rtx.dx11.kenshiWaterAdmit.");

    RTX_OPTION("rtx.dx11", bool, kenshiWaterSimulatedDepth, false,
      "Keep water in the stable opaque primary G-buffer, but use Kenshi's raster-style depth curve to transmit a continuation ray to the bottom. Submerged surfaces then use the normal path-traced material, direct lighting, shadows and indirect lighting; terrain follows the Secondary Ray Shading setting. Water also receives Kenshi's primary-only glow term. Disable this to retain the existing fully opaque/translucent switch. Requires rtx.dx11.kenshiWaterAdmit.");


    RTX_OPTION("rtx.dx11", float, kenshiWaterGlowBoost, 0.0f,
      "Adds to Kenshi's own water `glow` constant, which drives a primary-only unlit term of waterColour * glow * (1 - scum), weighted by the depth blend (data/materials/forward/water.hlsl:210). "
      "Kenshi declares glow once, as 0.0 in WaterFP's default_params, and no water material overrides it - so the raster term is inert too and this is the only way to reach it. "
      "The COLOUR stays per-biome regardless of this value: it comes from the world-wide colour map at the water's own map UV, not from the constant, so one global amount still produces different colours in different waters. "
      "Additive rather than an override so that a water body which ever does set a non-zero glow at runtime keeps its own variation, with this riding on top. "
      "Primary rays only - water never becomes an emitter for shadows, NEE or indirect light. Requires rtx.dx11.kenshiWaterAdmit and rtx.dx11.kenshiWaterSimulatedDepth.");

    RTX_OPTION("rtx.dx11", float, kenshiWaterColourGain, 1.0f,
      "Multiplies the water surface's diffuse albedo, which is the only route by which biome colour reaches the lit water term. "
      "Kenshi's raster water is not lit physically: lightingFunctions.hlsl multiplies the sun diffuse by PI where a normalised BRDF divides by it, and scales the irradiance probe by 4, so its water diffuse runs roughly an order of magnitude hotter than a correct one. "
      "That gain is why raster shallows show more biome colour over the bottom and raster deep water reads brighter rather than darker. Raise this to recover it; 1.0 is physically correct and matches the previous behaviour. "
      "Unlike Glow this term is LIT, so it still responds to sun angle, time of day and cloud shadow. "
      "Safe above 1 only because simulated-depth water is invisible to secondary rays, so the extra energy cannot bounce onto anything else. "
      "Applies to the water colour ONLY: the scum layer is an ordinary authored albedo that already reaches the image correctly lit, so gaining it as well (which V632 did, by applying this after the scum blend) simply made the film louder than raster's by this factor. "
      "Does not affect the glow term, which uses the raw colour map exactly as the raster shader does. Requires rtx.dx11.kenshiWaterAdmit.");
    // DX11_V614. How far rain rings reach, which the game bounds by geometry
    // and the path tracer does not.
    // Default measured, not assumed: 4000 is the game's own near-patch reach and
    // still left the distant reflections breathing; 1000 is where the user
    // reports the pulsing mostly gone.
    RTX_OPTION("rtx.dx11", float, kenshiWaterRainDistance, 1000.0f,
      "Distance in world units beyond which rain rings fade out of path-traced water. 0 disables the limit.\n"
      "The game only ever draws rain on its NEAR water patches - the whole-map distant grid compiles them out entirely - so its rings never appear further away than a patch reaches, roughly 4000 units. Remix traces one surface for both families, so without this the rings run to the clipmap's full 262144-unit reach, bounded only by the game's own `saturate(viewDir.y*2 - dist*0.0001)` fade, which alone still reaches 20000 units looking straight down.\n"
      "That matters beyond range: the rain map's alpha is a per-drop PHASE, and a mip is an arithmetic mean, so once a pixel's footprint covers many drops the phase collapses to the texture average and every drop in view pulses in unison - the breathing seen on distant reflections. Fading the rings out before their footprint reaches that point is the same bound the game gets for free from its geometry.\n"
      "Fades over the last quarter of the range so no ring edge is visible. Requires rtx.dx11.kenshiWaterAdmit and rtx.dx11.kenshiWaterRainRipples.");

    // DX11_V605-V607. One-build visual isolation of the water normal stages.
    RTX_OPTION("rtx.dx11", uint32_t, kenshiWaterNormalTelemetry, 0u,
      "Select a Kenshi water-normal diagnostic path.\n"
      "0: current complete water shading with bent normal, 1: geometric-flat final normal, 2: ripple-only normal without bending, 3: complete water normal without bending, 4: DC-centred ripple-only normal without bending, 5: DC-centred ripple-only normal with bending, 6: original ripple-only normal with bending, 7: DC-centred complete water normal with bending.\n"
      "Modes 1-7 are diagnostic and require rtx.dx11.kenshiWaterAdmit.");

    // DX11_V583. The water vertex shader declares NORMAL and never reads it.
    RTX_OPTION("rtx.dx11", bool, kenshiWaterFlatNormal, true,
      "Shade Kenshi's water from its flat triangle normal instead of the normal stream in its vertex buffer.\n"
      "The water vertex shader declares a NORMAL input and never reads it - every normal the surface actually uses is built in the pixel shader from the ripple maps - so whatever the mesh carries in that stream is unconstrained. Sourced as a shading normal it makes a flat plane shade as though it were rough and grainy, whatever roughness is set.\n"
      "Only affects water, and only while rtx.useInputAssemblerNormals is on. Turn off to shade water from the mesh's own normals again.");

    // DX11_V587. Which of Kenshi's two water families reaches the RT scene.
    RTX_OPTION("rtx.dx11", bool, kenshiWaterNearPatches, false,
      "Trace the per-zone near-water patches instead of the whole-map water grid.\n"
      "Both families sit at exactly the same height, so admitting both puts coplanar surfaces in one BVH and the water shimmers. The grid is one 525000-unit mesh whose vertex coordinates reach 262500, and the hit position interpolated across it carries an absolute error of about 0.03 units - visible as a world-locked lattice on a mirror-flat surface at grazing angles, which every shadow and reflection ray then inherits through its origin. The near patches are small local meshes whose coordinates are thousands of times smaller, so they do not have that error.\n"
      "They are ALPHA BLENDED, so Remix routes them through unordered transparency and they will differ from the grid in ways unrelated to that lattice. Requires rtx.dx11.kenshiWaterAdmit.");

    // DX11_V589. Replace Kenshi's water grid with a generated clipmap.
    RTX_OPTION("rtx.dx11", bool, kenshiWaterClipmap, false,
      "Trace a generated clipmap in place of Kenshi's whole-map water grid.\n"
      "Remix packs ray-hit barycentrics into two 16-bit unorms, so every hit position quantizes at triangle-edge/65535. Kenshi's water grid has 5250-unit quads, which puts that step at about 0.1 units - a world-locked lattice that shadow and reflection ray origins inherit, and the reason water pixelates where terrain (30-unit triangles) does not. This substitutes a mesh with 128-unit quads near the camera, coarsening outward in six levels to the same 262144-unit reach, so the step is about 0.003 units where detail is visible and only relaxes where it cannot be seen.\n"
      "About 156k triangles, built once and cached; the instance follows the camera snapped to a 4096-unit grid so its BLAS is never rebuilt. Texture coordinates come from world position, so the surface does not slide as it moves. Requires rtx.dx11.kenshiWaterAdmit and has no effect with Near-Water Patches on.");

    // ---------------------------------------------------------------------
    // Diagnostic log volume. Config-file only, deliberately not in the dev
    // menu: a run has to be started with the right verbosity, not switched
    // mid-session. Set these in game-test/rtx.conf (or dxvk.conf).
    //
    // Measured 2026-09-05 on a 14-minute run (d3d11.8816.log, 47 MB): the
    // submit summary was 47% of it and the draw-transition telemetry 38%.
    // ---------------------------------------------------------------------

    // DX11_V636_KENSHI_INTERIOR_CLIP. Kenshi hides terrain, rock and vegetation
    // that intrudes into a building interior by rendering a mask shell into a
    // depth-range target and discarding fragments whose view depth lands inside
    // the slab. That is screen-space and view-ray-local, so it cannot be ported
    // to a path tracer at all - see journal chapter 16. These reproduce the
    // effect as a world-space volume test in isSurfaceClipped(), which every
    // ray type already routes through.
    // V640: the box approximation over-culls outside authored shells, and the
    // camera-distance transform heuristic is not a valid placement proof.
    // Leave the experiment opt-in until shell-accurate placement is verified.
    RTX_OPTION("rtx.dx11", bool, kenshiInteriorClip, false,
      "Cull terrain and vegetation that intrudes into building interiors.\n"
      "Only ever active while Kenshi is actually drawing an interior mask shell, so it costs nothing outdoors. Off restores the pre-V636 behaviour, where that geometry is visible through the floor of every building you enter.");

    // Live knob, tuned by eye. The captured shell is an authored mesh whose
    // bounds are approximated by an oriented box, so a small inflation closes
    // seams at the wall line and a small deflation pulls the cut back inside if
    // it eats terrain that should stay visible from outside.
    RTX_OPTION("rtx.dx11", float, kenshiInteriorClipBias, 0.0f,
      "Inflate (+) or shrink (-) the interior cull volume, in Kenshi object units.\n"
      "The volume is an oriented bounding box fitted to the game's own mask shell. Positive values cull a little beyond the walls, negative values pull the cut inward. Kenshi's interiors are see-through from outside, so over-culling is directly visible as a hole in the terrain - if you see one, come down from 0.");

    // DX11_V643 excluded the `interiorMask` permutations by default: a bounding
    // BOX could not tell a wall lying ON the shell from terrain lying inside it,
    // so it shaved slivers off the buildings themselves.
    //
    // DX11_V769 turns it on. V650 retired the box - the cull volume is now the
    // authored mask shell's own triangles, tested by signed vertical crossings
    // against the ~4 triangles binned under the hit point - which is exactly the
    // "volume that follows the authored shell closely" this text asked for. The
    // operator verified it live on 2026-09-20 against the V768 pair with the
    // option forced on from rtx.conf: rain culled correctly indoors, and no
    // cracks, missing armour or other collateral in the families this admits.
    //
    // Turning it on is what gives BROAD RASTER PARITY, because the name split in
    // d3d11_rtx.cpp puts every `interiorMask` consumer in one bucket:
    //   t1  forward/basic.hlsl CLIP_INTERIOR - ONE pixel shader, shared by all 41
    //       ambient weather particle materials, including all five rain ones.
    //       Zero risk: every one of them is `scene_blend add`, `depth_write off`.
    //   t3  deferred/objects.hlsl   - building parts, props, armour.
    //   t6  deferred/triplanar.hlsl - rock and blended world meshes.
    // Raster clips all three; leaving this off left all three unculled, which is
    // why rain fell through interior roofs. See journal chapter 16.
    RTX_OPTION("rtx.dx11", bool, kenshiInteriorClipObjects, true,
      "Also cull the object/triplanar/basic CLIP_INTERIOR permutations, not just terrain and grass.\n"
      "ON is raster parity. It admits the three remaining families that sample Kenshi's interior mask: rock and blended world meshes (triplanar), building parts, props and armour (objects), and - through one shared pixel shader - all 41 ambient weather particle materials, which is what keeps rain, ash and blowing dust out of a room you are standing in.\n"
      "This needs the V650 shell-accurate cull volume. Against the old bounding box it shaved slivers off the walls themselves, visible as cracks along the seams where terrain met a building; if you ever see those again, this is the switch to come off.");

    // DX11_V639. Diagnostic, off by default. See the raytrace_args comment.
    RTX_OPTION("rtx.dx11", bool, kenshiInteriorClipDebugAll, false,
      "DIAGNOSTIC. Cull every surface the interior cull considers eligible, ignoring the cull volumes entirely.\n"
      "Everything that samples Kenshi's interior mask - all terrain and all grass, plus the CLIP_INTERIOR object variants - disappears while any interior is active. It is deliberately far too aggressive; it exists to tell a plumbing failure apart from a volume-placement failure. If this makes terrain vanish but the normal path does not, the surface flag and constants are arriving correctly and the volume is in the wrong place.");

    // DX11_V651. Diagnostic for the interior building-part flicker. Config-only
    // (no dev-menu widget), so rtx.conf can never shadow it.
    RTX_OPTION("rtx.dx11", bool, kenshiLogEntrySteal, true,
      "Report cross-object reuse in the two caches that decide what a draw IS: DrawCallCache (geometry/BLAS) and DrawCallTracker (object identity).\n"
      "DrawCallCache matches a draw to a cached entry on material, geometry and bone hashes - none of which include the transform - so two copies of one mesh (a ceiling, a beam, a fan in identical buildings) can claim each other's entry, and which one wins depends on submission order. That is a hypothesis for the PT-only flicker whose rate tracks camera movement and stops when the game is paused with a still camera; this measures whether it actually happens.\n"
      "Silent when the pairing is stable. Costs one distance compare per cache hit.");

    RTX_OPTION("rtx.dx11", float, kenshiEntryStealDistance, 50.0f,
      "How far apart (game units) two objects must be before reusing one's BlasEntry for the other counts as a steal.\n"
      "Well above anything a single object moves in one frame - a character at 10 m/s covers about 8 units per frame at this project's frame rate - and far below the 385-unit separation measured between the two identical buildings that flicker. Lower it to catch closer pairs, raise it if ordinary motion is being reported.");

    // DX11_V634. Master switch for the per-frame submit summary.
    RTX_OPTION("rtx.dx11", bool, kenshiLogSubmitSummary, true,
      "Write the per-frame '[D3D11Rtx] Submit summary' line.\n"
      "This is the first thing to read when geometry goes missing - posCapture=0 means capture silently produced nothing while every admission counter still says accepted - so it is ON by default and self-throttling: a burst of 24 lines, then once every 3 seconds, plus the start of any anomaly run.\n"
      "Turn it off only for a long play session where nothing is being diagnosed.");

    // DX11_V634. Bounds the anomaly bypass, which used to have no bound at all.
    RTX_OPTION("rtx.dx11", uint32_t, kenshiLogAnomalySummaryFrames, 4,
      "How many frames at the START of a frame-anomaly run print a full submit summary.\n"
      "An anomalous frame bypasses the burst budget and the 3-second cadence on purpose, because the event is shorter than the cadence. But the anomaly baseline is only fed by frames that carry a real scene, so a scene that STAYS collapsed - a menu, a load, a genuine loss - freezes the median above the baseline and latches the anomaly on indefinitely. Before this option that printed a full summary every frame for as long as it lasted: 32,206 lines in 14 minutes, measured 2026-09-04.\n"
      "0 disables the bypass entirely; a large value restores the pre-V634 behaviour. The one-line '[D3D11Rtx][frame-anomaly]' warn is unaffected and still marks every run.");

    // DX11_V634. The other high-volume emitter: 512 lines per arming, and it re-arms.
    RTX_OPTION("rtx.dx11", bool, kenshiLogDrawTransitions, true,
      "Write the '[D3D11Rtx][pt-transition] committed draw' telemetry.\n"
      "Armed for the first complete PT frame after a raster bypass and capped at 512 draws per arming, but it re-arms on every bypass - measured 90 armings and 28,049 lines in one 14-minute run. Off costs nothing unless you are working on the raster-to-PT handover itself.");

    // DX11_V635. Detection-only probe for the building-interior clip, chapter 16.
    RTX_OPTION("rtx.dx11", bool, kenshiLogInteriorProbe, true,
      "Write the '[D3D11Rtx][interior]' probe lines. Detection only - this option changes no rendering.\n"
      "Reports, on every interior enter/leave and then once every ~600 frames while inside: how many InteriorMask compositor draws Kenshi issued (2 per active volume, so the count says whether several of the 39 authored *MASK*.mesh shells are unioned), their index count (which identifies WHICH shell, against the chapter 16 inventory), how many admitted draws carry the interiorClip/interiorMask texture, and whether that texture is the 1x1 dummy main.compositor binds when no interior is active.\n"
      "Its whole purpose is to check that the two independent 'an interior is active' signals - volume draws present, mask texture not 1x1 - actually agree. Bounded by design: transitions are rare and the heartbeat is slow, unlike the V634 emitters above.");

    // DX11_V655. Bindless buffer-index lifetime audit. Detection only.
    RTX_OPTION("rtx.dx11", bool, kenshiLogBindlessAudit, true,
      "Write the [D3D11Rtx][bindless-audit] lines. Changes no rendering. "
      "SceneManager::m_bufferCache is cleared every frame in onFrameEnd, and BufferRefTable::track appends in submission order while deduplicating only against the entry immediately before it, so a bindless buffer index is valid for exactly the frame it was tracked in. "
      "updateBufferCache has one caller, inside processGeometryInfo, which runs only for a BLAS touched this frame, while InstanceManager::processInstanceBuffers copies blas.modifiedGeometryData buffer indices AND offsetFromSlice into the surface. "
      "An instance kept alive without its BLAS being touched therefore carries the previous frame index next to a current offset. "
      "This walks every submitted instance before the descriptor table is published and reports how many carry indices from an untouched BLAS, and how many of those fall outside this frame table. "
      "Out of range is the dangerous case: createDescriptorSet never writes those slots, so the shader reads a stale or dummy descriptor at a real suballocation offset, which is the sub-100MB read-invalid address in the device-fault reports.");

    RTX_OPTION("rtx.dx11", bool, kenshiRetrackKeptInstanceBuffers, false,
      "CANDIDATE FIX for the above. Off by default - turn on only to test it. "
      "Re-tracks the geometry buffers of every instance whose BLAS was not touched this frame, once per BLAS, and rewrites that instance six surface buffer indices to the freshly reserved ones. "
      "Runs before RtxBindlessResourceManager::prepareSceneData publishes the table, which is the V527 ordering rule: reserve during scene building, never at binding time. "
      "Changes no geometry, no transform, no admission and no culling policy - only which table slot a surface names. "
      "Growth is bounded: the retrack is per BLAS, not per instance, and the table limit is 65525 against a measured 450-800 entries per frame.");

    // DX11_V656. Validates the ENTRIES of the bindless buffer table, not the
    // indices into it. V655b settled that indices are in range.
    RTX_OPTION("rtx.dx11", bool, kenshiLogBufferTableAudit, true,
      "Write the [D3D11Rtx][buffer-table] lines. Detection only, changes no rendering. "
      "createDescriptorSet writes engineObject.getDescriptor().buffer into the bindless table whenever engineObject.defined() is true, and defined() only tests that the slice HAS a buffer - not that the buffer is still live. "
      "A released DxvkBuffer therefore yields a descriptor with a dead or null VkBuffer handle beside a surviving suballocation offset, which is the shape of every fake-OOM fault address this session: a read below every tracked allocation at 72-101 MB. "
      "This walks the table immediately before it is published and reports entries that are defined but carry a null VkBuffer handle, entries whose descriptor offset+range exceeds the owning buffer size, and the largest descriptor offset in the table. "
      "If the largest offset tracks the fault band, the faulting resource is in this table.");

    // DX11_V657. Gates the Integrate NEE dispatch on the NEE cache being enabled.
    RTX_OPTION("rtx.dx11", bool, kenshiGateNeeIntegration, true,
      "Skip DxvkPathtracerIntegrateIndirect::dispatchNEE when rtx.neeCache.enable is False. "
      "Aftermath named Integrate NEE as the pass in flight at the DMA page fault (Device state Error_DMA_PageFault, GPU VA 0x6640000, 102.25 MB - the same band as all 19 fake-OOM faults). "
      "That pass is dispatched unconditionally from rtx_context.cpp:2524 even though NeeCachePass::dispatch itself early-returns when the cache is off, so with rtx.neeCache.enable already False the cache is never updated while the integration that consumes it still runs every frame. "
      "It binds the per-frame surface primitive-ID prefix sum (m_reorderedSurfacesPrimitiveIDPrefixSum), and surfaces are reordered every frame, so a stale surface index maps through that table to a garbage global primitive ID - which is what a scene collapse would produce. "
      "Diagnostic first, candidate fix second. Indirect lighting may change or degrade because ReSTIR GI input samples are constructed in this pass; only the crash is under test.");

    // DX11_V759. Restores the distance falloff of Kenshi's own heat-haze post
    // process. See rtx/pass/kenshi_heat_haze_depth.h for the mechanism.
    RTX_OPTION("rtx.dx11", bool, kenshiHeatHazeDepth, true,
      "Give Kenshi's heat-haze post process the scene depth it reads for its distance falloff.\n"
      "The effect scales its distortion by global_gbuffer target 2, which the deferred pass fills with distance/farClip. Under path tracing those draws never reach the raster pipeline, so that target keeps its clear value of 0 - and the shader then reads 0 as sky and applies FULL distortion to every pixel, near geometry included.\n"
      "On, Remix writes its own primary hit distance into that target in the game's own encoding, immediately after the composite injection and before the effect samples it. Off restores the flat, distance-blind haze.\n"
      "Costs one compute dispatch and one blit at render resolution per frame. Affects nothing else: no other pass in Kenshi's shipped compositor chain reads that target.");

    RTX_OPTION("rtx.dx11", float, kenshiHeatHazeDistanceScale, 1.0f,
      "Scales the distance at which the heat haze reaches full strength.\n"
      "1.0 reproduces the game's own ramp exactly - saturation at farClip/6, matching raster. Larger values push full strength further away, so near and mid-range objects shimmer less; smaller values bring it closer.\n"
      "Requires Heat Haze Depth to be on. Useful range: 0.25 to 4.0.");

    // DX11_V760. The building-placement ghost's colour. See the capture site in
    // d3d11_rtx.cpp (search DX11_V760_KENSHI_CONSTANT_COLOUR).
    RTX_OPTION("rtx.dx11", bool, kenshiConstantColour, true,
      "Let an UNTEXTURED draw take its albedo from its pixel shader's colour constant instead of opaque white.\n"
      "Kenshi's building-placement preview - the ghost that turns green where a plot is legal and red where it is not - binds no texture at all and carries no vertex colours: `redbuilding`/`greenbuilding` (forward/previewbuilding.material) put the colour in a `colour` uniform, and the pixel shader is two instructions long. Untextured draws defaulted to white, so every ghost was white under path tracing whatever raster showed.\n"
      "Only fires where the shader declares NO texture resources at all, which is what proves the constant is the whole albedo rather than a tint. Alpha travels with it, so the yellow sign highlight keeps its transparency.\n"
      "Off restores the opaque-white default.");

    // DX11_V760. Buildings under construction. See the flag
    // OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_CONSTRUCTION in shared_constants.h.
    RTX_OPTION("rtx.dx11", bool, kenshiConstruction, true,
      "Reproduce Kenshi's under-construction scaffold: the building is cut away above the build line and only the lattice stands there, filling upward as the work progresses.\n"
      "The game does this with a discard whose threshold is an immediate 0.7 ANDed with a height test against a per-draw progress constant, so the generic cutout recovery cannot see it and an unfinished building otherwise renders complete and solid.\n"
      "On, the material carries the scaffold texture, the progress and the mesh height, and the RT material reproduces the game's own expression per hit - including the scaffold's 0.4 gloss and the flattened normal underneath it. Costs one extra texture read on construction draws only.\n"
      "Off renders every building as finished, which is the behaviour before V760.");

    // DX11_V765_KENSHI_LIGHT_GC. See LightManager::addGameLight and
    // LightManager::kenshiLightSweep.
    RTX_OPTION("rtx.dx11", bool, kenshiLightNoSleep, true,
      "Stop a motionless local light from becoming permanent.\n"
      "Remix identifies a game light by POSITION alone - radiance, colour, power and range are all excluded from its hash on purpose - so a light whose world position holds still for numFramesToKeepLights/2 frames (50 by default) is judged 'static'. Remix then stops updating it, freezing its brightness, AND exempts it from garbage collection entirely, so it outlives the game removing it with no time limit. A lamp unequipped while its carrier stands still, or a lightning flash that lasts long enough, never goes out.\n"
      "On, Kenshi's lights are held one step below that threshold, so they keep tracking the game's brightness and stay collectable. Off restores stock behaviour.");

    RTX_OPTION("rtx.dx11", bool, kenshiLightDropWhenVisible, true,
      "Remove a local light as soon as the game stops drawing it, when the game would still be drawing it.\n"
      "Kenshi submits a light volume whenever that volume intersects the view, so a light that is missing while its own volume still reaches the view was switched off by the game rather than culled by it. Those are dropped at once instead of lingering for numFramesToKeepLights (100) frames, which is the second or two every local light currently takes to go out. A light whose volume does NOT reach the view is genuinely ambiguous and is left on the stock timer - that is what keeps off-screen lights available for indirect lighting.\n"
      "The test is the light's REACH against the view, not whether its position is on screen, so a distant lightning flash bright enough to light the whole scene is covered too.\n"
      "Off restores the stock 100-frame delay for every light.");

    RTX_OPTION("rtx.dx11", uint32_t, kenshiLightDropGraceFrames, 3u,
      "How many consecutive frames a light must be missing before Light Drop When Visible removes it.\n"
      "Guards against a one-frame gap in submission - the per-frame harvest cap, or a volume briefly clipped - reading as the game switching the light off. Useful range: 2 to 10. Requires Light Drop When Visible.");

    RTX_OPTION("rtx.dx11", float, kenshiLightCullMargin, 1.25f,
      "Safety margin on the radius Light Drop When Visible tests, as a multiple of the light's own volume radius.\n"
      "Our frustum is reconstructed and is not bit-identical to the game's, so the test is inflated slightly to guarantee a light the game is still drawing is never judged out of reach and dropped. Raise it if the census reports non-zero violations. Below 1.0 is not useful. Requires Light Drop When Visible.");

    RTX_OPTION("rtx.dx11", bool, kenshiLightGcStats, true,
      "Log a periodic census of local-light lifetime.\n"
      "Reports the light table size, how many lights are held but no longer submitted, the oldest such age, how many were dropped - and `violations`, the number of lights the game WAS drawing that our reach test called out of view. Violations must be zero: a non-zero count means the test is too tight and Light Drop When Visible could remove a light the game still wants, which Light Cull Margin corrects.\n"
      "One summary line per interval, not one line per light. Cheap enough to leave on.");

    RTX_OPTION("rtx.dx11", uint32_t, kenshiLightGcStatsInterval, 600u,
      "How many frames between Light GC census lines. Requires Light GC Stats.");
  };

}
