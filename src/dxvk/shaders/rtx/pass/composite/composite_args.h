/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
#ifndef RTX_PASS_COMPOSITE_COMPOSITE_ARGS_H_DX11V225
#define RTX_PASS_COMPOSITE_COMPOSITE_ARGS_H_DX11V225

// DX11_V505: maximum simultaneously visible Kenshi fog volumes. Must match
// dxvk::kenshi_fog::kMaxFogVolumes in rtx_kenshi_fog_volumes.h.
//
// DX11_V754: raised 16 -> 32. 16 was sized from "28 volumes in the whole world,
// so 16 on screen is generous" and that reasoning was wrong - a Ctrl+Alt+O trace
// in the Swamp measured 18 publishable volumes in a single frame. The full
// reasoning and the measurement are on kenshi_fog::kMaxFogVolumes. Cost is 160
// bytes per volume: CompositeArgs measures 8288 bytes at 16 and 10848 at 32,
// in a uniform buffer whose limit on the target hardware is 64 KiB.
#define KENSHI_FOG_MAX_VOLUMES 32 // DX11_V225_GUARD

#include "rtx/utility/shader_types.h"
#include "rtx/pass/volume_args.h"
#include "rtx/pass/raytrace_args.h"
#include "rtx/algorithm/accumulate.h"

#define DENOISER_MODE_OFF 0
#define DENOISER_MODE_RELAX 1
#define DENOISER_MODE_REBLUR 2

struct CompositeArgs {
  Camera camera;
  DomeLightArgs domeLightArgs;
  RayPortalHitInfo rayPortalHitInfos[maxRayPortalCount * 2];
  VolumeArgs volumeArgs;
  AccumulationArgs accumulationArgs;

  // -- Struct objects should go above this line to preserve alignment --

  // Fog
  vec3 fogColor;
  uint fogMode;

  float fogScale;
  float fogEnd;
  float fogDensity;
  float maxFogDistance;

  vec4 debugKnob;

  uint usePostFilter;
  uint demodulateRoughness;
  float roughnessDemodulationOffset;
  uint combineLightingChannels;

  // One of DENOISER_MODE constants, affects signal conversion
  uint primaryDirectDenoiser;
  uint primaryIndirectDenoiser;
  uint secondaryCombinedDenoiser;  
  uint enableRtxdi;

  float primaryDirectMissLinearViewZ;
  uint enableReSTIRGI;
  float pixelHighlightReuseStrength;
  uint debugViewIdx;

  uint8_t compositePrimaryDirectDiffuse;
  uint8_t compositePrimaryDirectSpecular;
  uint8_t compositePrimaryIndirectDiffuse;
  uint8_t compositePrimaryIndirectSpecular;
  uint8_t compositeSecondaryCombinedDiffuse;
  uint8_t compositeSecondaryCombinedSpecular;
  // The number of active Ray Portals (Used for Ray Portal sampling). Always <= RAY_PORTAL_MAX_COUNT
  uint8_t numActiveRayPortals;
  uint8_t pad0;

  uint enableSeparatedDenoisers;
  uint frameIdx;

  uint outputSecondarySignalToParticleLayer;
  uint compositeVolumetricLight;
  uint outputParticleLayer;
  uint enableDemodulateAttenuation;

  uint enableStochasticAlphaBlend;
  uint stochasticAlphaBlendEnableFilter;
  uint stochasticAlphaBlendUseNeighborSearch;
  uint stochasticAlphaBlendSearchTheSameObject;

  uint stochasticAlphaBlendSearchIteration;
  float stochasticAlphaBlendInitialSearchRadius;
  float stochasticAlphaBlendRadiusExpandFactor;
  uint stochasticAlphaBlendShareNeighbors;

  float stochasticAlphaBlendNormalSimilarity;
  float stochasticAlphaBlendDepthDifference;
  float stochasticAlphaBlendPlanarDifference;
  uint stochasticAlphaBlendUseRadianceVolume;

  float stochasticAlphaBlendRadianceVolumeMultiplier;
  uint stochasticAlphaBlendDiscardBlackPixel;
  uint enhanceAlbedo;
  float skyBrightness;

  vec3 clearColorFinalColor;
  uint timeSinceStartMS;

  float alphaBlendSurfacePackMult; // for packing/unpacking hitT into Float16 in AlphaBlendSurface
  float postFilterThreshold;
  float kenshiFogDensity;
  float kenshiFogOpacity;

  // DX11_V496_KENSHI_DISTANT_FOG. Appended at the END of the struct on purpose -
  // inserting into the middle of a shared args struct was a measured regression
  // (see the V456/V457 entries for RaytraceArgs).
  vec3 kenshiFogColour;
  uint kenshiFogActive;

  float kenshiFogAtmoStart;
  float kenshiFogAtmoEnd;
  float kenshiFogSkyBlend;
  float kenshiFogFarClip;

  vec3 kenshiFogHorizonColour;
  float kenshiFogHorizonBlend;

  // DX11_V503: SkyX scattering, appended at the END of the struct.
  vec3 kenshiSkyXCameraPos;
  float kenshiSkyXInnerRadius;

  vec3 kenshiSkyXInvWaveLength;
  float kenshiSkyXSkydomeRadius;

  vec3 kenshiSkyXScatter;          // (uKrESun, uKr4PI, uKm4PI)
  float kenshiSkyXExposure;

  vec3 kenshiSkyXScaleParams;      // (uScale, uScaleDepth, uScaleOverScaleDepth)
  float kenshiFogScatterScale;

  vec3 kenshiFogSunDir;
  uint kenshiFogScatterValid;

  // DX11_V505: Kenshi's local fog volumes. The cap costs constant-buffer bytes
  // only - the shader loops to kenshiFogVolumeCount, never to the cap.
  // planes: block = 7 world-space planes (dot(n,p) <= w);
  //         sphere = [0] is (centre.xyz, radius);
  //         cylinder = [0] (base.xyz, radius), [1] (axis.xyz, height).
  // params0: (density, edgeBlur, type, sunLight)   type 1=block 2=sphere 3=cylinder
  // params1: (worldOffset.xyz, unused)  - blocks add this to the CAMERA
  vec4 kenshiFogVolumePlanes[KENSHI_FOG_MAX_VOLUMES * 7];
  vec4 kenshiFogVolumeColour[KENSHI_FOG_MAX_VOLUMES];
  vec4 kenshiFogVolumeParams0[KENSHI_FOG_MAX_VOLUMES];
  vec4 kenshiFogVolumeParams1[KENSHI_FOG_MAX_VOLUMES];

  uint kenshiFogVolumeCount;
  uint kenshiFogVolumePad0;
  uint kenshiFogVolumePad1;
  uint kenshiFogVolumePad2;
};

#endif // RTX_PASS_COMPOSITE_COMPOSITE_ARGS_H_DX11V225
