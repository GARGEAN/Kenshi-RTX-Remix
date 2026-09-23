#pragma once
#include <string>
#include <array>
#include <d3d11.h>

extern bool g_PerfDebugMode;

enum PerfFeature {
    FEATURE_SHADOWS = 0,
    FEATURE_REFLECTIONS,
    FEATURE_POSTFX,
    FEATURE_AO,
    FEATURE_VOLUMETRICS,
    FEATURE_RAYTRACING,
    FEATURE_PARTICLES,
    FEATURE_DECALS,
    FEATURE_LIGHTING,
    FEATURE_FOG,
    FEATURE_COUNT
};

void PerfDebug_Init(ID3D11Device* device, ID3D11DeviceContext* context);
void PerfDebug_Shutdown(); // Added for proper cleanup
void PerfDebug_BeginFrame();
void PerfDebug_EndFrame();
void PerfDebug_Log(const std::string& msg);
void PerfDebug_SetFeatureDisabled(PerfFeature feature, bool disabled);
bool PerfDebug_IsFeatureDisabled(PerfFeature feature);
void PerfDebug_IncrementDrawCall();

// GPU timing
void PerfDebug_BeginFeature(PerfFeature feature);
void PerfDebug_EndFeature(PerfFeature feature);