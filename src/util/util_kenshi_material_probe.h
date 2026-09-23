#include "util_kenshi_telemetry.h"
#pragma once
#include <cstdint>

namespace dxvk::kenshi_material_probe {
  // Command-stream thread only. Begin is enqueued before the captured draws.
  // The frame guard prevents an unrendered request leaking into a later scene.
  inline uint32_t ticket = 0;
  inline uint32_t frame = ~0u;
  inline uint32_t sceneRows = 0;
  inline uint32_t sceneSuppressed = 0;
  inline bool active(uint32_t currentFrame) {
    return kenshi_telemetry::enabled() && ticket != 0 && frame == currentFrame;
  }
  inline void begin(uint32_t id, uint32_t currentFrame) {
    ticket = id;
    frame = currentFrame;
    sceneRows = sceneSuppressed = 0;
  }
}
