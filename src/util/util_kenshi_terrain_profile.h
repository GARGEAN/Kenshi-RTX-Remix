#pragma once

// Opt-in CPU accounting. No renderer class or CPU/GPU layout changes.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <unordered_set>
#include "util_kenshi_telemetry.h"
#ifndef KENSHI_PROFILE_TEST
#include "util_env.h"
#include "log/log.h"
#include <windows.h>
#endif

namespace dxvk::terrain_profile {
  enum class Stage : uint32_t {
    Bridge, Index, Bounds, Transform, Material, TerrainMaterial, Hash,
    Capture, Enqueue, Restore, Backend, Futures, SceneMaterial, Geometry,
    Instance, Scene, Texture, TerrainArgs, PreparedLookup, PreparedReuse,
    SceneMaintenance, SceneResources, SceneAuditInstances, SceneAuditBuffers,
    SceneParticles, SceneVirtualInstances, SceneLights, SceneMaterials, SceneExtensions,
    AccelMerge, AccelClassify, AccelDynamic, AccelRestore, AccelBuffers,
    AccelPrepare, AccelSurfaceUpload, AccelParticleMap, AccelBlas, AccelTlas,
    BlasOwners, BlasOmm, BlasIssue, BlasFlush, BlasRetrack, BlasDestinations, BlasInputs,
    BridgeAdmission, BridgeStreams, BridgeSkin, BridgeExtensions, OpaquePreparation,
    TintDiagnostics, SnapshotCopy, CameraKey, CpuInterleave, OgreLayout, OgreRecovery, WorkerFutureWait, Count
  };
  enum Counter : uint32_t {
    Draws, Accepted, Bypassed, IndexValues, BoundsVertices, NamedConstants,
    LayerViews, UvReplay, NormalReplay, UvReuse, NormalReuse, Chunks,
    CacheHit, CacheMiss, MeshBuild, MeshUpdate, InstanceOnly, TerrainPurged,
    BoundsEligible, BoundsHit, BoundsMiss, BoundsAvoided, BoundsVerified, BoundsMismatch,
    SkinDraws, SkinNew, SkinTopology, SkinPose, SkinVertex, SkinShader, SkinUpdate, SkinInstance,
    RigidDraws, RigidNew, RigidTopology, RigidPose, RigidVertex, RigidShader, RigidUpdate, RigidInstance,
    ParticleDraws, ParticleNew, ParticleTopology, ParticlePose, ParticleVertex, ParticleShader, ParticleUpdate, ParticleInstance,
    VkBlasDescriptors, VkBlasBuilds, VkBlasUpdates, VkBlasCalls, VkBlasFlushes, VkBlasPrimitives,
    OwnerPoolEntries, OwnerDestinations, OwnerTracks, GeometryRetracks,
    InputListBuilds, InputUniqueBuffers, InputBufferTracks, InputBarriers,
    CameraRecipeHit, CameraRecipeFallback, OpaquePreparationHit, OpaquePreparationMiss, NativeSkinRestore,
    TintBudgetSkipped, TintVertices, SnapshotBuffers, SnapshotBytes, SnapshotCached, SnapshotUncached,
    CachedHelperReuse, CachedHelperNew, CpuInterleaveVertices, CpuInterleaveSkinned, CpuInterleaveUnskinned,
    BatchInputLists, FullInputLists, BatchInputFallback, InputTracksAvoided,
    OgreLayoutReads, OgreLayoutCopies, OgreCandidates, OgreViable, OgreRejected, OgreRecovered,
    WorkerFutureReady, WorkerFutureBlocked, Count
  };
  constexpr uint32_t stageCount = uint32_t(Stage::Count);
  constexpr const char* stageNames[] = { "bridgeOther", "indexScan", "bounds", "transform",
    "material", "terrainMaterial", "hash", "capture", "enqueue", "restore",
    "backendOther", "futureResolve", "sceneMaterial", "geometry", "instance", "scene",
    "texture", "terrainArgs", "preparedLookup", "preparedReuse",
    "sceneMaintenance", "sceneResources", "sceneAuditInstances", "sceneAuditBuffers",
    "sceneParticles", "sceneVirtualInstances", "sceneLights", "sceneMaterials", "sceneExtensions",
    "accelMerge", "accelClassify", "accelDynamic", "accelRestore", "accelBuffers",
    "accelPrepare", "accelSurfaceUpload", "accelParticleMap", "accelBlas", "accelTlas",
    "blasOwners", "blasOmm", "blasIssue", "blasFlush", "blasRetrack", "blasDestinations", "blasInputs",
    "bridgeAdmission", "bridgeStreams", "bridgeSkin", "bridgeExtensions", "opaquePreparation",
    "tintDiagnostics", "snapshotCopy", "cameraKey", "cpuInterleave", "ogreLayout", "ogreRecovery", "workerFutureWait" };
  constexpr const char* counterNames[] = { "draws", "accepted", "bypassed", "indexValues",
    "boundsVertices", "namedConstants", "layerViews", "uvReplay", "normalReplay",
    "uvReuse", "normalReuse", "chunks", "cacheHit", "cacheMiss", "meshBuild",
    "meshUpdate", "instanceOnly", "terrainPurged", "boundsEligible", "boundsHit",
    "boundsMiss", "boundsAvoided", "boundsVerified", "boundsMismatch",
    "skinDraws", "skinNew", "skinTopology", "skinPose", "skinVertex", "skinShader", "skinUpdate", "skinInstance",
    "rigidDraws", "rigidNew", "rigidTopology", "rigidPose", "rigidVertex", "rigidShader", "rigidUpdate", "rigidInstance",
    "particleDraws", "particleNew", "particleTopology", "particlePose", "particleVertex", "particleShader", "particleUpdate", "particleInstance",
    "vkBlasDescriptors", "vkBlasBuilds", "vkBlasUpdates", "vkBlasCalls", "vkBlasFlushes", "vkBlasPrimitives",
    "ownerPoolEntries", "ownerDestinations", "ownerTracks", "geometryRetracks",
    "inputListBuilds", "inputUniqueBuffers", "inputBufferTracks", "inputBarriers",
    "cameraRecipeHit", "cameraRecipeFallback", "opaquePreparationHit", "opaquePreparationMiss", "nativeSkinRestore",
    "tintBudgetSkipped", "tintVertices", "snapshotBuffers", "snapshotBytes", "snapshotCached", "snapshotUncached",
    "cachedHelperReuse", "cachedHelperNew", "cpuInterleaveVertices", "cpuInterleaveSkinned", "cpuInterleaveUnskinned",
    "batchInputLists", "fullInputLists", "batchInputFallback", "inputTracksAvoided",
    "ogreLayoutReads", "ogreLayoutCopies", "ogreCandidates", "ogreViable", "ogreRejected", "ogreRecovered",
    "workerFutureReady", "workerFutureBlocked" };
  // Finite checks are armed/stopped on their owning ordered CPU lanes.
  inline thread_local uint32_t cityVerifyRemaining = 0;
  inline thread_local uint32_t cityVerified = 0, cityMismatches = 0;
  static_assert(sizeof(stageNames) / sizeof(*stageNames) == stageCount);
  static_assert(sizeof(counterNames) / sizeof(*counterNames) == Count);
  // Switch lanes only at ordered frame/CS-command boundaries. Queued backend work must not lose profiling
  // when the main thread reaches the end.
  inline thread_local bool timedLane = false;
  inline thread_local uint64_t captureId = 0;
#ifdef KENSHI_PROFILE_TEST
  inline bool testEnabled = false;
  inline uint64_t testNow = 0, testClockCalls = 0;
  inline bool launchEnabled() { return testEnabled; }
  inline bool enabled() { return launchEnabled() || timedLane; }
  inline uint64_t now() { ++testClockCalls; return testNow; }
  inline uint64_t threadCpu() { return testNow; }
#else
  inline bool launchEnabled() {
    static const bool value = env::getEnvVar("DXVK_KENSHI_TERRAIN_PROFILE") == "1";
    return value;
  }
  inline bool enabled() { return kenshi_telemetry::enabled() && (launchEnabled() || timedLane); }
  inline uint64_t now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  inline uint64_t threadCpu() {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    const auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) * 100u;
  }
#endif
  inline std::atomic<bool> requestedBypass { false };
  struct Totals {
    std::array<uint64_t, stageCount> exclusive {}, inclusive {}, calls {};
    std::array<uint64_t, Count> counters {};
  };
  struct Scope;
  struct Settings {
    bool anti = false, buildings = false, bypass = false;
    float radius = 0, buildingRadius = 0;
    bool boundsCache = true;
    bool operator==(const Settings& b) const {
      return anti == b.anti && buildings == b.buildings && bypass == b.bypass
        && radius == b.radius && buildingRadius == b.buildingRadius && boundsCache == b.boundsCache;
    }
  };
  struct ThreadState {
    Scope* active = nullptr;
    uint32_t group = 2; // terrain / nonterrain / frame-wide or outside a draw
    bool bypass = false;
    std::array<Totals, 3> totals {};
    uint32_t frames = 0, firstFrame = 0;
    uint64_t previousTime = 0, previousCpu = 0, intervalNs = 0, cpuNs = 0, maxNs = 0;
    uint64_t intervals = 0, settingsChanges = 0, cameraChanges = 0, cameraMissing = 0;
    uint64_t firstCamera = 0, lastCamera = 0;
    Settings settings {};
    bool haveSettings = false;
  };
  inline thread_local ThreadState state;
  // CS-thread-only pointers, erased at the owner's normal destruction boundary.
  inline thread_local std::unordered_set<const void*> terrainOwners;
  inline std::array<std::atomic<uint64_t>, 2> workerNs {}, workerJobs {};
  enum class TimedAction { None, Armed, Start, Stop, Cancelled };
  struct TimedControl {
    enum class Phase { Idle, Waiting, Running } phase = Phase::Idle;
    uint64_t readyAt = 0, deadline = 0, serial = 0;
    TimedAction advance(bool request, bool cameraValid, uint64_t time) {
      if (phase == Phase::Idle && request) {
        phase = Phase::Waiting; readyAt = time + 2000000000ull;
        deadline = time + 10000000000ull;
        return TimedAction::Armed;
      }
      if (phase == Phase::Waiting) {
        if (time >= deadline) { phase = Phase::Idle; return TimedAction::Cancelled; }
        if (time >= readyAt && cameraValid) {
          phase = Phase::Running; deadline = time + 20000000000ull;
          ++serial; return TimedAction::Start;
        }
      } else if (phase == Phase::Running && time >= deadline) {
        phase = Phase::Idle; return TimedAction::Stop;
      }
      return TimedAction::None;
    }
  };
  inline void beginTimedLane(uint64_t id) {
    state = {}; terrainOwners.clear();
    captureId = id; timedLane = true;
    state.previousTime = now(); state.previousCpu = threadCpu();
  }
  inline void endTimedLane() {
    timedLane = false; captureId = 0;
    state = {}; terrainOwners.clear();
  }
#ifndef KENSHI_PROFILE_TEST
  inline TimedAction pollTimedCapture(bool cameraValid, uint64_t& id) {
    // Process-local, non-persistent one-shot control. No environment/config edit or launcher needed. Poll
    // one inexpensive event every 64 EndFrame calls.
    struct Control {
      HANDLE event = nullptr;
      uint32_t polls = 0;
      TimedControl timer;
      Control() {
        const auto name = L"Local\\KenshiTerrainCPUProfile-" + std::to_wstring(GetCurrentProcessId());
        event = CreateEventW(nullptr, FALSE, FALSE, name.c_str());
        KENSHI_DIAGNOSTIC_INFO(std::string("[TerrainCPU V705] timedControl=") + (event ? "ready" : "unavailable")
          + " warmupSeconds=2 durationSeconds=20 normalDefault=off");
      }
      ~Control() { if (event) CloseHandle(event); }
    };
    static Control* instance = nullptr;
    if (!kenshi_telemetry::enabled()) {
      if (!instance) return TimedAction::None;
      delete instance;
      instance = nullptr;
      return TimedAction::Cancelled;
    }
    if (!instance) instance = new Control;
    auto& control = *instance;
    const bool request = ((++control.polls & 63u) == 1u) && control.event
      && WaitForSingleObject(control.event, 0) == WAIT_OBJECT_0;
    if (launchEnabled()) {
      if (request) Logger::warn("[TerrainCPU V705] timed request ignored: launch-wide profiling already enabled.");
      return TimedAction::None;
    }
    if (!request && control.timer.phase == TimedControl::Phase::Idle) return TimedAction::None;
    const auto action = control.timer.advance(request, cameraValid, now());
    id = control.timer.serial;
    if (action == TimedAction::Armed)
      KENSHI_DIAGNOSTIC_INFO("[TerrainCPU V705] armed: waiting2seconds and a valid game camera; automatic20second capture follows.");
    if (action == TimedAction::Cancelled)
      Logger::warn("[TerrainCPU V705] cancelled: no valid game camera within10seconds; profiling remains off.");
    return action;
  }
#endif
  struct WorkerScope {
    int group;
    uint64_t start;
    explicit WorkerScope(int g) : group(g), start(g >= 0 ? now() : 0) { }
    ~WorkerScope() {
      if (group < 0) return;
      workerNs[group].fetch_add(now() - start, std::memory_order_relaxed);
      workerJobs[group].fetch_add(1, std::memory_order_relaxed);
    }
  };
  // Legacy reports are opt-in too. A finite CPU capture also enables them on its owning lane; this does
  // not enable the crash recorder or change its state.
  inline bool diagnosticsEnabled() {
#ifdef KENSHI_PROFILE_TEST
    return enabled();
#else
    return kenshi_telemetry::enabled();
#endif
  }
  inline void count(Counter counter, uint64_t n = 1) {
    if (enabled()) state.totals[state.group].counters[counter] += n;
  }
  // Mutually exclusive draw classes; change reasons may overlap.
  enum class GeometryReason : uint32_t { Draw, New, Topology, Pose, Vertex, Shader, Update, Instance };
  inline void geometryReason(bool skinned, bool particle, GeometryReason reason) {
    if (!enabled()) return;
    const uint32_t base = skinned ? SkinDraws : particle ? ParticleDraws : RigidDraws;
    count(static_cast<Counter>(base + uint32_t(reason)));
  }
  struct Scope {
    Scope* parent = nullptr;
    uint64_t start = 0, children = 0;
    uint32_t group = 2, previousGroup = 2;
    Stage stage;
    bool armed;
    explicit Scope(Stage s, int drawGroup = -1) : stage(s), armed(enabled()) {
      if (!armed) return;
      parent = state.active;
      previousGroup = state.group;
      if (drawGroup >= 0) state.group = uint32_t(drawGroup);
      group = state.group;
      state.active = this;
      start = now();
    }
    void finish() {
      if (!armed) return;
      const uint64_t elapsed = now() - start;
      auto& total = state.totals[group];
      const uint32_t i = uint32_t(stage);
      total.inclusive[i] += elapsed;
      total.exclusive[i] += elapsed >= children ? elapsed - children : 0;
      ++total.calls[i];
      if (parent) parent->children += elapsed;
      state.active = parent;
      state.group = previousGroup;
      armed = false;
    }
    ~Scope() { finish(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  };
  inline void owner(const void* p) {
    if (!enabled() || !p) return;
    if (state.group == 0) terrainOwners.insert(p);
    else terrainOwners.erase(p); // never retain terrain classification after reassociation
  }
  inline void forgetOwner(const void* p) { if (enabled()) terrainOwners.erase(p); }
  inline bool purgeOwner(const void* p) {
    return enabled() && state.bypass && terrainOwners.count(p) != 0;
  }
  inline bool bypass() { return enabled() && state.bypass; }
  inline bool shouldBypass(bool terrain, bool immediate, bool raytracing, bool safeBaseline,
                           bool passthrough, bool injected, bool hasColourTarget) {
    return bypass() && terrain && immediate && raytracing && !safeBaseline
      && !passthrough && !injected && hasColourTarget;
  }
  inline void frame(const char* lane, const void* context, uint32_t id,
                    Settings settings, uint64_t camera, bool cameraValid, bool final = false) {
    if (!enabled()) return;
    auto& t = state;
    const uint64_t time = now(), cpu = threadCpu();
    if (t.previousTime) {
      const uint64_t elapsed = time - t.previousTime;
      t.intervalNs += elapsed;
      if (elapsed > t.maxNs) t.maxNs = elapsed;
      ++t.intervals;
      if (cpu >= t.previousCpu) t.cpuNs += cpu - t.previousCpu;
    }
    t.previousTime = time; t.previousCpu = cpu;
    if (t.frames++ == 0) t.firstFrame = id;
    if (t.haveSettings && !(settings == t.settings)) ++t.settingsChanges;
    t.settings = settings; t.haveSettings = true;
    if (cameraValid) {
      if (!t.firstCamera) t.firstCamera = camera;
      if (t.lastCamera && camera != t.lastCamera) ++t.cameraChanges;
      t.lastCamera = camera;
    } else ++t.cameraMissing;
    if (t.frames < 120 && !final) return;
#ifndef KENSHI_PROFILE_TEST
    for (uint32_t group = 0; group < 3; ++group) {
      std::ostringstream out;
      out << "[TerrainCPU V697] lane=" << lane << " context=" << context
          << " group=" << (group == 0 ? "terrain" : group == 1 ? "nonterrain" : "frame")
          << " frames=" << t.frames << " firstFrame=" << t.firstFrame << " lastFrame=" << id
          << " capture=" << captureId << " final=" << final
          << " anti=" << settings.anti << " radius=" << settings.radius
          << " buildings=" << settings.buildings << " buildingRadius=" << settings.buildingRadius
          << " bypass=" << settings.bypass << " boundsCache=" << settings.boundsCache
          << " settingsChanges=" << t.settingsChanges
          << " cameraFirst=" << t.firstCamera << " cameraLast=" << t.lastCamera
          << " cameraChanges=" << t.cameraChanges << " cameraMissing=" << t.cameraMissing
          << " intervals=" << t.intervals << " intervalNs=" << t.intervalNs
          << " maxIntervalNs=" << t.maxNs << " threadCpuNs=" << t.cpuNs;
      uint64_t assigned = 0;
      for (uint32_t i = 0; i < stageCount; ++i) {
        assigned += t.totals[group].exclusive[i];
        out << ' ' << stageNames[i] << "Ns=" << t.totals[group].exclusive[i]
            << ' ' << stageNames[i] << "InclusiveNs=" << t.totals[group].inclusive[i]
            << ' ' << stageNames[i] << "Calls=" << t.totals[group].calls[i];
      }
      for (uint32_t i = 0; i < Count; ++i)
        out << ' ' << counterNames[i] << '=' << t.totals[group].counters[i];
      out << " measuredExclusiveNs=" << assigned << " retainedTerrainOwners=" << terrainOwners.size()
          << " completedHashWorkerNs=" << ((lane[0] == 'm' && group < 2)
               ? workerNs[group].exchange(0, std::memory_order_relaxed) : 0)
          << " completedHashJobs=" << ((lane[0] == 'm' && group < 2)
               ? workerJobs[group].exchange(0, std::memory_order_relaxed) : 0)
          << " units=nanoseconds policy=optInCPU scopes=exclusivePlusInclusive";
      Logger::info(out.str());
    }
#endif
    t.totals = {}; t.frames = 0; t.intervalNs = 0; t.cpuNs = 0; t.maxNs = 0;
    t.intervals = 0; t.settingsChanges = 0; t.cameraChanges = 0; t.cameraMissing = 0;
    t.firstCamera = 0; t.lastCamera = 0;
  }
}
