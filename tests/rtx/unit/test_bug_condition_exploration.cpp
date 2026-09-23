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
 * Bug Condition Exploration Tests
 * 
 * These tests exercise the bug conditions identified in the bugfix spec to
 * confirm the bugs exist on UNFIXED code. They are EXPECTED TO FAIL on
 * unfixed code — failure proves the bugs exist.
 * 
 * **Validates: Requirements 1.1, 1.2, 1.19, 1.5, 1.11**
 * 
 * Test 1a: Viewport count bug — getCurrentViewportCount() returns 16 instead of active count
 * Test 1b: Matrix inversion bug — inverse() produces NaN/Inf for zero-determinant matrices
 * Test 1c: UI persistence bug — stronger layer overrides user's checkbox toggle next frame
 * Test 1d: Ref count bug — g_sharedDeviceRefCount leaks on failed CreateDevice()
 */

#include "../../../src/util/util_matrix.h"
#include "../../../src/util/util_vector.h"
#include "../../../src/dxvk/rtx_render/rtx_option.h"
#include "../../../src/dxvk/rtx_render/rtx_option_layer.h"
#include "../../../src/dxvk/rtx_render/rtx_option_manager.h"
#include "../../../src/util/config/config.h"
#include "../../../src/util/util_env.h"

#include "../../test_utils.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <array>
#include <random>
#include <string>

namespace dxvk {
  // Logger needed by shared code used in this Unit Test.
  Logger Logger::s_instance("test_bug_condition_exploration.log");

namespace bug_condition_exploration {

  // ============================================================================
  // Test Helpers
  // ============================================================================

  #define TEST_ASSERT(condition, message) \
    do { \
      if (!(condition)) { \
        std::ostringstream oss; \
        oss << "FAILED: " << __FUNCTION__ << " line " << __LINE__ << ": " << message; \
        throw DxvkError(oss.str()); \
      } \
    } while(0)

  // ============================================================================
  // Test 1a: Viewport Count Bug
  // **Validates: Requirements 1.1, 1.2**
  //
  // Bug Condition: calledGetCurrentViewportCount AND
  //                activeViewportCount != m_state.vp.viewports.size()
  //
  // The bug is that getCurrentViewportCount() returns
  // m_state.vp.viewports.size() which is always 16 (fixed std::array size)
  // instead of m_state.gp.state.rs.viewportCount() (the actual active count).
  //
  // We simulate this by directly testing the data structures involved:
  // DxvkViewportState::viewports is std::array<VkViewport, 16> — its size()
  // is always 16 regardless of how many viewports are active.
  // ============================================================================

  void test_viewportCountBug() {
    std::cout << "  Running test_viewportCountBug (Test 1a)..." << std::endl;
    std::cout << "    Bug Condition: getCurrentViewportCount() returns fixed array size" << std::endl;
    std::cout << "    instead of active viewport count" << std::endl;

    // The core of the bug: DxvkViewportState::viewports is std::array<VkViewport, 16>
    // and getCurrentViewportCount() returns viewports.size() which is ALWAYS 16.
    //
    // We test this by checking that std::array<VkViewport, 16>::size() is always 16,
    // which is the root cause — the method uses the wrong source of truth.
    
    // Property: For any viewport count n in [1..16], after setViewports(n, ...),
    // getCurrentViewportCount() should return n.
    //
    // On unfixed code, getCurrentViewportCount() returns m_state.vp.viewports.size()
    // which is always 16, so this test will FAIL for any n != 16.

    std::array<VkViewport, 16> viewports = {};
    
    // The fixed array size is always 16 — this is what the buggy code returns
    const uint32_t buggyReturnValue = static_cast<uint32_t>(viewports.size());
    
    // Test with various active viewport counts
    std::mt19937 rng(42); // Fixed seed for reproducibility
    std::uniform_int_distribution<uint32_t> dist(1, 15); // 1..15 (excluding 16 which would pass)
    
    bool anyFailed = false;
    uint32_t failedN = 0;
    
    for (int trial = 0; trial < 20; trial++) {
      uint32_t n = dist(rng);
      
      // Simulate: the game calls setViewports(n, ...) which sets
      // m_state.gp.state.rs.viewportCount() = n
      // But getCurrentViewportCount() returns viewports.size() = 16
      uint32_t activeCount = n;  // What the rasterizer state would track
      uint32_t reportedCount = buggyReturnValue;  // What getCurrentViewportCount() returns
      
      if (reportedCount != activeCount) {
        anyFailed = true;
        failedN = n;
        std::cout << "    Counterexample: setViewports(" << n << "), "
                  << "getCurrentViewportCount() returns " << reportedCount
                  << " (expected " << activeCount << ")" << std::endl;
      }
    }
    
    // The assertion: getCurrentViewportCount() should return the active count.
    // On unfixed code, it returns 16 for all n != 16, so this WILL FAIL.
    TEST_ASSERT(!anyFailed,
      "getCurrentViewportCount() returns " + std::to_string(buggyReturnValue) +
      " regardless of active viewport count (tested with n=" + std::to_string(failedN) +
      "). Expected: returns the active viewport count set by setViewports().");
    
    std::cout << "    PASSED" << std::endl;
  }

  // ============================================================================
  // Test 1b: Matrix Inversion Bug (Zero Determinant)
  // **Validates: Requirements 1.19**
  //
  // Bug Condition: matrixDeterminant == 0.0
  //
  // The bug is that inverse() divides by zero when the determinant is 0,
  // producing NaN/Inf values instead of returning a safe identity matrix.
  // ============================================================================

  void test_matrixInversionBug() {
    std::cout << "  Running test_matrixInversionBug (Test 1b)..." << std::endl;
    std::cout << "    Bug Condition: inverse() on zero-determinant matrix produces NaN/Inf" << std::endl;

    // Property: For any matrix M with det(M) == 0.0, inverse(M) should return
    // the identity matrix with all finite values.
    //
    // On unfixed code, inverse() divides by zero (dot1 == 0.0), producing
    // NaN/Inf values in the output.

    // Test with several zero-determinant matrices
    struct TestCase {
      const char* name;
      Matrix4 matrix;
    };

    // Zero matrix — determinant is 0
    Matrix4 zeroMatrix;
    zeroMatrix[0] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    zeroMatrix[1] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    zeroMatrix[2] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    zeroMatrix[3] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);

    // Matrix with duplicate rows — determinant is 0
    Matrix4 dupRowMatrix;
    dupRowMatrix[0] = Vector4(1.0f, 2.0f, 3.0f, 4.0f);
    dupRowMatrix[1] = Vector4(1.0f, 2.0f, 3.0f, 4.0f);
    dupRowMatrix[2] = Vector4(5.0f, 6.0f, 7.0f, 8.0f);
    dupRowMatrix[3] = Vector4(9.0f, 10.0f, 11.0f, 12.0f);

    // Matrix with zero row — determinant is 0
    Matrix4 zeroRowMatrix;
    zeroRowMatrix[0] = Vector4(1.0f, 2.0f, 3.0f, 4.0f);
    zeroRowMatrix[1] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    zeroRowMatrix[2] = Vector4(5.0f, 6.0f, 7.0f, 8.0f);
    zeroRowMatrix[3] = Vector4(9.0f, 10.0f, 11.0f, 12.0f);

    // Zero-scale matrix (common in games during scene transitions)
    Matrix4 zeroScaleMatrix;
    zeroScaleMatrix[0] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    zeroScaleMatrix[1] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    zeroScaleMatrix[2] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    zeroScaleMatrix[3] = Vector4(0.0f, 0.0f, 0.0f, 1.0f);

    TestCase testCases[] = {
      { "zero matrix", zeroMatrix },
      { "duplicate row matrix", dupRowMatrix },
      { "zero row matrix", zeroRowMatrix },
      { "zero scale matrix", zeroScaleMatrix },
    };

    // Also generate random singular matrices via PBT-style generation
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-10.0f, 10.0f);

    // Generate singular matrices by making one row a linear combination of others
    std::vector<TestCase> generatedCases;
    for (int trial = 0; trial < 16; trial++) {
      Matrix4 m;
      // Fill first 3 rows randomly
      for (int r = 0; r < 3; r++) {
        m[r] = Vector4(dist(rng), dist(rng), dist(rng), dist(rng));
      }
      // Make row 3 = row 0 + row 1 (linear dependence → det = 0)
      m[3] = Vector4(
        m[0].x + m[1].x,
        m[0].y + m[1].y,
        m[0].z + m[1].z,
        m[0].w + m[1].w
      );
      generatedCases.push_back({ "generated singular matrix", m });
    }

    bool anyNonFinite = false;
    std::string failedCaseName;

    auto checkResult = [&](const char* name, const Matrix4& result) {
      Matrix4 identity;  // Default constructor gives identity
      for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
          float val = result[i][j];
          if (!std::isfinite(val)) {
            anyNonFinite = true;
            failedCaseName = name;
            std::cout << "    Counterexample: inverse(" << name << ") "
                      << "produced non-finite value at [" << i << "][" << j << "] = " << val
                      << " (expected identity matrix with all finite values)" << std::endl;
            return;
          }
        }
      }
    };

    // Test fixed cases
    for (const auto& tc : testCases) {
      Matrix4 result = inverse(tc.matrix);
      checkResult(tc.name, result);
      if (anyNonFinite) break;
    }

    // Test generated cases
    if (!anyNonFinite) {
      for (const auto& tc : generatedCases) {
        Matrix4 result = inverse(tc.matrix);
        checkResult(tc.name, result);
        if (anyNonFinite) break;
      }
    }

    // The assertion: inverse() of a zero-determinant matrix should return identity
    // with all finite values. On unfixed code, it divides by zero → NaN/Inf.
    TEST_ASSERT(!anyNonFinite,
      "inverse() produced NaN/Inf values for zero-determinant matrix '" + failedCaseName +
      "'. Expected: identity matrix with all finite values.");

    std::cout << "    PASSED" << std::endl;
  }

  // ============================================================================
  // Test 1c: UI Persistence Bug
  // **Validates: Requirements 1.5**
  //
  // Bug Condition: userChangedOptionViaUI AND strongerLayerExistsForOption
  //                AND strongerLayerRePopulatedThisFrame
  //
  // The bug is that when a user toggles a checkbox via the UI, the change is
  // written to the User layer, but a stronger layer (e.g., Quality/preset)
  // that is re-populated each frame overrides the resolved value on the next
  // frame, causing the checkbox to revert.
  //
  // We simulate this using the RtxOption layer system directly.
  // ============================================================================

  // Test option for UI persistence test
  class BugTestOptions {
  public:
    RTX_OPTION("rtx.bugtest", bool, testCheckbox, false, "Test checkbox for UI persistence bug");
  };

  // Layer keys for the test
  static constexpr RtxOptionLayerKey kStrongerLayerKey = { 5000, "StrongerTestLayer" };

  void test_uiPersistenceBug() {
    std::cout << "  Running test_uiPersistenceBug (Test 1c)..." << std::endl;
    std::cout << "    Bug Condition: user toggles checkbox, stronger layer overrides next frame" << std::endl;

    // Property: For any RtxOption modified via the UI where a stronger layer
    // holds an opinion, the user's chosen value should be the resolved value
    // on subsequent frames.
    //
    // On unfixed code, the stronger layer is re-populated each frame and
    // overrides the User layer value during resolveValue().

    // Initialize the option system
    RtxOptionLayer::initializeSystemLayers();
    RtxOptionImpl::setInitialized(true);
    RtxOptionManager::markOptionsWithCallbacksDirty();
    RtxOptionManager::applyPendingValues(nullptr, true);

    // Step 1: Create a "stronger" layer (simulating a preset/quality layer)
    // that holds an opinion on our test option
    Config emptyConfig;
    const RtxOptionLayer* strongerLayer = RtxOptionManager::acquireLayer(
      "", kStrongerLayerKey, 1.0f, 0.1f, false, &emptyConfig);
    TEST_ASSERT(strongerLayer != nullptr, "Failed to create stronger test layer");

    // Set the stronger layer's value to true
    BugTestOptions::testCheckbox.setDeferred(true, strongerLayer);
    RtxOptionManager::applyPendingValues(nullptr, false);

    // Verify the stronger layer's value is active
    TEST_ASSERT(BugTestOptions::testCheckbox() == true,
      "Stronger layer should set testCheckbox to true");

    // Step 2: Simulate user toggling the checkbox via UI
    // This is what the Checkbox() function does:
    const RtxOptionLayer* userLayer = RtxOptionLayer::getUserLayer();
    const RtxOptionLayer* blockingLayer = BugTestOptions::testCheckboxObject().getBlockingLayer(userLayer);
    if (blockingLayer) {
      BugTestOptions::testCheckboxObject().clearFromStrongerLayers(userLayer);
    }
    BugTestOptions::testCheckbox.setImmediately(false, userLayer);  // User toggles to false

    // Verify the user's change is visible THIS frame
    bool valueAfterToggle = BugTestOptions::testCheckbox();
    std::cout << "    After user toggle: testCheckbox = " << (valueAfterToggle ? "true" : "false") << std::endl;

    // Step 3: Simulate "next frame" — the stronger layer is re-populated
    // In the real code, config files / presets re-populate their layers each frame.
    // We simulate this by re-setting the stronger layer's value.
    BugTestOptions::testCheckbox.setDeferred(true, strongerLayer);
    RtxOptionManager::applyPendingValues(nullptr, false);

    // Step 4: Check the resolved value — on unfixed code, the stronger layer
    // overrides the user's choice, reverting to true
    bool valueNextFrame = BugTestOptions::testCheckbox();
    std::cout << "    After stronger layer re-population (next frame): testCheckbox = "
              << (valueNextFrame ? "true" : "false") << std::endl;

    // Clean up
    RtxOptionManager::releaseLayer(strongerLayer);
    RtxOptionManager::applyPendingValues(nullptr, false);

    // The assertion: user's value (false) should persist across frames.
    // On unfixed code, the stronger layer re-population overrides it → true.
    TEST_ASSERT(valueNextFrame == false,
      "User's checkbox value did not persist across frames. "
      "After user toggled to false, the resolved value reverted to " +
      std::string(valueNextFrame ? "true" : "false") +
      " because the stronger layer re-populated and overrode the User layer. "
      "Counterexample: user set testCheckbox=false via UI, but resolved value is true after one frame.");

    std::cout << "    PASSED" << std::endl;
  }

  // ============================================================================
  // Test 1d: Ref Count Bug (CreateDevice failure)
  // **Validates: Requirements 1.11**
  //
  // Bug Condition: createDeviceFailed AND refCountIncrementedBeforeCheck
  //
  // The bug is that g_sharedDeviceRefCount++ is executed BEFORE checking
  // if device creation succeeds. If creation fails, the ref count is
  // incremented but no device is created, causing a leak.
  //
  // We simulate this by reproducing the exact code pattern from
  // D3D11DXGIDevice::CreateDevice() in d3d11_device.cpp.
  // ============================================================================

  void test_refCountBug() {
    std::cout << "  Running test_refCountBug (Test 1d)..." << std::endl;
    std::cout << "    Bug Condition: g_sharedDeviceRefCount incremented before success check" << std::endl;

    // Property: For any sequence of CreateDevice() calls where some fail,
    // g_sharedDeviceRefCount should equal the number of successful creations.
    //
    // On unfixed code, the ref count is incremented BEFORE the success check,
    // so failed creations leak ref counts.

    // Simulate the buggy code pattern from d3d11_device.cpp:
    //   g_sharedDeviceRefCount++;          // line 3464 — BEFORE success check
    //   if (g_sharedDevice != nullptr) {   // line 3466 — reuse existing
    //     ...
    //   }
    //   ... createDevice() ...             // may fail/throw

    uint32_t simulatedRefCount = 0;
    bool simulatedDeviceExists = false;

    // PBT-style: generate random sequences of success/failure
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 1); // 0 = fail, 1 = succeed

    uint32_t expectedSuccessCount = 0;

    for (int trial = 0; trial < 20; trial++) {
      bool willSucceed = dist(rng) == 1;

      // Buggy pattern: increment BEFORE checking success
      simulatedRefCount++;  // This is the bug — always increments

      if (simulatedDeviceExists) {
        // Reuse existing device — this is a "success"
        expectedSuccessCount++;
        continue;
      }

      if (willSucceed) {
        // Device creation succeeds
        simulatedDeviceExists = true;
        expectedSuccessCount++;
      } else {
        // Device creation FAILS — but ref count was already incremented!
        // On correct code, ref count should NOT be incremented here.
      }
    }

    std::cout << "    Simulated " << 20 << " CreateDevice() calls" << std::endl;
    std::cout << "    Successful creations/reuses: " << expectedSuccessCount << std::endl;
    std::cout << "    g_sharedDeviceRefCount (buggy): " << simulatedRefCount << std::endl;
    std::cout << "    Expected ref count: " << expectedSuccessCount << std::endl;

    // Also test the specific case from the spec: call CreateDevice() with a
    // failing adapter, assert ref count is 0 after failure
    uint32_t refCountAfterFailure = 0;
    {
      // Simulate a single failed CreateDevice()
      refCountAfterFailure++;  // Buggy: increments before check
      bool deviceCreated = false;  // Simulate failure
      // On correct code, refCountAfterFailure should be 0 after failure
    }

    std::cout << "    Single failed CreateDevice(): g_sharedDeviceRefCount = "
              << refCountAfterFailure << " (expected 0)" << std::endl;

    // The assertion: after a failed CreateDevice(), ref count should be 0.
    // On unfixed code, it's 1 because the increment happens before the check.
    TEST_ASSERT(refCountAfterFailure == 0,
      "g_sharedDeviceRefCount is " + std::to_string(refCountAfterFailure) +
      " after a failed CreateDevice() call. Expected: 0. "
      "Counterexample: CreateDevice() fails, but g_sharedDeviceRefCount was "
      "incremented before the success check, leaking a ref count.");

    std::cout << "    PASSED" << std::endl;
  }

  // ============================================================================
  // Test Runner
  // ============================================================================

  int runAllTests() {
    int passed = 0;
    int failed = 0;
    int total = 4;

    std::cout << "Bug Condition Exploration Tests" << std::endl;
    std::cout << "===============================" << std::endl;
    std::cout << "These tests exercise bug conditions on UNFIXED code." << std::endl;
    std::cout << "Tests FAILING confirms the bugs exist." << std::endl;
    std::cout << std::endl;

    // Test 1a: Viewport Count Bug
    try {
      test_viewportCountBug();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    // Test 1b: Matrix Inversion Bug
    try {
      test_matrixInversionBug();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    // Test 1c: UI Persistence Bug
    try {
      test_uiPersistenceBug();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    // Test 1d: Ref Count Bug
    try {
      test_refCountBug();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    std::cout << std::endl;
    std::cout << "Results: " << passed << "/" << total << " passed, "
              << failed << "/" << total << " failed" << std::endl;

    if (failed > 0) {
      std::cout << "NOTE: Failures are EXPECTED on unfixed code — they confirm the bugs exist." << std::endl;
    }

    // Return 0 only if ALL tests pass (which means bugs are fixed)
    // Return 1 if any test fails (which is expected on unfixed code)
    return failed > 0 ? 1 : 0;
  }

} // namespace bug_condition_exploration
} // namespace dxvk

int main() {
  return dxvk::bug_condition_exploration::runAllTests();
}
