#include "util_kenshi_telemetry.h"
#pragma once
#include "util_kenshi_terrain_profile.h"

namespace dxvk::city_cpu {
  // Process-local comparison controls. No persistent configuration or renderer layout changes. Baseline
  // leases expire if the capture runner exits.
  inline std::atomic<bool> optimized { true };
  inline thread_local uint32_t keyRemaining = 0, keyVerified = 0, keyMismatches = 0;
  inline thread_local bool keyValid = true;
  inline bool enabled() { return optimized.load(std::memory_order_relaxed); }
#ifndef KENSHI_PROFILE_TEST
  inline void poll() {
    if (!kenshi_telemetry::enabled()) { optimized = true; return; }
    struct Control {
      HANDLE baseline = nullptr, normal = nullptr;
      uint32_t polls = 0;
      ULONGLONG deadline = 0;
      Control() {
        const auto suffix = std::to_wstring(GetCurrentProcessId());
        baseline = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\KenshiCityCPUBaseline-" + suffix).c_str());
        normal = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\KenshiCityCPUOptimized-" + suffix).c_str());
        KENSHI_DIAGNOSTIC_INFO(std::string("[CityCPU V716] controls=") + (baseline && normal ? "ready" : "unavailable")
          + " optimized=1 baselineLeaseSeconds=90 normalProfiling=off");
      }
      ~Control() { if (baseline) CloseHandle(baseline); if (normal) CloseHandle(normal); }
    };
    static Control control;
    if ((++control.polls & 63u) != 1u) return;
    const bool baseline = control.baseline && WaitForSingleObject(control.baseline, 0) == WAIT_OBJECT_0;
    const bool normal = control.normal && WaitForSingleObject(control.normal, 0) == WAIT_OBJECT_0;
    if (baseline && !normal) {
      control.deadline = GetTickCount64() + 90000;
      optimized.store(false, std::memory_order_relaxed);
      KENSHI_DIAGNOSTIC_INFO("[CityCPU V716] mode=baseline optimized=0 baselineLeaseSeconds=90");
    } else if (normal || (!enabled() && GetTickCount64() >= control.deadline)) {
      optimized.store(true, std::memory_order_relaxed);
      KENSHI_DIAGNOSTIC_INFO("[CityCPU V716] mode=optimized optimized=1");
    }
  }
#endif
}
