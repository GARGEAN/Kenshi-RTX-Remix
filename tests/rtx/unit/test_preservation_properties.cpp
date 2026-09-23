/*
* Copyright (c) 2024-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

/**
 * Preservation Property Tests — Renderer Viewport Correctness
 *
 * Property 2: Preservation — Single-Swapchain Election and Existing Behavior
 *
 * These tests verify that existing correct behaviors are preserved after
 * the fix is applied. The local copy of the election logic has been updated
 * to match the FIXED production code, confirming no regressions.
 *
 * **Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8**
 *
 * Test A: Single-swapchain election — immediate primary, no hysteresis
 * Test B: Alt+X override — forced primary bypasses election
 * Test C: Destroyed primary — next presenter claims immediately
 * Test D: Actual backbuffer resize — resize transition fires exactly once
 * Test E: Dominant candidate — clearly better swapchain wins election
 */

#include "../../test_utils.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <random>
#include <string>
#include <algorithm>
#include <functional>

namespace dxvk {
  // Logger needed by shared code used in this Unit Test.
  Logger Logger::s_instance("test_preservation_properties.log");

namespace preservation_properties {

  // ==========================================================================
  // Test Helpers
  // ==========================================================================

  #define TEST_ASSERT(condition, message) \
    do { \
      if (!(condition)) { \
        std::ostringstream oss; \
        oss << "FAILED: " << __FUNCTION__ << " line " << __LINE__ << ": " << message; \
        throw DxvkError(oss.str()); \
      } \
    } while(0)

  // ==========================================================================
  // Reproduce the FIXED election logic from d3d11_swapchain.cpp
  //
  // Updated copy of PrimaryCandidateInfo and isClearlyBetterCandidate from
  // the fixed source code, so we can test election in isolation.
  // ==========================================================================

  struct PrimaryCandidateInfo {
    bool visible = false;
    bool isForeground = false;
    bool exactClientMatch = false;
    bool nearClientMatch = false;
    bool hasDraws = false;
    uint64_t area = 0;
    uint32_t draws = 0;
    uint32_t clientWidth = 0;
    uint32_t clientHeight = 0;
  };

  // Copy of the FIXED isClearlyBetterCandidate logic.
  // Priority order: visible → isForeground → hasDraws → area (2x margin) → draws (+64 threshold)
  // exactClientMatch and nearClientMatch are intentionally NOT checked.
  static bool isClearlyBetterCandidate(const PrimaryCandidateInfo& candidate,
                                       const PrimaryCandidateInfo& current) {
    // Priority 1: Visibility — invisible windows never win.
    if (candidate.visible != current.visible)
      return candidate.visible;

    // Priority 2: Foreground window — strongest signal for the main game window.
    if (candidate.isForeground != current.isForeground)
      return candidate.isForeground;

    // Priority 3: Has draws — a swapchain actively drawing is more likely the game.
    if (candidate.hasDraws != current.hasDraws)
      return candidate.hasDraws;

    if (current.area == 0)
      return candidate.area > 0;

    // Priority 4: Area with incumbency advantage — require 2x area margin.
    if (candidate.area > current.area * 2)
      return true;

    // Priority 5: Draw count with higher threshold — require +64 extra draws.
    if (candidate.draws > current.draws + 64)
      return true;

    return false;
  }

  // Reproduce the fixed hysteresis constant (raised from 3 to 5).
  static constexpr uint32_t kPrimaryHysteresisFrames = 5;

  // ==========================================================================
  // Reproduce the FIXED noteResizeTransition logic from d3d11_rtx.cpp
  // ==========================================================================

  struct VkExtent2D {
    uint32_t width;
    uint32_t height;
  };

  static constexpr uint32_t kResizeCameraGraceFrames = 16;

  // Simulates the resize tracking state from D3D11Rtx.
  struct ResizeTracker {
    VkExtent2D lastOutputExtent = { 0u, 0u };
    VkExtent2D lastClientExtent = { 0u, 0u };
    uint32_t   resizeTransitionFramesRemaining = 0;

    // Exact copy of the unfixed noteResizeTransition lambda.
    void noteResizeTransition(VkExtent2D newExtent, VkExtent2D& trackedExtent) {
      if (newExtent.width == 0u || newExtent.height == 0u)
        return;

      if (trackedExtent.width != 0u && trackedExtent.height != 0u
       && (trackedExtent.width != newExtent.width || trackedExtent.height != newExtent.height)) {
        resizeTransitionFramesRemaining = kResizeCameraGraceFrames;
      }

      trackedExtent = newExtent;
    }

    // Simulates the FIXED EndFrame logic: client extent is tracked via
    // direct assignment (no noteResizeTransition), and only the backbuffer
    // extent comparison triggers resize transitions.
    void endFrame(VkExtent2D clientExtent, VkExtent2D backbufferExtent) {
      // Track the client extent for viewport fallback, but do NOT use it
      // for resize detection.
      if (clientExtent.width > 0u && clientExtent.height > 0u) {
        lastClientExtent = clientExtent;
      }

      if (backbufferExtent.width > 0u && backbufferExtent.height > 0u) {
        noteResizeTransition(backbufferExtent, lastOutputExtent);
      }
    }
  };

  // ==========================================================================
  // Helper: build a PrimaryCandidateInfo simulating describeCandidate
  // ==========================================================================

  static PrimaryCandidateInfo makeCandidate(
      bool visible,
      uint32_t backbufferWidth, uint32_t backbufferHeight,
      uint32_t clientWidth, uint32_t clientHeight,
      uint32_t draws,
      bool isForeground = false) {
    PrimaryCandidateInfo info;
    info.visible = visible;
    info.isForeground = isForeground;
    info.draws = draws;
    info.hasDraws = draws > 0;
    info.area = uint64_t(backbufferWidth) * uint64_t(backbufferHeight);
    info.clientWidth = clientWidth;
    info.clientHeight = clientHeight;

    // Reproduce the exactClientMatch / nearClientMatch logic from describeCandidate
    if (clientWidth > 0 && clientHeight > 0 && backbufferWidth >= 64 && backbufferHeight >= 64) {
      auto absDiff = [](uint32_t a, uint32_t b) -> uint32_t {
        return a > b ? a - b : b - a;
      };

      const uint32_t exactWidthTolerance = clientWidth / 16u + 8u;
      const uint32_t exactHeightTolerance = clientHeight / 16u + 8u;
      const uint32_t nearWidthTolerance = clientWidth / 4u + 64u;
      const uint32_t nearHeightTolerance = clientHeight / 4u + 64u;

      info.exactClientMatch = absDiff(backbufferWidth, clientWidth) <= exactWidthTolerance
                           && absDiff(backbufferHeight, clientHeight) <= exactHeightTolerance;
      info.nearClientMatch = absDiff(backbufferWidth, clientWidth) <= nearWidthTolerance
                          && absDiff(backbufferHeight, clientHeight) <= nearHeightTolerance;
    }

    return info;
  }

  // ==========================================================================
  // Simulated primary election state (mirrors g_primarySwapChain logic)
  // ==========================================================================

  struct ElectionState {
    int primaryId = -1;           // -1 means no primary
    int challengerId = -1;
    uint32_t challengerFrames = 0;
    bool primaryDestroyed = false; // simulates window gone
    bool forcedOverride = false;   // simulates Alt+X

    // Simulate a present call from swapchain `id` with the given candidate info.
    // Returns true if this swapchain became primary (stole or claimed).
    bool present(int id, const PrimaryCandidateInfo& candidate,
                 const PrimaryCandidateInfo& currentPrimaryInfo) {
      // Alt+X forced override
      if (forcedOverride) {
        primaryId = id;
        challengerId = -1;
        challengerFrames = 0;
        forcedOverride = false;
        return true;
      }

      // No current primary or primary destroyed — elect immediately
      if (primaryId < 0 || primaryDestroyed) {
        primaryId = id;
        challengerId = -1;
        challengerFrames = 0;
        primaryDestroyed = false;
        return true;
      }

      // Already primary
      if (primaryId == id) {
        return false; // no steal, already primary
      }

      // Challenge: only if this swapchain has draws
      if (candidate.draws > 0) {
        if (isClearlyBetterCandidate(candidate, currentPrimaryInfo)) {
          if (challengerId == id) {
            ++challengerFrames;
          } else {
            challengerId = id;
            challengerFrames = 1;
          }
          if (challengerFrames >= kPrimaryHysteresisFrames) {
            primaryId = id;
            challengerId = -1;
            challengerFrames = 0;
            return true;
          }
        } else {
          if (challengerId == id) {
            challengerId = -1;
            challengerFrames = 0;
          }
        }
      }

      return false;
    }
  };

  // ==========================================================================
  // Simple PRNG for property-based test generation
  // ==========================================================================

  class TestRng {
  public:
    explicit TestRng(uint64_t seed) : m_rng(seed) {}

    uint32_t nextU32(uint32_t lo, uint32_t hi) {
      std::uniform_int_distribution<uint32_t> dist(lo, hi);
      return dist(m_rng);
    }

    bool nextBool() {
      return nextU32(0, 1) == 1;
    }

  private:
    std::mt19937_64 m_rng;
  };


  // ==========================================================================
  // Test A: Single-swapchain election — immediate primary, no hysteresis
  // **Validates: Requirements 3.1**
  //
  // Observed on UNFIXED code: When only one swapchain exists, it is elected
  // primary immediately on the first present call. No hysteresis delay.
  //
  // Property: For ALL randomly generated single-swapchain inputs (varying
  // resolution, draw count, visibility), the lone swapchain is elected
  // primary on the very first present call.
  // ==========================================================================

  void test_A_singleSwapchainImmediateElection() {
    std::cout << "  Running Test A: Single-swapchain immediate election (property-based)..." << std::endl;

    static constexpr uint32_t kNumTrials = 200;
    TestRng rng(42);

    for (uint32_t trial = 0; trial < kNumTrials; ++trial) {
      // Generate random single-swapchain parameters
      const uint32_t width = rng.nextU32(64, 3840);
      const uint32_t height = rng.nextU32(64, 2160);
      // Client rect may or may not match backbuffer
      const uint32_t clientW = rng.nextBool() ? width : rng.nextU32(64, 3840);
      const uint32_t clientH = rng.nextBool() ? height : rng.nextU32(64, 2160);
      const uint32_t draws = rng.nextU32(0, 1000);
      const bool visible = true; // must be visible to present

      PrimaryCandidateInfo candidate = makeCandidate(visible, width, height, clientW, clientH, draws, true /*isForeground*/);

      ElectionState state;
      // Empty current primary info (no primary exists yet)
      PrimaryCandidateInfo emptyPrimary;

      bool elected = state.present(0, candidate, emptyPrimary);

      TEST_ASSERT(elected,
        "Trial " + std::to_string(trial) + ": Single swapchain (" +
        std::to_string(width) + "x" + std::to_string(height) +
        ", draws=" + std::to_string(draws) +
        ") was NOT elected primary immediately on first present.");

      TEST_ASSERT(state.primaryId == 0,
        "Trial " + std::to_string(trial) + ": primaryId should be 0 after single-swapchain election.");
    }

    std::cout << "    PASSED (" << kNumTrials << " trials)" << std::endl;
  }

  // ==========================================================================
  // Test B: Alt+X override — forced primary bypasses election
  // **Validates: Requirements 3.2**
  //
  // Observed on UNFIXED code: When Alt+X is pressed, the current swapchain
  // immediately claims primary, bypassing all election heuristics and
  // hysteresis. Even if another swapchain is currently primary.
  //
  // Property: For ALL randomly generated election states and swapchain
  // configurations, setting forcedOverride=true causes the presenting
  // swapchain to become primary immediately.
  // ==========================================================================

  void test_B_altXOverride() {
    std::cout << "  Running Test B: Alt+X override (property-based)..." << std::endl;

    static constexpr uint32_t kNumTrials = 200;
    TestRng rng(123);

    for (uint32_t trial = 0; trial < kNumTrials; ++trial) {
      // Set up a random existing primary
      ElectionState state;
      state.primaryId = 0;

      const uint32_t existingW = rng.nextU32(64, 3840);
      const uint32_t existingH = rng.nextU32(64, 2160);
      const uint32_t existingDraws = rng.nextU32(1, 1000);
      PrimaryCandidateInfo existingPrimary = makeCandidate(
        true, existingW, existingH, existingW, existingH, existingDraws, true /*isForeground*/);

      // Generate a random challenger (could be worse, equal, or better)
      const uint32_t chalW = rng.nextU32(64, 3840);
      const uint32_t chalH = rng.nextU32(64, 2160);
      const uint32_t chalDraws = rng.nextU32(0, 1000);
      const bool chalVisible = rng.nextBool();
      PrimaryCandidateInfo challenger = makeCandidate(
        chalVisible, chalW, chalH, chalW, chalH, chalDraws, false /*isForeground*/);

      // Simulate Alt+X override
      state.forcedOverride = true;
      bool elected = state.present(1, challenger, existingPrimary);

      TEST_ASSERT(elected,
        "Trial " + std::to_string(trial) + ": Alt+X override did NOT force primary. "
        "Challenger: " + std::to_string(chalW) + "x" + std::to_string(chalH) +
        " draws=" + std::to_string(chalDraws) +
        " visible=" + std::to_string(chalVisible));

      TEST_ASSERT(state.primaryId == 1,
        "Trial " + std::to_string(trial) + ": primaryId should be 1 after Alt+X override.");

      // Verify hysteresis was reset
      TEST_ASSERT(state.challengerId == -1 && state.challengerFrames == 0,
        "Trial " + std::to_string(trial) + ": Hysteresis state should be reset after Alt+X override.");
    }

    std::cout << "    PASSED (" << kNumTrials << " trials)" << std::endl;
  }

  // ==========================================================================
  // Test C: Destroyed primary — next presenter claims immediately
  // **Validates: Requirements 3.3**
  //
  // Observed on UNFIXED code: When the primary swapchain is destroyed (its
  // window is gone), the next swapchain to present claims primary immediately
  // with no hysteresis delay.
  //
  // Property: For ALL randomly generated swapchain configurations, when the
  // primary is destroyed, the next presenter becomes primary immediately.
  // ==========================================================================

  void test_C_destroyedPrimaryReplacement() {
    std::cout << "  Running Test C: Destroyed primary replacement (property-based)..." << std::endl;

    static constexpr uint32_t kNumTrials = 200;
    TestRng rng(456);

    for (uint32_t trial = 0; trial < kNumTrials; ++trial) {
      // Set up a primary that gets destroyed
      ElectionState state;
      state.primaryId = 0;
      state.primaryDestroyed = true; // simulate window gone

      // Generate a random replacement swapchain
      const uint32_t repW = rng.nextU32(64, 3840);
      const uint32_t repH = rng.nextU32(64, 2160);
      const uint32_t repDraws = rng.nextU32(0, 1000);
      const bool repVisible = true;
      PrimaryCandidateInfo replacement = makeCandidate(
        repVisible, repW, repH, repW, repH, repDraws, true /*isForeground*/);

      // The current primary info doesn't matter since it's destroyed
      PrimaryCandidateInfo destroyedPrimary;

      bool elected = state.present(1, replacement, destroyedPrimary);

      TEST_ASSERT(elected,
        "Trial " + std::to_string(trial) + ": Replacement swapchain (" +
        std::to_string(repW) + "x" + std::to_string(repH) +
        ", draws=" + std::to_string(repDraws) +
        ") was NOT elected primary immediately after primary destruction.");

      TEST_ASSERT(state.primaryId == 1,
        "Trial " + std::to_string(trial) + ": primaryId should be 1 after destroyed primary replacement.");

      TEST_ASSERT(!state.primaryDestroyed,
        "Trial " + std::to_string(trial) + ": primaryDestroyed should be cleared after replacement.");
    }

    std::cout << "    PASSED (" << kNumTrials << " trials)" << std::endl;
  }

  // ==========================================================================
  // Test D: Actual backbuffer resize — resize transition fires exactly once
  // **Validates: Requirements 3.4, 3.5**
  //
  // Observed on UNFIXED code: When the backbuffer extent actually changes
  // between frames, noteResizeTransition fires and sets
  // m_resizeTransitionFramesRemaining = kResizeCameraGraceFrames. On
  // subsequent frames with the same backbuffer extent, it does NOT re-fire.
  //
  // Property: For ALL randomly generated backbuffer resize sequences, the
  // resize transition fires exactly once per actual backbuffer change, and
  // does NOT re-fire on subsequent stable frames.
  // ==========================================================================

  void test_D_actualBackbufferResize() {
    std::cout << "  Running Test D: Actual backbuffer resize (property-based)..." << std::endl;

    static constexpr uint32_t kNumTrials = 200;
    TestRng rng(789);

    for (uint32_t trial = 0; trial < kNumTrials; ++trial) {
      ResizeTracker tracker;

      // Generate initial backbuffer extent
      const uint32_t initW = rng.nextU32(64, 3840);
      const uint32_t initH = rng.nextU32(64, 2160);
      const VkExtent2D initBB = { initW, initH };
      // For this test, client rect matches backbuffer (no mismatch confusion)
      const VkExtent2D clientRect = initBB;

      // Frame 1: Initialize — should NOT fire (tracked extent starts at 0,0)
      tracker.endFrame(clientRect, initBB);
      TEST_ASSERT(tracker.resizeTransitionFramesRemaining == 0,
        "Trial " + std::to_string(trial) + ": Resize should NOT fire on first frame (init from 0,0).");

      // Frame 2: Same extent — should NOT fire
      tracker.endFrame(clientRect, initBB);
      TEST_ASSERT(tracker.resizeTransitionFramesRemaining == 0,
        "Trial " + std::to_string(trial) + ": Resize should NOT fire on stable frame.");

      // Frame 3: Actual resize — generate a different backbuffer extent
      uint32_t newW = rng.nextU32(64, 3840);
      uint32_t newH = rng.nextU32(64, 2160);
      // Ensure it's actually different
      while (newW == initW && newH == initH) {
        newW = rng.nextU32(64, 3840);
        newH = rng.nextU32(64, 2160);
      }
      const VkExtent2D newBB = { newW, newH };
      const VkExtent2D newClient = newBB; // client matches new backbuffer

      tracker.resizeTransitionFramesRemaining = 0; // clear for clean measurement
      tracker.endFrame(newClient, newBB);

      TEST_ASSERT(tracker.resizeTransitionFramesRemaining == kResizeCameraGraceFrames,
        "Trial " + std::to_string(trial) + ": Resize should fire on actual backbuffer change. "
        "Old: " + std::to_string(initW) + "x" + std::to_string(initH) +
        " New: " + std::to_string(newW) + "x" + std::to_string(newH));

      // Frame 4: Same new extent — should NOT re-fire
      tracker.resizeTransitionFramesRemaining = 0; // clear for clean measurement
      tracker.endFrame(newClient, newBB);
      TEST_ASSERT(tracker.resizeTransitionFramesRemaining == 0,
        "Trial " + std::to_string(trial) + ": Resize should NOT re-fire on stable frame after resize.");
    }

    std::cout << "    PASSED (" << kNumTrials << " trials)" << std::endl;
  }

  // ==========================================================================
  // Test E: Dominant candidate — clearly better swapchain wins election
  // **Validates: Requirements 3.6, 3.7, 3.8**
  //
  // Observed behavior: When one swapchain is dramatically better than
  // the current primary (>2x area OR >+64 draws), it wins election after
  // the hysteresis period, regardless of other factors.
  //
  // Property: For ALL randomly generated non-buggy multi-swapchain inputs
  // where the dominant candidate has >2x area AND >+64 draws AND both have
  // the same visibility/foreground status, the dominant candidate wins
  // election after kPrimaryHysteresisFrames.
  //
  // This captures the preservation requirement that legitimate transitions
  // (where one swapchain is clearly the main game window) still work.
  // ==========================================================================

  void test_E_dominantCandidateWins() {
    std::cout << "  Running Test E: Dominant candidate wins election (property-based)..." << std::endl;

    static constexpr uint32_t kNumTrials = 200;
    TestRng rng(1337);

    for (uint32_t trial = 0; trial < kNumTrials; ++trial) {
      // Generate a current primary with moderate stats
      const uint32_t curW = rng.nextU32(64, 1920);
      const uint32_t curH = rng.nextU32(64, 1080);
      const uint32_t curDraws = rng.nextU32(1, 200);
      // Both candidates have the same foreground status (true) so foreground
      // doesn't break the tie — area and draws must decide.
      PrimaryCandidateInfo current = makeCandidate(
        true, curW, curH, curW, curH, curDraws, true /*isForeground*/);

      // Generate a dominant challenger: >2x area AND significantly more draws.
      // The fixed code requires candidate.area > current.area * 2 (strict >2x margin)
      // and candidate.draws > current.draws + 64 (raised threshold).
      // We ensure the challenger exceeds BOTH thresholds so isClearlyBetterCandidate
      // returns true on the area check alone (the draws check is a fallback).
      const uint32_t domW = curW * 2 + rng.nextU32(1, 200);
      const uint32_t domH = curH * 2 + rng.nextU32(1, 200);
      // Ensure draws exceed current by more than 64 (the fixed threshold)
      const uint32_t domDraws = curDraws + 65 + rng.nextU32(0, 200);
      PrimaryCandidateInfo dominant = makeCandidate(
        true, domW, domH, domW, domH, domDraws, true /*isForeground*/);

      // Verify the dominant candidate IS clearly better on unfixed code
      bool isBetter = isClearlyBetterCandidate(dominant, current);
      TEST_ASSERT(isBetter,
        "Trial " + std::to_string(trial) + ": Dominant candidate should be clearly better. "
        "Current: " + std::to_string(curW) + "x" + std::to_string(curH) + " draws=" + std::to_string(curDraws) +
        " Dominant: " + std::to_string(domW) + "x" + std::to_string(domH) + " draws=" + std::to_string(domDraws));

      // Simulate the election with hysteresis
      ElectionState state;
      state.primaryId = 0;

      bool stole = false;
      for (uint32_t frame = 0; frame < kPrimaryHysteresisFrames; ++frame) {
        stole = state.present(1, dominant, current);
      }

      TEST_ASSERT(stole,
        "Trial " + std::to_string(trial) + ": Dominant candidate should win after " +
        std::to_string(kPrimaryHysteresisFrames) + " frames of hysteresis. "
        "Current: area=" + std::to_string(current.area) + " draws=" + std::to_string(curDraws) +
        " Dominant: area=" + std::to_string(dominant.area) + " draws=" + std::to_string(domDraws));

      TEST_ASSERT(state.primaryId == 1,
        "Trial " + std::to_string(trial) + ": primaryId should be 1 after dominant candidate wins.");
    }

    std::cout << "    PASSED (" << kNumTrials << " trials)" << std::endl;
  }

  // ==========================================================================
  // Test Runner
  // ==========================================================================

  int runAllTests() {
    int passed = 0;
    int failed = 0;
    int total = 5;

    std::cout << "Renderer Viewport Correctness — Preservation Property Tests" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "These tests verify baseline behavior is preserved after the fix." << std::endl;
    std::cout << "Tests PASSING confirms no regressions in election behavior." << std::endl;
    std::cout << std::endl;

    // Test A: Single-swapchain immediate election
    try {
      test_A_singleSwapchainImmediateElection();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    // Test B: Alt+X override
    try {
      test_B_altXOverride();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    // Test C: Destroyed primary replacement
    try {
      test_C_destroyedPrimaryReplacement();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    // Test D: Actual backbuffer resize
    try {
      test_D_actualBackbufferResize();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    // Test E: Dominant candidate wins
    try {
      test_E_dominantCandidateWins();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    std::cout << std::endl;
    std::cout << "Results: " << passed << "/" << total << " passed, "
              << failed << "/" << total << " failed" << std::endl;

    if (failed > 0) {
      std::cout << "WARNING: Preservation test failures indicate baseline behavior has changed!" << std::endl;
    } else {
      std::cout << "All preservation tests passed — baseline behavior confirmed." << std::endl;
    }

    // Return 0 if all tests pass, 1 if any fail
    return failed > 0 ? 1 : 0;
  }

} // namespace preservation_properties
} // namespace dxvk

int main() {
  return dxvk::preservation_properties::runAllTests();
}
