#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dxvk::kenshi_terrain_culling {
  inline bool finite(float x) { return x == x && x > -1.0e20f && x < 1.0e20f; }

  inline bool cameraPosition(const void* wrapper, float (&position)[3]) {
    if (!wrapper) return false;
    std::memcpy(position, static_cast<const std::uint8_t*>(wrapper) + 8, sizeof(position));
    return finite(position[0]) && finite(position[1]) && finite(position[2]);
  }

  inline bool rescue(const float* box, const float* position, double radiusSquared,
      std::uint32_t incoming, std::uint32_t nativeResult) {
    if (!box || !position || nativeResult != 0 || incoming <= 1 || (incoming & ~126u)
        || !(radiusSquared > 0)) return false;
    std::uint32_t extent;
    std::memcpy(&extent, box + 6, sizeof(extent));
    if (extent != 1) return false;
    double distanceSquared = 0;
    for (unsigned i = 0; i < 3; ++i) {
      const double lo = box[i], hi = box[i + 3], x = position[i];
      if (!finite(float(lo)) || !finite(float(hi)) || !finite(float(x)) || lo > hi) return false;
      const double delta = x < lo ? lo - x : x > hi ? x - hi : 0;
      distanceSquared += delta * delta;
    }
    return distanceSquared <= radiusSquared;
  }
}
