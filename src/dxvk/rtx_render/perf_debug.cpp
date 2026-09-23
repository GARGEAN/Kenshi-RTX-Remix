#include "perf_debug.h"
#include "../../util/util_kenshi_diagnostic_output.h"
#include <fstream>
#include <chrono>
#include <iomanip>

bool g_PerfDebugMode = false;

static std::ofstream logFile;
static std::chrono::high_resolution_clock::time_point frameStart;
static unsigned int drawCallCount = 0;
static std::array<bool, FEATURE_COUNT> featureDisabled = { false };

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;

// GPU timing queries
struct GPUTimer {
    ID3D11Query* startQuery = nullptr;
    ID3D11Query* endQuery = nullptr;
    double lastTimeMs = 0.0;
};
static std::array<GPUTimer, FEATURE_COUNT> gpuTimers;
static ID3D11Query* disjointQuery = nullptr;

void PerfDebug_Init(ID3D11Device* device, ID3D11DeviceContext* context) {
    g_device = device;
    g_context = context;

    if (!g_device || !g_context) return;

    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_TIMESTAMP;
    for (int i = 0; i < FEATURE_COUNT; i++) {
        g_device->CreateQuery(&qd, &gpuTimers[i].startQuery);
        g_device->CreateQuery(&qd, &gpuTimers[i].endQuery);
    }

    // Create the disjoint query ONCE for the lifetime of the app
    D3D11_QUERY_DESC disjointDesc = {};
    disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    g_device->CreateQuery(&disjointDesc, &disjointQuery);
}

void PerfDebug_Shutdown() {
    for (int i = 0; i < FEATURE_COUNT; i++) {
        if (gpuTimers[i].startQuery) { gpuTimers[i].startQuery->Release(); gpuTimers[i].startQuery = nullptr; }
        if (gpuTimers[i].endQuery) { gpuTimers[i].endQuery->Release(); gpuTimers[i].endQuery = nullptr; }
    }
    if (disjointQuery) { disjointQuery->Release(); disjointQuery = nullptr; }
    if (logFile.is_open()) {
        logFile.close();
    }
}

void PerfDebug_BeginFrame() {
    if (!g_PerfDebugMode || !g_context) return;
    
    if (dxvk::kenshi_telemetry::fileOutputEnabled() && !logFile.is_open()) {
        logFile.open("perf_debug.log", std::ios::out | std::ios::trunc);
    }
    frameStart = std::chrono::high_resolution_clock::now();
    drawCallCount = 0;

    // Begin the disjoint query for the whole frame
    if (disjointQuery) {
        g_context->Begin(disjointQuery);
    }
}

void PerfDebug_EndFrame() {
    if (!g_PerfDebugMode || !g_context) return;

    // End the disjoint query for the whole frame
    if (disjointQuery) {
        g_context->End(disjointQuery);
    }

    // Fetch disjoint data ONCE per frame
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData = {};
    if (disjointQuery) {
        while (g_context->GetData(disjointQuery, &disjointData, sizeof(disjointData), 0) != S_OK) {}
    }

    for (int i = 0; i < FEATURE_COUNT; i++) {
        if (gpuTimers[i].startQuery && gpuTimers[i].endQuery) {
            UINT64 startTime = 0, endTime = 0;
            while (g_context->GetData(gpuTimers[i].startQuery, &startTime, sizeof(startTime), 0) != S_OK) {}
            while (g_context->GetData(gpuTimers[i].endQuery, &endTime, sizeof(endTime), 0) != S_OK) {}

            if (!disjointData.Disjoint && disjointData.Frequency > 0 && endTime >= startTime) {
                gpuTimers[i].lastTimeMs = static_cast<double>(endTime - startTime) / static_cast<double>(disjointData.Frequency) * 1000.0;
            } else {
                gpuTimers[i].lastTimeMs = 0.0; // Invalid data
            }
        }
    }

    auto frameEnd = std::chrono::high_resolution_clock::now();
    double cpuMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();

    if (logFile.is_open()) {
        logFile << "Frame CPU time: " << std::fixed << std::setprecision(2) << cpuMs
                << " ms | Draw calls: " << drawCallCount << "\n";

        for (int i = 0; i < FEATURE_COUNT; i++) {
            logFile << "  Feature[" << i << "] GPU time: "
                    << std::fixed << std::setprecision(2) << gpuTimers[i].lastTimeMs << " ms\n";
        }
        logFile.flush();
    }
}

void PerfDebug_Log(const std::string& msg) {
    if (!dxvk::kenshi_telemetry::fileOutputEnabled()) return;
    // Removed the g_PerfDebugMode check here so we can log when it turns OFF
    if (!logFile.is_open()) {
        logFile.open("perf_debug.log", std::ios::out | std::ios::trunc);
    }
    if (logFile.is_open()) {
        logFile << msg << "\n";
        logFile.flush();
    }
}

void PerfDebug_SetFeatureDisabled(PerfFeature feature, bool disabled) {
    if (feature < FEATURE_COUNT) {
        featureDisabled[feature] = disabled;
        PerfDebug_Log(std::string("Feature ") + std::to_string(feature) +
                      (disabled ? " DISABLED" : " ENABLED"));
    }
}

bool PerfDebug_IsFeatureDisabled(PerfFeature feature) {
    return (feature < FEATURE_COUNT) ? featureDisabled[feature] : false;
}

void PerfDebug_IncrementDrawCall() {
    if (g_PerfDebugMode) {
        drawCallCount++;
    }
}

void PerfDebug_BeginFeature(PerfFeature feature) {
    if (!g_PerfDebugMode || feature >= FEATURE_COUNT || !g_context) return;
    if (gpuTimers[feature].startQuery) {
        g_context->End(gpuTimers[feature].startQuery);
    }
}

void PerfDebug_EndFeature(PerfFeature feature) {
    if (!g_PerfDebugMode || feature >= FEATURE_COUNT || !g_context) return;
    if (gpuTimers[feature].endQuery) {
        g_context->End(gpuTimers[feature].endQuery);
    }
}
