// Included by d3d11_context.cpp only; no existing class layouts are changed.
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <sstream>

namespace dxvk::terrain_restore {
  bool diagnosticsEnabled() { return terrain_profile::diagnosticsEnabled(); }
  namespace {
    struct Entry {
      Policy policy;
      RestoreFn restore = nullptr;
      VerifyFn verify = nullptr;
      bool enabled = false, actualPending = false, tracked = false;
      bool continuousVerification = false;
      uint32_t verifyRemaining = 0, verifyPending = 0, finiteChecks = 0, finiteMismatches = 0;
      uint64_t skipped = 0, actualDrains = 0;
      uint64_t validationChecks = 0, validationMismatches = 0;
      uint64_t identity = 0;
      Settings settings;
      uint32_t lastFrame = ~0u, frames = 0, firstFrame = 0;
      uint64_t acceptedMin = ~0ull, acceptedMax = 0;
      uint64_t cameraFirst = 0, cameraLast = 0, cameraChanges = 0, cameraMissing = 0;
      uint64_t settingsTransitions = 0;
      bool haveSettings = false, haveCamera = false, haveTime = false;
      std::chrono::steady_clock::time_point previousEnd;
      uint64_t intervalNs = 0, intervals = 0, intervalMaxNs = 0;
    };
    struct Registry { std::mutex mutex; std::unordered_map<const void*, Entry> entries; uint64_t nextIdentity = 0; };
    // DLL teardown can destroy contexts after other statics. Keep the small registry
    // owner alive until process exit; context entries are removed by forget().
    Registry& registry() { static Registry* value = new Registry; return *value; }
    constexpr const char* metricNames[] = {
      "terrainDraws", "accepted", "eligible", "blockedMode", "blockedSpecial",
      "restores", "terrainRestores", "eligibleRestores", "restoreNs", "terrainRestoreNs",
      "eligibleRestoreNs", "uvReplay", "normalReplay", "uvReuse", "normalReuse",
      "chunks", "pendingChunks", "required", "unpaired",
      "meshDraws", "meshAccepted", "meshEligible", "meshBlockedMode", "meshBlockedSpecial"
    };
    constexpr const char* reasonNames[] = {
      "nonTerrain", "native", "dispatch", "capture", "backend", "clear", "commandList",
      "stateSwap", "injection", "frame", "explicit", "position", "other"
    };
    static_assert(sizeof(metricNames) / sizeof(*metricNames) == Count);
    static_assert(sizeof(reasonNames) / sizeof(*reasonNames) == uint32_t(Reason::Count));

    std::string report(const void* ctx, const Entry& e, bool partial) {
      std::ostringstream out;
      out << "[TerrainRestore V691] context=" << ctx << " frames=" << e.frames
          << " firstFrame=" << e.firstFrame << " lastFrame=" << e.lastFrame
          << " partial=" << partial << " settingsTransitions=" << e.settingsTransitions
          << " terrain=" << e.settings.terrain << " radius=" << e.settings.radius
          << " buildings=" << e.settings.buildings << " buildingRadius=" << e.settings.buildingRadius;
      for (uint32_t i = 0; i < Count; ++i) out << ' ' << metricNames[i] << '=' << e.policy.counters[i];
      const auto eligible = e.policy.counters[Eligible], required = e.policy.counters[Required];
      out << " potentialRemoved=" << (eligible >= required ? eligible - required : 0)
          << " maxRun=" << e.policy.maxRun << " acceptedMin=" << e.acceptedMin
          << " acceptedMax=" << e.acceptedMax << " cameraFirst=" << e.cameraFirst
          << " cameraLast=" << e.cameraLast << " cameraChanges=" << e.cameraChanges
          << " cameraMissing=" << e.cameraMissing << " intervals=" << e.intervals
          << " intervalNs=" << e.intervalNs << " intervalMaxNs=" << e.intervalMaxNs;
      for (uint32_t i = 0; i < uint32_t(Reason::Count); ++i)
        out << " drain_" << reasonNames[i] << '=' << e.policy.drains[i];
      out << " mode=" << (e.continuousVerification ? (e.enabled ? "verify" : "eager-verify") : (e.enabled ? "defer" : "eager"))
          << " skipped=" << e.skipped << " actualDrains=" << e.actualDrains
          << " actualPending=" << e.actualPending
          << " validationChecks=" << e.validationChecks << " validationMismatches=" << e.validationMismatches
          << " counts=drawSections timing=frontendEnqueue"
          << " verifyRemaining=" << e.verifyRemaining << " verifyPending=" << e.verifyPending
          << " policy=terrainAndOpaqueMeshesV707";
      return out.str();
    }
    void resetWindow(Entry& e) {
      // Frame boundary drained the hypothetical state. Do not reset context identity.
      e.policy.counters = {}; e.policy.drains = {}; e.policy.maxRun = 0;
      e.skipped = 0; e.actualDrains = 0;
      e.frames = 0; e.acceptedMin = ~0ull; e.acceptedMax = 0;
      e.cameraFirst = 0; e.cameraLast = 0; e.cameraChanges = 0; e.cameraMissing = 0;
      e.haveCamera = false; e.intervalNs = 0; e.intervals = 0; e.intervalMaxNs = 0;
      e.settingsTransitions = 0;
    }
  }
  const char* reasonName(Reason reason) {
    return uint32_t(reason) < uint32_t(Reason::Count) ? reasonNames[uint32_t(reason)] : "invalid";
  }
  void initialize(const void* ctx, RestoreFn restore, bool enabled, VerifyFn verify, bool continuousVerification) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto& e = r.entries[ctx];
    e.policy.collect = diagnosticsEnabled();
    e.restore = restore; e.enabled = enabled && restore != nullptr;
    e.verify = verify;
    e.continuousVerification = verify != nullptr && continuousVerification;
    e.verifyRemaining = e.verifyPending = e.finiteChecks = e.finiteMismatches = 0;
    e.identity = ++r.nextIdentity;
  }
  void requestFiniteVerification(const void* ctx, uint32_t count) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx); if (it == r.entries.end()) return;
    auto& e = it->second;
    if (!e.verify || e.continuousVerification || e.verifyPending || e.verifyRemaining) return;
    e.verifyRemaining = std::min(count, 64u);
    e.finiteChecks = e.finiteMismatches = 0;
    KENSHI_DIAGNOSTIC_INFO(str::format("[MeshRestore V707] verification armed limit=", e.verifyRemaining));
  }
  void stopFiniteVerification(const void* ctx) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx); if (it == r.entries.end()) return;
    auto& e = it->second;
    e.verifyRemaining = 0;
    KENSHI_DIAGNOSTIC_INFO(str::format("[MeshRestore V707] verification timingBoundary checks=", e.finiteChecks,
      " mismatches=", e.finiteMismatches, " pending=", e.verifyPending,
      " continuous=", e.continuousVerification, " deferral=", e.enabled));
  }
  uint64_t validationIdentity(const void* ctx) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx); return it == r.entries.end() ? 0 : it->second.identity;
  }
  bool validationResult(const void* ctx, uint64_t identity, bool equal) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx); if (it == r.entries.end()) return false;
    if (it->second.identity != identity) return false;
    ++it->second.validationChecks;
    if (!equal) ++it->second.validationMismatches;
    auto& e = it->second;
    if (e.verifyPending) {
      --e.verifyPending; ++e.finiteChecks;
      if (!equal) ++e.finiteMismatches;
      if (!e.verifyPending && !e.verifyRemaining)
        KENSHI_DIAGNOSTIC_INFO(str::format("[MeshRestore V707] verification complete checks=", e.finiteChecks,
          " mismatches=", e.finiteMismatches, " normalVerification=0"));
    }
    // The oracle repairs this boundary before its consumer. Later draws fall
    // back to eager restoration if the comparison disproves the invariant.
    if (!equal) e.enabled = false;
    return !equal && it->second.validationMismatches <= 8;
  }
  bool deferRestore(const void* ctx) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx); if (it == r.entries.end()) return false;
    auto& e = it->second;
    if (!e.enabled || (e.policy.scope & 6u) != 6u || !e.policy.expectedDrawRestore) return false;
    // Called only AFTER the independent geometry commit was successfully queued.
    e.actualPending = true;
    e.policy.expectedDrawRestore = false;
    if (e.policy.collect) ++e.skipped;
    return true;
  }
  uint32_t begin(const void* ctx, bool terrain, bool candidate) {
    if (!candidate) demand(ctx, Reason::NonTerrain);
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx);
    if (it == r.entries.end()) {
      if (!terrain && !candidate) return 0;
      it = r.entries.emplace(ctx, Entry{}).first;
    }
    it->second.tracked |= terrain || candidate;
    it->second.policy.collect = diagnosticsEnabled();
    return it->second.policy.begin(terrain, candidate);
  }
  void end(const void* ctx, uint32_t previous) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx); if (it != r.entries.end()) it->second.policy.end(previous);
  }
  void demand(const void* ctx, Reason reason) {
    RestoreFn restore = nullptr;
    VerifyFn verify = nullptr;
    VerificationBoundary boundary;
    {
      auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
      auto it = r.entries.find(ctx);
      if (it == r.entries.end()) return;
      boundary = { reason, it->second.lastFrame, it->second.policy.scope,
                   it->second.actualPending, it->second.enabled };
      it->second.policy.demand(reason);
      if (it->second.actualPending) restore = it->second.restore;
      // Clear/Reset intentionally replace frontend state; their pre-clear injection
      // and subsequent native consumers have their own verification boundaries.
      auto& e = it->second;
      if (e.tracked && reason != Reason::Clear && e.verify
       && kenshi_telemetry::enabled() && (e.continuousVerification || e.verifyRemaining)) {
        verify = e.verify;
        if (!e.continuousVerification) { --e.verifyRemaining; ++e.verifyPending; }
      }
    }
    // Context recording is serialized by its D3D11 lock. Never hold the registry
    // lock here: RestoreState records chunks and calls back into this registry.
    // RestoreTimer clears actualPending only after the entire restore is queued.
    if (restore) restore(ctx);
    if (verify) verify(ctx, boundary);
  }
  void commit(const void* ctx, bool safe, bool willRestore) {
    bool eligible = false;
    {
      auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
      auto it = r.entries.find(ctx);
      eligible = it != r.entries.end() && (it->second.policy.scope & 2u) && safe && willRestore;
    }
    if (!eligible) demand(ctx, Reason::Backend);
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx); if (it != r.entries.end()) it->second.policy.commit(safe, willRestore);
  }
  void capture(const void* ctx, bool normal, bool replay) {
    if (replay) demand(ctx, Reason::Capture);
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx); if (it != r.entries.end()) it->second.policy.capture(normal, replay);
  }
  void chunk(const void* ctx) {
    if (!diagnosticsEnabled()) return; // This hook only counts command sections.
    terrain_profile::count(terrain_profile::Chunks);
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx); if (it != r.entries.end()) it->second.policy.chunk();
  }
  void forget(const void* ctx) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); r.entries.erase(ctx);
  }
  uint32_t beginRestore(const void* ctx) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx); return it != r.entries.end() ? it->second.policy.beginRestore() : 0;
  }
  void finishRestore(const void* ctx, uint32_t flags, uint64_t ns) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.entries.find(ctx);
    if (it != r.entries.end()) {
      auto& e = it->second;
      if (e.actualPending && e.policy.collect) ++e.actualDrains;
      e.actualPending = false;
      e.policy.finishRestore(flags, ns);
    }
  }
  void frame(const void* ctx, uint32_t id, Settings settings, uint64_t camera, bool validCamera) {
    demand(ctx, Reason::Frame);
    std::string line;
    {
      auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
      auto it = r.entries.find(ctx); if (it == r.entries.end()) return;
      auto& e = it->second;
      if (!e.tracked) return;
      if (e.lastFrame == id) return;
      e.policy.demand(Reason::Frame);
      if (!diagnosticsEnabled()) {
        e.lastFrame = id; // Verification boundaries still need the current frame.
        if (e.frames) resetWindow(e);
        e.policy.frameAccepted = 0;
        e.haveTime = false;
        return;
      }
      if (e.haveSettings && !(settings == e.settings)) ++e.settingsTransitions;
      e.settings = settings; e.haveSettings = true;
      if (e.frames == 0) e.firstFrame = id;
      e.lastFrame = id; ++e.frames;
      e.acceptedMin = std::min(e.acceptedMin, e.policy.frameAccepted);
      e.acceptedMax = std::max(e.acceptedMax, e.policy.frameAccepted);
      e.policy.frameAccepted = 0;
      if (validCamera) {
        if (!e.haveCamera) { e.cameraFirst = camera; e.haveCamera = true; }
        else if (camera != e.cameraLast) ++e.cameraChanges;
        e.cameraLast = camera;
      } else ++e.cameraMissing;
      const auto now = std::chrono::steady_clock::now();
      if (e.haveTime) {
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - e.previousEnd).count());
        e.intervalNs += ns; ++e.intervals; e.intervalMaxNs = std::max(e.intervalMaxNs, ns);
      }
      e.previousEnd = now; e.haveTime = true;
      if (e.frames >= 120) { line = report(ctx, e, false); resetWindow(e); }
    }
    // Formatting once/120frames; never hold the registry lock during log I/O.
    if (!line.empty()) Logger::info(line);
  }
}
