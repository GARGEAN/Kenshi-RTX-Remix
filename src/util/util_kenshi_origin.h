#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_set>

// Native values are copied on the immediate-context thread and delivered by an
// ordered CS command. Never read OGRE's newest origin on the CS worker.
namespace dxvk::kenshi_origin {
struct Snapshot {
  uintptr_t manager = 0;
  std::array<float, 3> origin{};
  bool valid = false;
  bool operator==(const Snapshot& b) const {
    return manager == b.manager && origin == b.origin && valid == b.valid;
  }
};
struct State {
  const void* owner = nullptr;
  Snapshot snapshot;
  uint32_t snapshotFrame = UINT32_MAX, eventFrame = UINT32_MAX;
  std::array<float, 3> shift{}; // Old native world -> new native world: old O - new O.
  bool continuous = false;
  const void* camera = nullptr;
  std::unordered_set<const void*> nativeTracking;
  std::unordered_set<uint64_t> retainedMotion;
  uint32_t retainedFrame = UINT32_MAX;
};
inline thread_local State state;

inline bool finite(const Snapshot& s) {
  return s.valid && s.manager && std::isfinite(s.origin[0])
    && std::isfinite(s.origin[1]) && std::isfinite(s.origin[2]);
}

// Returns true once per observed native shift. Missing/ambiguous frame history
// still permits placement maintenance, but never exempts a camera cut.
inline bool receive(const void* owner, uint32_t frame, const Snapshot& value) {
  auto& s = state;
  if (s.owner != owner) { s = State{}; s.owner = owner; }
  const bool sameManager = finite(value) && finite(s.snapshot)
    && value.manager == s.snapshot.manager;
  const bool changed = sameManager && value.origin != s.snapshot.origin;
  if (changed) {
    for (uint32_t i = 0; i < 3; ++i) s.shift[i] = s.snapshot.origin[i] - value.origin[i];
    if (!std::isfinite(s.shift[0]) || !std::isfinite(s.shift[1]) || !std::isfinite(s.shift[2])) {
      s.snapshot = value; s.snapshotFrame = frame;
      s.continuous = false; s.eventFrame = UINT32_MAX; s.camera = nullptr;
      return false;
    }
    s.continuous = frame > 0 && s.snapshotFrame == frame - 1 && s.shift[1] == 0.f;
    s.eventFrame = frame;
    s.camera = nullptr;
  } else if (!sameManager) {
    s.continuous = false;
    s.eventFrame = UINT32_MAX;
    s.camera = nullptr;
  }
  s.snapshot = value;
  s.snapshotFrame = frame;
  return changed;
}

inline std::array<float, 3> cameraShift(const void* camera, uint32_t frame) {
  return state.camera == camera && state.continuous && state.eventFrame == frame
    ? state.shift : std::array<float, 3>{};
}
}
