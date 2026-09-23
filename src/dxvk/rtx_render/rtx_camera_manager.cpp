#include "../../util/util_kenshi_telemetry.h"
/*
* Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx_camera_manager.h"
#include "../../util/util_kenshi_origin.h"
#include "../../util/util_kenshi_camera_audit.h"

#include "dxvk_device.h"

#include <cmath>

namespace {
  constexpr float kFovToleranceRadians = 0.001f;
}

namespace dxvk {

  CameraManager::CameraManager(DxvkDevice* device) : CommonDeviceObject(device) {
    for (int i = 0; i < CameraType::Count; i++) {
      m_cameras[i].setCameraType(CameraType::Enum(i));
    }
  }

  bool CameraManager::isCameraValid(CameraType::Enum cameraType) const {
    assert(cameraType < CameraType::Enum::Count);
    return accessCamera(*this, cameraType).isValid(m_device->getCurrentFrameId());
  }

  void CameraManager::onFrameEnd() {
    m_lastSetCameraType = CameraType::Unknown;
    m_decompositionCache.clear();
  }

  CameraType::Enum CameraManager::processCameraData(const DrawCallState& input) {
    // If theres no real camera data here - bail
    if (isIdentityExact(input.getTransformData().viewToProjection)) {
      return input.testCategoryFlags(InstanceCategories::Sky) ? CameraType::Sky : CameraType::Unknown;
    }

    // DX11_V285_OFFSCREEN_CAMERA_GATE: draws into offscreen color targets
    // (water reflection, environment cubemap, mirror passes) carry that pass's
    // OWN view/projection. RtCamera::update() is first-touch-wins per frame and
    // those passes render BEFORE the main scene in most engines, so without
    // this gate the reflection camera claims the Main camera every frame and
    // the whole scene is traced from the wrong viewpoint ("raytracer uses the
    // wrong camera"). The geometry itself still submits (Unknown falls back to
    // the Main camera pose at the call site); it just never drives a camera.
    if (input.getTransformData().offscreenRenderTarget) {
      ONCE(KENSHI_DIAGNOSTIC_INFO("[RTX] CameraManager: ignoring camera from an offscreen render-target pass (reflection/cubemap/mirror)"));
      return input.testCategoryFlags(InstanceCategories::Sky) ? CameraType::Sky : CameraType::Unknown;
    }

    switch (RtxOptions::fusedWorldViewMode()) {
    case FusedWorldViewMode::None:
      if (!input.getTransformData().cameraRelativeView
       && input.getTransformData().objectToView == input.getTransformData().objectToWorld
       && !isIdentityExact(input.getTransformData().objectToView)) {
        return input.testCategoryFlags(InstanceCategories::Sky) ? CameraType::Sky : CameraType::Unknown;
      }
      break;
    case FusedWorldViewMode::View:
      if (Logger::logLevel() >= LogLevel::Warn) {
        // Check if World is identity
        ONCE_IF_FALSE(isIdentityExact(input.getTransformData().objectToWorld),
                      Logger::warn("[RTX-Compatibility] Fused world-view tranform set to View but World transform is not identity!"));
      }
      break;
    case FusedWorldViewMode::World:
      if (Logger::logLevel() >= LogLevel::Warn) {
        // Check if View is identity
        ONCE_IF_FALSE(isIdentityExact(input.getTransformData().objectToView),
                      Logger::warn("[RTX-Compatibility] Fused world-view tranform set to World but View transform is not identity!"));
      }
      break;
    }

    // Get camera params
    DecomposeProjectionParams decomposeProjectionParams = getOrDecomposeProjection(input.getTransformData().viewToProjection);

    // Filter invalid cameras, extreme shearing
    static auto isFovValid = [](float fovA) {
      return fovA >= kFovToleranceRadians;
    };
    static auto areFovsClose = [](float fovA, const RtCamera& cameraB) {
      return std::abs(fovA - cameraB.getFov()) < kFovToleranceRadians;
    };

    if (std::abs(decomposeProjectionParams.shearX) > 0.01f || !isFovValid(decomposeProjectionParams.fov)) {
      ONCE(Logger::warn("[RTX] CameraManager: rejected an invalid camera"));
      return input.getCategoryFlags().test(InstanceCategories::Sky) ? CameraType::Sky : CameraType::Unknown;
    }


    auto isViewModel = [this](float fov, float maxZ, uint32_t frameId) {
      if (RtxOptions::ViewModel::enable()) {
        // Note: max Z check is the top-priority
        if (maxZ <= RtxOptions::ViewModel::maxZThreshold()) {
          return true;
        }
        if (getCamera(CameraType::Main).isValid(frameId)) {
          // FOV is different from Main camera => assume that it's a ViewModel one
          if (!areFovsClose(fov, getCamera(CameraType::Main))) {
            return true;
          }
        }
      }
      return false;
    };

    const uint32_t frameId = m_device->getCurrentFrameId();

    auto cameraType = CameraType::Main;
    if (input.isDrawingToRaytracedRenderTarget) {
      cameraType = CameraType::RenderToTexture;
    } else if (input.testCategoryFlags(InstanceCategories::Sky)) {
      cameraType = CameraType::Sky;
    } else if (!input.getTransformData().exactReplacementCamera
            && isViewModel(decomposeProjectionParams.fov, input.maxZ, frameId)) {
      cameraType = CameraType::ViewModel;
    }

    // The DX11 replacement-camera path submits exact post-VS clip positions
    // in a camera-relative view-space world. Generic camera inference is still
    // useful while that path is unavailable, but it must not win first-touch
    // once exact captures are arriving. Keep the priority window short so a
    // game can fall back automatically during menus, video, or a shader path
    // that genuinely cannot be captured.
    const bool exactReplacementMain = cameraType == CameraType::Main
      && input.getTransformData().exactReplacementCamera;
    const bool exactReplacementRecentlyActive =
      m_lastExactReplacementMainCameraFrame != ~0u
      && frameId >= m_lastExactReplacementMainCameraFrame
      && frameId - m_lastExactReplacementMainCameraFrame <= 2u;
    if (cameraType == CameraType::Main) {
      if (exactReplacementMain) {
        if (m_lastExactReplacementMainCameraFrame == ~0u) {
          KENSHI_DIAGNOSTIC_INFO("[RTX] CameraManager: exact DX11 replacement camera acquired; inferred raster cameras no longer get first-touch priority");
        }
        m_lastExactReplacementMainCameraFrame = frameId;
      } else if (exactReplacementRecentlyActive) {
        return CameraType::Unknown;
      }
    }

    // Unverified camera constants (see DrawCallState::allowMainCameraUpdate) must not steer the
    // Main camera; the draw itself still renders through the Unknown-camera fallback.
    if (cameraType == CameraType::Main && !input.allowMainCameraUpdate) {
      if (logMainCameraUpdates()) {
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[RTX-Compatibility] CameraManager: skipped Main camera update from unverified camera constants on frame ", frameId,
          "; vsHash=0x", std::hex, input.programmableVertexShaderBytecodeHash, std::dec,
          ", pass=", input.passDescription,
          ", drawCallID=", input.drawCallID));
      } else {
        ONCE(Logger::info("[RTX-Compatibility] CameraManager: skipped Main camera update from a draw with unverified camera constants (likely an engine utility pass)."));
      }
      return CameraType::Unknown;
    }

    // Check fov consistency across frames
    if (frameId > 0) {
      if (getCamera(cameraType).isValid(frameId - 1) && !areFovsClose(decomposeProjectionParams.fov, getCamera(cameraType))) {
        ONCE(Logger::warn("[RTX] CameraManager: FOV of a camera changed between frames"));
      }
    }

    auto& camera = getCamera(cameraType);
    auto cameraSequence = RtCameraSequence::getInstance();
    bool shouldUpdateMainCamera = cameraType == CameraType::Main && camera.getLastUpdateFrame() != frameId;
    bool isPlaying = RtCameraSequence::mode() == RtCameraSequence::Mode::Playback;
    bool isBrowsing = RtCameraSequence::mode() == RtCameraSequence::Mode::Browse;
    bool isCameraCut = false;
    // Bind the known native shift only to this frame's real main camera.
    // Matrix history stays in its original native frame for motion reprojection.
    auto& origin = kenshi_origin::state;
    if (shouldUpdateMainCamera) {
      origin.camera = origin.owner == m_device && origin.continuous && origin.eventFrame == frameId
        && frameId > 0 && camera.getLastUpdateFrame() == frameId - 1
        && !isPlaying && !isBrowsing && !RtCamera::enableFreeCamera() ? &camera : nullptr;
    }
    Matrix4 worldToView = input.getTransformData().worldToView;
    Matrix4 viewToProjection = input.getTransformData().viewToProjection;
    if (shouldUpdateMainCamera) {
      kenshi_camera_audit::accept(m_device, &camera, frameId,
        isPlaying || isBrowsing || RtCamera::enableFreeCamera() ? nullptr
          : kenshi_camera_audit::submitted(m_device));
    }

    // Logging-only; the previous Main pose must be captured before camera.update() overwrites it.
    const bool hadPreviousMainCamera =
      logMainCameraUpdates() &&
      shouldUpdateMainCamera &&
      frameId > 0 &&
      getCamera(CameraType::Main).isValid(frameId - 1);
    Vector3 previousMainPosition(0.0f);
    Vector3 previousMainDirection(0.0f);
    float previousMainFov = 0.0f;

    if (hadPreviousMainCamera) {
      const RtCamera& prevMain = getCamera(CameraType::Main);
      previousMainPosition = prevMain.getPosition(false);
      previousMainDirection = prevMain.getDirection(false);
      const float previousDirectionLength = length(previousMainDirection);
      if (previousDirectionLength > 0.0f) {
        previousMainDirection /= previousDirectionLength;
      }
      previousMainFov = prevMain.getFov();
    }

    if (isPlaying || isBrowsing) {
      if (shouldUpdateMainCamera) {
        RtCamera::RtCameraSetting setting;
        cameraSequence->getRecord(cameraSequence->currentFrame(), setting);
        isCameraCut = camera.updateFromSetting(frameId, setting, 0);

        if (isPlaying) {
          cameraSequence->goToNextFrame();
        }
      }
    } else {
      isCameraCut = camera.update(
        frameId,
        worldToView,
        viewToProjection,
        decomposeProjectionParams.fov,
        decomposeProjectionParams.aspectRatio,
        decomposeProjectionParams.nearPlane,
        decomposeProjectionParams.farPlane,
        decomposeProjectionParams.isLHS
      );
    }


    if (shouldUpdateMainCamera && RtCameraSequence::mode() == RtCameraSequence::Mode::Record) {
      auto& setting = camera.getSetting();
      cameraSequence->addRecord(setting);
    }

    if (shouldUpdateMainCamera && logMainCameraUpdates() && camera.isValid(frameId)) {
      float positionDelta = 0.0f;
      float directionDot = 1.0f;
      float fovDiff = 0.0f;
      if (hadPreviousMainCamera) {
        Vector3 currentDirection = camera.getDirection(false);
        const float currentDirectionLength = length(currentDirection);
        if (currentDirectionLength > 0.0f) {
          currentDirection /= currentDirectionLength;
        }

        positionDelta = std::sqrt(lengthSqr(camera.getPosition(false) - previousMainPosition));
        directionDot = dot(currentDirection, previousMainDirection);
        fovDiff = std::abs(camera.getFov() - previousMainFov);
      }

      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[RTX-Compatibility] CameraManager: accepted Main camera on frame ", frameId,
        "; positionDelta=", positionDelta,
        ", directionDot=", directionDot,
        ", fovDiff=", fovDiff,
        ", cameraCut=", isCameraCut ? "true" : "false",
        ", vsHash=0x", std::hex, input.programmableVertexShaderBytecodeHash, std::dec,
        ", pass=", input.passDescription,
        ", drawCallID=", input.drawCallID));
    }

    // Register camera cut when there are significant interruptions to the view (like changing level, or opening a menu)
    if (isCameraCut && cameraType == CameraType::Main) {
      m_lastCameraCutFrameId = m_device->getCurrentFrameId();
    }

    // DX11_V225: track real vs viewport-fallback main cameras for the DX11 layer.
    // These flags describe the draw that actually won the first-touch camera
    // update. Updating them for later Main-classified draws made EndFrame use
    // metadata from a camera that RtCamera::update had ignored.
    if (cameraType == CameraType::Main && shouldUpdateMainCamera) {
      const bool usedViewportFallback = input.getTransformData().usedViewportFallbackProjection;
      m_mainCameraLastUpdateUsedViewportFallback = usedViewportFallback;
      m_mainCameraLastUpdateUsedCameraRelativeView = input.getTransformData().cameraRelativeView;
      if (exactReplacementMain) {
        static uint32_t s_exactReplacementWinnerLogs = 0;
        if (s_exactReplacementWinnerLogs++ < 12u) {
          KENSHI_DIAGNOSTIC_INFO(str::format(
            "[RTX] CameraManager: exact replacement camera won frame ", frameId,
            " projectionDiag=", viewToProjection[0][0], ",",
            viewToProjection[1][1], ",", viewToProjection[2][2],
            " near=", decomposeProjectionParams.nearPlane,
            " far=", decomposeProjectionParams.farPlane));
        }
      }
      if (!usedViewportFallback) {
        m_hasSeenRealMainCamera = true;
      }
    }

    m_lastSetCameraType = cameraType;

    return cameraType;
  }

  bool CameraManager::isCameraCutThisFrame() const {
    return m_lastCameraCutFrameId == m_device->getCurrentFrameId();
  }

  void CameraManager::processExternalCamera(CameraType::Enum type,
                                            const Matrix4& worldToView,
                                            const Matrix4& viewToProjection) {
    DecomposeProjectionParams decomposeProjectionParams = getOrDecomposeProjection(viewToProjection);

    getCamera(type).update(
      m_device->getCurrentFrameId(),
      worldToView,
      viewToProjection,
      decomposeProjectionParams.fov,
      decomposeProjectionParams.aspectRatio,
      decomposeProjectionParams.nearPlane,
      decomposeProjectionParams.farPlane,
      decomposeProjectionParams.isLHS);
  }

    DecomposeProjectionParams CameraManager::getOrDecomposeProjection(const Matrix4& viewToProjection) {
      XXH64_hash_t projectionHash = XXH64(&viewToProjection, sizeof(viewToProjection), 0);
      auto iter = m_decompositionCache.find(projectionHash);
      if (iter != m_decompositionCache.end()) {
        return iter->second;
      }

      DecomposeProjectionParams decomposeProjectionParams;
      decomposeProjection(viewToProjection, decomposeProjectionParams);
      m_decompositionCache.emplace(projectionHash, decomposeProjectionParams);
      return decomposeProjectionParams;
    }
}  // namespace dxvk
