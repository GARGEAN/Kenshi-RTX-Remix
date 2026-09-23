// Included inside namespace dxvk in rtx_scene_manager.cpp only.
namespace terrain_origin {
struct Frame {
  uint32_t id = UINT32_MAX;
  bool consistent = true;
  bool cameraValid = false;
  Matrix4 origin;
  std::vector<uint64_t> instances;
};
struct State {
  const void* owner = nullptr;
  Frame previous, current;
  uint32_t repairedFrame = UINT32_MAX;
};
static thread_local State history;

static bool translationOnly(const Matrix4& m) {
  for (uint32_t c = 0; c < 4; ++c)
    for (uint32_t r = 0; r < 4; ++r)
      if (!std::isfinite(m[c][r]) ||
          ((c != 3 || r == 3) && m[c][r] != (c == r ? 1.f : 0.f)))
        return false;
  return true;
}

static void observe(const void* owner, uint32_t frame, const DrawCallState& draw,
                    const RtInstance* instance) {
  const auto shader = draw.programmableVertexShaderBytecodeHash;
  if (!instance || draw.getSkinningState().numBones != 0u ||
      (shader != 0xdf5c7e0b230c7f2eull && shader != 0x3a2f0355b844fbf6ull))
    return;
  if (history.owner != owner) {
    history = State{};
    history.owner = owner;
  }
  auto& current = history.current;
  if (current.id != frame) {
    history.previous = std::move(current);
    current = Frame{};
    current.id = frame;
    current.origin = draw.getTransformData().objectToWorld;
  }
  const auto& origin = draw.getTransformData().objectToWorld;
  current.consistent &= translationOnly(origin) && origin == current.origin;
  current.instances.push_back(instance->getId());
}

// Keep the previous camera and previous geometry in the SAME native coordinate
// frame. New ground identities have no per-instance history, but native terrain
// vertices carry absolute placement: the last frame's common ground transform
// is their correct previous transform too. A bright raw world-space vector on
// this frame is expected; it cancels the coordinate shift in camera reprojection.
static void repair(const void* owner, uint32_t frame,
                   const std::vector<RtInstance*>& instances, const RtCamera& camera) {
  auto& s = history;
  auto& previous = s.previous;
  auto& current = s.current;
  if (s.owner == owner && current.id == frame)
    current.cameraValid = camera.getLastUpdateFrame() == frame;
  if (s.owner != owner || frame == 0 || current.id != frame ||
      previous.id != frame - 1 || s.repairedFrame == frame ||
      !previous.consistent || !current.consistent || !previous.cameraValid ||
      previous.instances.size() < 8 || current.instances.size() < 8 ||
      RtCamera::enableFreeCamera() || camera.getLastUpdateFrame() != frame)
    return;
  const Vector3 shift = current.origin[3].xyz() - previous.origin[3].xyz();
  const Vector3 cameraDelta = Vector3(camera.getViewToWorld(false)[3].xyz()
    - camera.getPreviousViewToWorld(false)[3].xyz());
  const float shiftSquared = lengthSqr(shift);
  const float residualSquared = lengthSqr(cameraDelta - shift);
  const float cutSquared = RtxOptions::getUniqueObjectDistanceSqr();
  // Require an agreed horizontal origin jump plus otherwise ordinary camera
  // travel. A real camera teleport, missing frame, or mixed origins fail closed.
  if (shift.y != 0.f || !std::isfinite(shiftSquared) ||
      !std::isfinite(residualSquared) || shiftSquared <= 16.f * cutSquared ||
      residualSquared >= cutSquared || residualSquared >= .0025f * shiftSquared)
    return;
  std::sort(current.instances.begin(), current.instances.end());
  current.instances.erase(std::unique(current.instances.begin(), current.instances.end()),
                          current.instances.end());
  std::sort(previous.instances.begin(), previous.instances.end());
  previous.instances.erase(std::unique(previous.instances.begin(), previous.instances.end()),
                           previous.instances.end());
  if (current.instances.size() < 8 || previous.instances.size() < 8)
    return;
  s.repairedFrame = frame;
  for (RtInstance* instance : instances) {
    if (!instance || !std::binary_search(current.instances.begin(), current.instances.end(), instance->getId()))
      continue;
    auto& surface = instance->surface;
    if (instance->getFrameLastUpdated() != frame || surface.objectToWorld != current.origin ||
        surface.previousPositionBufferIndex != BINDING_INDEX_INVALID || instance->isCreatedByRenderer()) {
      continue;
    }
    surface.prevObjectToWorld = previous.origin;
    surface.isStatic = false;
  }
}

}
