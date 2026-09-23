#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dxvk::kenshi_retention {
  constexpr uint32_t normalGraceFrames = 120;
  constexpr uint32_t pressureGraceFrames = 30;
  constexpr uint32_t normalReleaseLimit = 32;
  constexpr uint32_t pressureReleaseLimit = 64;

  inline float radius(bool enabled, float value, bool ground) {
    if (!enabled) return 0.f;
    // Match the native hook's controls: ground has no fixed upper limit.
    if (!std::isfinite(value)) {
      if (ground) return 0.f;
      value = 1000.f;
    }
    return ground ? std::max(100.f, value) : std::clamp(value, 100.f, 10000.f);
  }

  struct Pressure {
    bool active = false;
    void update(uint64_t used, uint64_t budget) {
      if (!budget) { active = false; return; }
      const uint64_t low = std::min<uint64_t>(uint64_t(1024) << 20, budget / 8);
      const uint64_t free = used < budget ? budget - used : 0;
      if (free < low) active = true;
      else if (free >= low + low / 2) active = false;
    }
  };

  // Nearest point of a conservative world AABB, not transform translation.
  // Works for vertices carrying placement, rotated/scaled boxes and origin rebases.
  // Negative means invalid: callers must preserve the owner in that case.
  template<typename Vec, typename Mat>
  double distanceSquared(const Vec& min, const Vec& max, const Mat& transform, const Vec& camera) {
    if (transform[0][3] != 0 || transform[1][3] != 0 || transform[2][3] != 0
        || transform[3][3] != 1) return -1;
    double center[3], half[3];
    for (uint32_t axis = 0; axis < 3; ++axis) {
      if (!std::isfinite(min[axis]) || !std::isfinite(max[axis]) || min[axis] > max[axis]) return -1;
      center[axis] = (double(min[axis]) + max[axis]) * 0.5;
      half[axis] = (double(max[axis]) - min[axis]) * 0.5;
    }
    double result = 0;
    for (uint32_t row = 0; row < 3; ++row) {
      double worldCenter = transform[3][row], worldHalf = 0;
      for (uint32_t col = 0; col < 3; ++col) {
        if (!std::isfinite(transform[col][row])) return -1;
        worldCenter += double(transform[col][row]) * center[col];
        worldHalf += std::abs(double(transform[col][row])) * half[col];
      }
      if (!std::isfinite(worldCenter) || !std::isfinite(worldHalf) || !std::isfinite(camera[row])) return -1;
      const double gap = std::max(std::abs(worldCenter - camera[row]) - worldHalf, 0.0);
      result += gap * gap;
    }
    return std::isfinite(result) ? result : -1;
  }

  inline bool eligible(double distanceSqr, float range, uint32_t frame, uint32_t lastSeen, bool pressure) {
    if (!std::isfinite(distanceSqr) || distanceSqr < 0 || !std::isfinite(range) || range < 0
        || lastSeen > frame || distanceSqr <= double(range) * range) return false;
    return frame - lastSeen >= (pressure ? pressureGraceFrames : normalGraceFrames);
  }
}
