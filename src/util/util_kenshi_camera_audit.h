#pragma once
#include <array>
#include <cstdint>

// Native operations and their immutable submitted-frame cut state.
namespace dxvk::kenshi_camera_audit {
using Handle = std::array<uint32_t, 5>;
using Position = std::array<float, 3>;
enum class CutReason : uint32_t { None, Teleport, Follow, Load, Reset, CameraReplacement };
struct Pose {
  uintptr_t camera = 0, ogreCamera = 0;
  Handle target{};
  Position offset{}, center{};
  std::array<float, 16> view{}; // Native OGRE row-major view, copied on its thread.
  bool valid = false, main = false, hasView = false;
};
struct Snapshot {
  Pose pose;
  uint64_t version = 0, requests = 0, pendingRequest = 0;
  uint64_t cuts = 0;
  CutReason reason = CutReason::None;
  uint32_t generation = 0, nativeThread = 0, busy = 0;
  bool installed = false;
  bool pendingFollowCut = false;
};
struct Submission {
  Snapshot native;
  uint32_t thread = 0;
  bool stable = false;
};
struct Binding {
  const void* owner = nullptr;
  const Submission* submission = nullptr;
};
inline thread_local Binding binding;

// The snapshot lives in the same CS lambda as the submitted transforms. Scope
// prevents a later native snapshot or an external/carryover camera borrowing it.
class SubmissionScope {
public:
  SubmissionScope(const void* owner, const Submission& value) : previous(binding) {
    binding = {owner, &value};
  }
  ~SubmissionScope() { binding = previous; }
  SubmissionScope(const SubmissionScope&) = delete;
  SubmissionScope& operator=(const SubmissionScope&) = delete;
private:
  Binding previous;
};
inline const Submission* submitted(const void* owner) {
  return binding.owner == owner ? binding.submission : nullptr;
}
inline bool coherent(uint64_t before, const Snapshot& after) {
  return after.installed && after.pose.valid && after.pose.hasView && !after.busy && before == after.version;
}

// Accept once for the winning Main draw, before RtCamera::update queries cuts.
// All later consumers see this frame's decision, outside the submission scope too.
struct CutState {
  const void* owner = nullptr;
  const void* camera = nullptr;
  uint32_t frame = UINT32_MAX, generation = 0;
  uint64_t serial = 0;
  CutReason reason = CutReason::None;
  bool initialized = false, available = false, cut = false;
};
inline thread_local CutState cutState;
inline void accept(const void* owner, const void* camera, uint32_t frame, const Submission* source) {
  auto& s = cutState;
  if (s.owner != owner || s.camera != camera) { s = {}; s.owner = owner; s.camera = camera; }
  if (s.frame == frame) return;
  s.frame = frame;
  s.cut = false;
  s.reason = CutReason::None;
  s.available = source && source->stable && source->native.installed
    && source->native.pose.main && source->native.pose.valid && !source->native.busy
    && source->native.nativeThread == source->thread;
  // Unavailable native integration never enables the rejected movement heuristic.
  if (!s.available) return;
  const auto& n = source->native;
  const bool replacement = s.initialized && n.generation != s.generation;
  s.cut = s.initialized && (replacement || n.cuts > s.serial);
  if (s.cut) s.reason = replacement ? CutReason::CameraReplacement : n.reason;
  s.serial = n.cuts;
  s.generation = n.generation;
  s.initialized = true;
}
inline bool isCut(const void* camera, uint32_t frame) {
  return cutState.camera == camera && cutState.frame == frame && cutState.cut;
}
}
