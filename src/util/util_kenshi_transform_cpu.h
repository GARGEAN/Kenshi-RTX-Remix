#include "util_kenshi_telemetry.h"
#pragma once
#include "util_kenshi_terrain_profile.h"

namespace dxvk::transform_cpu {
  // V718: main-lane finite validation and process-local, expiring comparison.
  inline std::atomic<bool> optimized { true };
  inline thread_local bool valid = true;
  inline thread_local uint32_t layoutRemaining = 0, layoutVerified = 0, layoutMismatches = 0;
  inline thread_local uint32_t signRemaining = 0, signVerified = 0, signMismatches = 0, signViable = 0;
  inline bool enabled() { return optimized.load(std::memory_order_relaxed) && valid; }
  inline void arm() {
    layoutRemaining = signRemaining = 64;
    layoutVerified = layoutMismatches = signVerified = signMismatches = signViable = 0;
  }
  inline void stop() { layoutRemaining = signRemaining = 0; }
#ifndef KENSHI_PROFILE_TEST
  inline void poll() {
    if (!kenshi_telemetry::enabled()) { optimized = true; stop(); return; }
    struct Control {
      HANDLE baseline = nullptr, normal = nullptr;
      uint32_t polls = 0;
      ULONGLONG deadline = 0;
      Control() {
        const auto suffix = std::to_wstring(GetCurrentProcessId());
        baseline = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\KenshiTransformBaseline-" + suffix).c_str());
        normal = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\KenshiTransformOptimized-" + suffix).c_str());
        KENSHI_DIAGNOSTIC_INFO(std::string("[TransformCPU V718] controls=") + (baseline && normal ? "ready" : "unavailable")
          + " optimized=1 baselineLeaseSeconds=120 normalProfiling=off verification=off");
      }
      ~Control() { if (baseline) CloseHandle(baseline); if (normal) CloseHandle(normal); }
    };
    static Control control;
    if ((++control.polls & 63u) != 1u) return;
    const bool baseline = control.baseline && WaitForSingleObject(control.baseline, 0) == WAIT_OBJECT_0;
    const bool normal = control.normal && WaitForSingleObject(control.normal, 0) == WAIT_OBJECT_0;
    if (baseline && !normal) {
      control.deadline = GetTickCount64() + 120000;
      optimized = false;
      KENSHI_DIAGNOSTIC_INFO("[TransformCPU V718] mode=baseline optimized=0 baselineLeaseSeconds=120");
    } else if (normal || (!optimized && GetTickCount64() >= control.deadline)) {
      optimized = true;
      KENSHI_DIAGNOSTIC_INFO(std::string("[TransformCPU V718] mode=optimized optimized=") + (enabled() ? "1" : "0"));
    }
  }
#endif
}
