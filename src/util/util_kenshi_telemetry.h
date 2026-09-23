#pragma once

#include "util_kenshi_diagnostic_output.h"

#include <atomic>
#include <initializer_list>
#ifdef _WIN32
#include <windows.h>
#endif

namespace dxvk::kenshi_telemetry {
  // Shared by the frontend, command stream and workers in each runtime DLL.
  // The option callback publishes changes; hot paths never query the option system.
  inline std::atomic<bool> requested { false };
  inline bool enabled() { return requested.load(std::memory_order_relaxed); }
  inline void setEnabled(bool value) { requested.store(value, std::memory_order_relaxed); }
  inline void publish(bool value) {
    setEnabled(value);
#ifdef _WIN32
    // Each DLL links its own renderer statics. Publish UI changes to both copies.
    using Setter = void (WINAPI*)(BOOL);
    for (const wchar_t* name : { L"d3d11.dll", L"dxgi.dll" }) {
      if (HMODULE module = GetModuleHandleW(name)) {
        auto setter = reinterpret_cast<Setter>(GetProcAddress(module, "RemixSetKenshiTelemetry"));
        if (setter) setter(value ? TRUE : FALSE);
      }
    }
#endif
  }
  template<typename T>
  inline void add(std::atomic<T>& counter, T amount = T(1)) {
    if (enabled()) counter.fetch_add(amount, std::memory_order_relaxed);
  }
}

// Lazy arguments: formatting, hashing and resource queries inside a message
// are skipped too. Use only for diagnostics, never errors or functional work.
#define KENSHI_DIAGNOSTIC_INFO(...) do { \
  if (::dxvk::kenshi_telemetry::enabled()) ::dxvk::Logger::info(__VA_ARGS__); \
} while (false)
