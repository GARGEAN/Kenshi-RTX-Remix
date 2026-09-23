#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <cstring>
#include <string>
#include <vector>

namespace dxvk { class DxvkContext; }

// V691: per-context restoration barriers; the V690 count model remains as an oracle.
namespace dxvk::terrain_restore {
  bool diagnosticsEnabled();
  std::vector<uint8_t> snapshotNativeState(DxvkContext* context);
  enum class Reason : uint32_t {
    NonTerrain, NativeDraw, Dispatch, Capture, Backend, Clear, CommandList,
    StateSwap, Injection, Frame, Explicit, Position, Other, Count
  };
  enum Metric : uint32_t {
    Draws, Accepted, Eligible, BlockedMode, BlockedSpecial,
    Restores, TerrainRestores, EligibleRestores, RestoreNs, TerrainRestoreNs,
    EligibleRestoreNs, UvReplay, NormalReplay, UvReuse, NormalReuse,
    Chunks, PendingChunks, Required, Unpaired,
    MeshDraws, MeshAccepted, MeshEligible, MeshBlockedMode, MeshBlockedSpecial, Count
  };
  struct Settings {
    bool terrain = false;
    float radius = 0;
    bool buildings = false;
    float buildingRadius = 0;
    bool operator==(const Settings& b) const {
      return terrain == b.terrain && radius == b.radius
          && buildings == b.buildings && buildingRadius == b.buildingRadius;
    }
  };

  // Same policy used by production instrumentation and isolated tests.
  struct Policy {
    bool collect = true; // Production sets this from the explicit diagnostic mode.
    std::array<uint64_t, Count> counters{};
    std::array<uint64_t, uint32_t(Reason::Count)> drains{};
    uint32_t scope = 0; // terrain=1, candidate=2, accepted eligible=4
    bool pending = false;
    bool expectedDrawRestore = false;
    uint64_t run = 0, maxRun = 0;
    uint64_t frameAccepted = 0;
    void demand(Reason reason) {
      if (!pending) return;
      if (collect) {
        ++counters[Required];
        ++drains[uint32_t(reason)];
      }
      pending = false;
      run = 0;
    }
    uint32_t begin(bool terrain, bool candidate) {
      const uint32_t previous = scope;
      if (!candidate) demand(Reason::NonTerrain);
      scope = (terrain ? 1u : 0u) | (candidate ? 2u : 0u);
      if (collect) {
        if (terrain) ++counters[Draws];
        else ++counters[MeshDraws];
      }
      return previous;
    }
    void end(uint32_t previous) {
      if (expectedDrawRestore) {
        if (collect) ++counters[Unpaired];
        expectedDrawRestore = false;
        demand(Reason::Other);
      }
      scope = previous;
    }
    void commit(bool safe, bool willRestore) {
      const bool eligible = (scope & 2u) && safe && willRestore;
      if (collect && (scope & 1u)) {
        ++counters[Accepted];
        ++frameAccepted;
        ++counters[eligible ? Eligible : ((scope & 2u) ? BlockedSpecial : BlockedMode)];
      } else if (collect) {
        ++counters[MeshAccepted];
        ++counters[eligible ? MeshEligible : ((scope & 2u) ? MeshBlockedSpecial : MeshBlockedMode)];
      }
      if (eligible) {
        scope |= 4u;
        pending = true;
        if (collect && ++run > maxRun) maxRun = run;
      } else {
        demand(Reason::Backend);
      }
      expectedDrawRestore = willRestore;
    }
    uint32_t beginRestore() {
      if (!expectedDrawRestore) demand(Reason::Explicit);
      expectedDrawRestore = false;
      return scope;
    }
    void finishRestore(uint32_t flags, uint64_t ns) {
      if (!collect) return;
      ++counters[Restores]; counters[RestoreNs] += ns;
      if (flags & 1u) { ++counters[TerrainRestores]; counters[TerrainRestoreNs] += ns; }
      if (flags & 4u) { ++counters[EligibleRestores]; counters[EligibleRestoreNs] += ns; }
    }
    void capture(bool normal, bool replay) {
      if (collect && (scope & 1u)) ++counters[replay ? (normal ? NormalReplay : UvReplay)
                                     : (normal ? NormalReuse : UvReuse)];
      if (replay) demand(Reason::Capture);
    }
    void chunk() {
      if (!collect) return;
      ++counters[Chunks];
      if (pending) ++counters[PendingChunks];
    }
  };

  uint32_t begin(const void* context, bool terrain, bool candidate);
  using RestoreFn = void (*)(const void*);
  struct VerificationBoundary {
    Reason reason = Reason::Other;
    uint32_t lastCompletedFrame = ~0u;
    uint32_t scope = 0;
    bool pendingBeforeDrain = false;
    bool deferralEnabled = false;
  };
  using VerifyFn = void (*)(const void*, VerificationBoundary);
  struct Mode { bool defer, verify; };
  inline Mode selectMode(const std::string& value) {
    // V696: normal launches retain terrain deferral without the expensive oracle.
    // Eager controls and verification remain explicit diagnostic launch modes.
    return { value.empty() || value == "1" || value == "verify" || value == "defer-verify",
             value == "eager-verify" || value == "verify" || value == "defer-verify" };
  }
  inline uintptr_t snapshotComputeShader(const std::vector<uint8_t>& bytes) {
    // Snapshot begins with VS/TCS/TES/GS/FS/CS pointer identities, in that order.
    uintptr_t result = 0;
    if (bytes.size() >= 6 * sizeof(result))
      std::memcpy(&result, bytes.data() + 5 * sizeof(result), sizeof(result));
    return result;
  }
  const char* reasonName(Reason reason);
  void initialize(const void* context, RestoreFn restore, bool enabled, VerifyFn verify = nullptr,
                  bool continuousVerification = true);
  void requestFiniteVerification(const void* context, uint32_t count);
  void stopFiniteVerification(const void* context);
  uint64_t validationIdentity(const void* context);
  bool validationResult(const void* context, uint64_t identity, bool equal);
  bool deferRestore(const void* context);
  void end(const void* context, uint32_t previous);
  void demand(const void* context, Reason reason);
  void commit(const void* context, bool safe, bool willRestore);
  void capture(const void* context, bool normal, bool replay);
  void chunk(const void* context);
  void forget(const void* context);
  void frame(const void* context, uint32_t id, Settings settings, uint64_t camera, bool validCamera);
  uint32_t beginRestore(const void* context);
  void finishRestore(const void* context, uint32_t flags, uint64_t ns);

  struct DrawScope {
    const void* context;
    uint32_t previous;
    DrawScope(const void* ctx, bool terrain, bool candidate)
      : context(ctx), previous(begin(ctx, terrain, candidate)) { }
    ~DrawScope() { end(context, previous); }
  };
  struct RestoreTimer {
    const void* context;
    uint32_t flags;
    bool measured;
    std::chrono::steady_clock::time_point start;
    int exceptions;
    explicit RestoreTimer(const void* ctx)
      : context(ctx), flags(beginRestore(ctx)), measured(diagnosticsEnabled()),
        start(measured ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}),
        exceptions(std::uncaught_exceptions()) { }
    ~RestoreTimer() {
      const auto ns = measured ? std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count() : 0;
      // A partially recorded restore does not establish clean state.
      if (std::uncaught_exceptions() == exceptions)
        finishRestore(context, flags, uint64_t(ns));
    }
  };
}
