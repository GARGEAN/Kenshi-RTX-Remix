#include "util_kenshi_telemetry.h"
#pragma once
#include "util_kenshi_terrain_profile.h"

namespace dxvk::worker_cpu {
  // Ordered main-lane control; a normal launch parks idle geometry workers.
  inline bool waiting = true;
#ifndef KENSHI_PROFILE_TEST
  inline void poll() {
    if (!kenshi_telemetry::enabled()) { waiting = true; return; }
    struct Control {
      HANDLE baseline = nullptr, normal = nullptr;
      uint32_t polls = 0;
      ULONGLONG deadline = 0;
      Control() {
        const auto suffix = std::to_wstring(GetCurrentProcessId());
        baseline = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\KenshiWorkerBaseline-" + suffix).c_str());
        normal = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\KenshiWorkerWaiting-" + suffix).c_str());
        KENSHI_DIAGNOSTIC_INFO(std::string("[WorkerCPU V719] controls=") + (baseline && normal ? "ready" : "unavailable")
          + " waiting=1 emptyScans=32 baselineLeaseSeconds=120 normalProfiling=off");
      }
      ~Control() { if (baseline) CloseHandle(baseline); if (normal) CloseHandle(normal); }
    };
    static Control control;
    if ((++control.polls & 63u) != 1u) return;
    const bool baseline = control.baseline && WaitForSingleObject(control.baseline, 0) == WAIT_OBJECT_0;
    const bool normal = control.normal && WaitForSingleObject(control.normal, 0) == WAIT_OBJECT_0;
    if (baseline && !normal) {
      control.deadline = GetTickCount64() + 120000;
      waiting = false;
      KENSHI_DIAGNOSTIC_INFO("[WorkerCPU V719] mode=baseline waiting=0 baselineLeaseSeconds=120");
    } else if (normal || (!waiting && GetTickCount64() >= control.deadline)) {
      waiting = true;
      KENSHI_DIAGNOSTIC_INFO("[WorkerCPU V719] mode=waiting waiting=1");
    }
  }
#endif
}
