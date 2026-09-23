#pragma once

// DX11-only hook surface for the ported sky/atmosphere system.
//
// The upstream fork shipped a much larger hook catalogue that also brokered
// capture, submit, overlay, light and API-entry work. Those hooks are built on
// the D3D11 device and the Remix C API export surface (d3d11_device.h,
// IDirect3DTexture9, remixapi_*), none of which exists in this DX11-only
// runtime, so they are deliberately not ported. What remains here is exactly the
// set of entry points implemented by the atmosphere and weather modules, which
// are API-agnostic.
//
// These are free functions rather than RtxContext members because they need
// access to RtxContext private state (the atmosphere module, the weather
// blender) without widening the class's public interface; RtxContext declares
// them as friends. See the friend block in rtx_context.h.

#include "rtx_resources.h"

struct RaytraceArgs;

namespace dxvk {

  class RtxContext;

  namespace fork_hooks {

    // Creates the RtxAtmosphere instance on the context. Idempotent.
    void initAtmosphere(RtxContext& ctx);

    // Folds the atmosphere/cloud parameters into the frame's RaytraceArgs and
    // runs any LUT/cloud bakes that are due. Called before the constant buffer
    // is uploaded, so it may still mutate args.
    void updateAtmosphereConstants(RtxContext& ctx, RaytraceArgs& args);

    // Injects / updates / drops the sun (and moon, in Numos) distant lights.
    //
    // DX11_V463: called from RtxContext::updateRaytraceArgsConstantBuffer, the
    // LIVE atmosphere path. updateAtmosphereConstants above is vestigial - it
    // has no call site anywhere in the tree, so anything that depends on it
    // never runs, which is exactly how V462's sun came out dark.
    void syncAtmosphereDistantLights(RtxContext& ctx, const AtmosphereArgs& args);

    // Binds the atmosphere and cloud resources to their common-binding slots.
    // Slots with no live resource are left to DXVK's dummy-descriptor fallback.
    void bindAtmosphereLuts(RtxContext& ctx);

    // Cloud resource accessors used by passes outside the atmosphere module
    // (volumetric sky-ambient, debug views). Return an invalid Resource when the
    // corresponding feature is disabled.
    Resources::Resource getCloudSkyTransmittanceLut(RtxContext& ctx);
    Resources::Resource getCloudDSun(RtxContext& ctx);
    Resources::Resource getCloudDAmbient(RtxContext& ctx);
    Resources::Resource getCloudRenderRT(RtxContext& ctx);

    // True when the rasterized sky should be skipped because the physical
    // atmosphere is supplying sky radiance instead.
    bool injectRtxAtmosphereSkySkip();

    // Draws the atmosphere/cloud section of the Remix developer UI.
    void showAtmosphereUI(RtxContext& ctx);

    // Draws the weather section of the Remix developer UI.
    void showWeatherUI();

    // Advances the weather preset blender. Implemented in rtx_fork_weather.cpp.
    void updateWeatherBlender(RtxContext& ctx, float deltaTimeSeconds);

  } // namespace fork_hooks

} // namespace dxvk
