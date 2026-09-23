// Standalone Windows test: uses the production gates without starting Kenshi.
#define NOMINMAX
#include <cassert>
#include <iostream>
#include "../../src/util/util_kenshi_fault.h"
#include "../../src/util/util_kenshi_terrain_profile.h"
#include "../../src/util/util_kenshi_terrain_bounds.h"
#include "../../src/util/util_kenshi_prepared_terrain.h"
#include "../../src/util/util_kenshi_material_probe.h"

static unsigned messages = 0;
void dxvk::Logger::info(const std::string&) { ++messages; }
void dxvk::Logger::warn(const std::string&) { ++messages; }
void dxvk::Logger::err(const std::string&) { ++messages; }
// Even a legacy launch-wide request must lose to the disabled master.
std::string dxvk::env::getEnvVar(const char*) { return "1"; }

int main(int argc, char** argv) {
  using namespace dxvk;
  assert(!kenshi_telemetry::enabled());
  // Optional paths are isolated fixture DLLs, never the actual game runtimes.
  if (argc == 3) {
    HMODULE modules[] = { LoadLibraryA(argv[1]), LoadLibraryA(argv[2]) };
    using Getter = BOOL (WINAPI*)();
    for (HMODULE module : modules) assert(module);
    for (bool value : { true, false, true, false }) {
      kenshi_telemetry::publish(value);
      assert(kenshi_telemetry::enabled() == value);
      for (HMODULE module : modules) {
        auto getter = reinterpret_cast<Getter>(GetProcAddress(module, "TestTelemetryEnabled"));
        assert(getter && bool(getter()) == value);
      }
    }
    for (HMODULE module : modules) FreeLibrary(module);
  }
  unsigned formatted = 0;
  KENSHI_DIAGNOSTIC_INFO(std::to_string(++formatted));
  assert(formatted == 0 && messages == 0);
  assert(!terrain_profile::enabled() && !terrain_profile::diagnosticsEnabled());
  uint64_t capture = 0;
  for (unsigned i = 0; i != 1000; ++i) {
    assert(terrain_profile::pollTimedCapture(true, capture) == terrain_profile::TimedAction::None);
    kenshi_fault::frame(i, 1, true, 0);
    kenshi_fault::add(kenshi_fault::Submit, i);
  }
  assert(!kenshi_fault::state().initialized && kenshi_fault::state().serial == 0);
  assert(kenshi_fault::state().ledger == INVALID_HANDLE_VALUE);

  terrain_bounds::requestedVerification = 4;
  prepared_terrain::verifyRemaining = 4;
  assert(!terrain_bounds::takeVerification() && !prepared_terrain::claimVerification());
  assert(terrain_bounds::requestedVerification == 4 && prepared_terrain::verifyRemaining == 4);
  kenshi_material_probe::begin(1, 8);
  assert(!kenshi_material_probe::active(8));

  // Disabling telemetry must not disable the production geometry cache.
  shadow_geometry::Cache cache;
  shadow_geometry::Key key;
  unsigned computations = 0;
  auto calculate = [&] { ++computations; return shadow_geometry::Value {42, true}; };
  shadow_geometry::arm();
  assert(shadow_geometry::evaluate(&cache, 1, shadow_geometry::IndexHash, key, 4, calculate).result == 42);
  assert(shadow_geometry::evaluate(&cache, 1, shadow_geometry::IndexHash, key, 4, calculate).result == 42);
  assert(computations == 1 && shadow_geometry::state.verified[shadow_geometry::IndexHash] == 0);

  kenshi_telemetry::setEnabled(true);
  KENSHI_DIAGNOSTIC_INFO(std::to_string(++formatted));
  assert(formatted == 1 && messages == 1);
  assert(terrain_profile::enabled() && terrain_profile::diagnosticsEnabled());
  assert(terrain_bounds::takeVerification() && prepared_terrain::claimVerification());
  assert(kenshi_material_probe::active(8));
  shadow_geometry::evaluate(&cache, 1, shadow_geometry::IndexHash, key, 4, calculate);
  assert(computations == 2 && shadow_geometry::state.verified[shadow_geometry::IndexHash] == 1);
  kenshi_fault::add(kenshi_fault::Submit, 8);
  assert(kenshi_fault::state().serial == 1);
  terrain_profile::pollTimedCapture(true, capture);

  kenshi_telemetry::setEnabled(false);
  assert(terrain_profile::pollTimedCapture(true, capture) == terrain_profile::TimedAction::Cancelled);
  assert(terrain_profile::pollTimedCapture(true, capture) == terrain_profile::TimedAction::None);
  kenshi_fault::add(kenshi_fault::Submit, 9);
  assert(kenshi_fault::state().serial == 1);
  assert(!kenshi_material_probe::active(8));
  // Resource revision invalidation remains functional while diagnostics are off.
  shadow_geometry::evaluate(&cache, 2, shadow_geometry::IndexHash, key, 4, calculate);
  assert(computations == 3);
  std::cout << "PASS: default off, lazy messages, recorder/profiler gates, live disable, cache and revision preservation\n";
}
