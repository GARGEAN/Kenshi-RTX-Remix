// Isolated DLL fixture for the process-wide telemetry toggle test.
#define NOMINMAX
#include "../../src/util/util_kenshi_telemetry.h"

extern "C" __declspec(dllexport) void WINAPI RemixSetKenshiTelemetry(BOOL enabled) {
  dxvk::kenshi_telemetry::setEnabled(enabled != FALSE);
}
extern "C" __declspec(dllexport) BOOL WINAPI TestTelemetryEnabled() {
  return dxvk::kenshi_telemetry::enabled() ? TRUE : FALSE;
}
