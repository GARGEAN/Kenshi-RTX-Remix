#pragma once

#include <cstdint>
#include <string>

namespace dxvk {
  struct DrawCallState;
  class RtInstance;

  // CS-thread-only census. No changes to renderer object layouts.
  namespace KenshiFlickerTrace {
    void add(const char* stage, std::string row);
    void beginDraw(uint32_t sourceFrame, uint32_t entry, const DrawCallState& draw);
    void endDraw();
    void resolved(const DrawCallState& draw, uint64_t replacementId,
                  uint64_t previousId, const RtInstance* instance);
    std::string describeInstance(const RtInstance* instance);
    void finish(uint32_t frame);
  }
}
