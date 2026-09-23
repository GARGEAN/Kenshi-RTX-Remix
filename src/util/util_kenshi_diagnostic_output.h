#pragma once

// Shipping policy: diagnostic code stays available, but cannot create/write files.
// Future diagnostic builds: change 0 to 1 (or define this macro to 1 globally),
// rebuild BOTH DLLs including their PCH, then enable Project Telemetry as needed.
// Independent of runtime options, environment variables and initialization order.
#ifndef DXVK_KENSHI_DIAGNOSTIC_FILE_OUTPUT
#define DXVK_KENSHI_DIAGNOSTIC_FILE_OUTPUT 0
#endif

namespace dxvk::kenshi_telemetry {
  inline constexpr bool fileOutputEnabled() {
    return DXVK_KENSHI_DIAGNOSTIC_FILE_OUTPUT != 0;
  }
}
