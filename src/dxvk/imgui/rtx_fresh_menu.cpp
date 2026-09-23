/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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

#include "dxvk_imgui.h"
#include "imgui.h"

#include "dxvk_device.h"
#include "dxvk_objects.h"
#include "rtx_render/rtx_context.h"
#include "rtx_render/rtx_dlfg.h"
#include "rtx_render/rtx_dlss.h"
#include "rtx_render/rtx_imgui.h"
#include "rtx_render/rtx_nis.h"
#include "rtx_render/rtx_neural_radiance_cache.h"
#include "rtx_render/rtx_option_layer_gui.h"
#include "rtx_render/rtx_options.h"
#include "rtx_render/rtx_ray_reconstruction.h"
#include "rtx_render/rtx_reflex.h"
#include "rtx_render/rtx_restir_gi_rayquery.h"
#include "rtx_render/rtx_rtxdi_rayquery.h"
#include "rtx_render/rtx_xess.h"
#include "../util/util_string.h"

#include <cfloat>

namespace dxvk {

  extern RemixGui::ComboWithKey<DLSSProfile> dlssProfileCombo;
  extern RemixGui::ComboWithKey<XeSSPreset> xessPresetCombo;
  extern RemixGui::ComboWithKey<int> dlfgMfgModeCombo;
  extern RemixGui::ComboWithKey<ReflexMode> reflexModeCombo;
  RemixGui::ComboWithKey<UpscalerType>& getUpscalerCombo(DxvkDLSS& dlss, DxvkRayReconstruction& rayReconstruction);

  namespace {

    constexpr ImGuiSliderFlags kFreshSliderFlags = ImGuiSliderFlags_AlwaysClamp;
    constexpr ImGuiTreeNodeFlags kFreshHeaderFlags =
      ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_DefaultOpen;
    constexpr ImGuiTreeNodeFlags kFreshClosedHeaderFlags =
      ImGuiTreeNodeFlags_CollapsingHeader;

    RemixGui::ComboWithKey<DlssPreset> freshDlssPresetCombo {
      "DLSS Preset",
      RemixGui::ComboWithKey<DlssPreset>::ComboEntries { {
          {DlssPreset::Off, "Disabled"},
          {DlssPreset::On, "Enabled"},
          {DlssPreset::Custom, "Custom"},
      } }
    };

    RemixGui::ComboWithKey<NisPreset> freshNisPresetCombo {
      "NIS Preset",
      RemixGui::ComboWithKey<NisPreset>::ComboEntries { {
          {NisPreset::Performance, "Performance"},
          {NisPreset::Balanced, "Balanced"},
          {NisPreset::Quality, "Quality"},
          {NisPreset::Fullscreen, "Fullscreen"},
      } }
    };

    RemixGui::ComboWithKey<TaauPreset> freshTaauPresetCombo {
      "TAA-U Preset",
      RemixGui::ComboWithKey<TaauPreset>::ComboEntries { {
          {TaauPreset::UltraPerformance, "Ultra Performance"},
          {TaauPreset::Performance, "Performance"},
          {TaauPreset::Balanced, "Balanced"},
          {TaauPreset::Quality, "Quality"},
          {TaauPreset::Fullscreen, "Fullscreen"},
      } }
    };

    RemixGui::ComboWithKey<GraphicsPreset> freshGraphicsPresetCombo {
      "Graphics Preset",
      RemixGui::ComboWithKey<GraphicsPreset>::ComboEntries { {
          {GraphicsPreset::Ultra, "Ultra"},
          {GraphicsPreset::High, "High"},
          {GraphicsPreset::Medium, "Medium"},
          {GraphicsPreset::Low, "Low"},
          {GraphicsPreset::Custom, "Custom"},
      } }
    };

    RemixGui::ComboWithKey<IntegrateIndirectMode> freshIndirectModeCombo {
      "Indirect Lighting Mode",
      RemixGui::ComboWithKey<IntegrateIndirectMode>::ComboEntries { {
          {IntegrateIndirectMode::ImportanceSampled, "Importance Sampled"},
          {IntegrateIndirectMode::ReSTIRGI, "ReSTIR GI"},
          {IntegrateIndirectMode::NeuralRadianceCache, "Neural Radiance Cache"},
      } }
    };

    RemixGui::ComboWithKey<EnableVsync> freshVsyncCombo {
      "V-Sync",
      RemixGui::ComboWithKey<EnableVsync>::ComboEntries { {
          {EnableVsync::Off, "Disabled"},
          {EnableVsync::On, "Enabled"},
          {EnableVsync::WaitingForImplicitSwapchain, "Auto"},
      } }
    };

    const char* upscalerName(UpscalerType type) {
      return getUpscalerTypeName(type);
    }

    RtxOptionLayer* mutableUserLayer() {
      return const_cast<RtxOptionLayer*>(RtxOptionLayer::getUserLayer());
    }

    void separatorLabel(const char* label) {
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextUnformatted(label);
      ImGui::Spacing();
    }

    void renderLayerControls(const char* label, RtxOptionLayer* layer, const char* idSuffix) {
      if (layer == nullptr) {
        ImGui::Text("%s: unavailable", label);
        return;
      }

      const bool hasUnsaved = layer->hasUnsavedChanges();
      ImGui::Text("%s: %s", label, hasUnsaved ? "unsaved changes" : "clean");
      if (layer->hasSaveableConfigFile()) {
        ImGui::TextWrapped("%s", layer->getFilePath().c_str());
      }

      OptionLayerUI::renderLayerButtons(layer, idSuffix);
    }

    void renderPresetResolution(const char* label) {
      ImGui::TextWrapped(str::format(label, ": ", RtxOptions::resolutionScale()).c_str());
    }
  }

  void ImGUI::showFreshMenu(const Rc<DxvkContext>& ctx, UIType mode) {
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::User);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(std::max(m_userWindowWidth, 640.0f), std::max(320.0f, viewport->WorkSize.x - 32.0f));
    const float height = std::min(std::max(m_userWindowHeight, 640.0f), std::max(320.0f, viewport->WorkSize.y - 24.0f));
    const float posX = viewport->WorkPos.x + std::max(16.0f, viewport->WorkSize.x - width - 16.0f);
    const float posY = viewport->WorkPos.y + 16.0f;

    ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);

    bool open = mode != UIType::None;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("RTX Remix", &open, flags)) {
      ImGui::End();
      if (!open) {
        switchMenu(UIType::None);
      }
      return;
    }

    ImGui::PushItemWidth(largeUiMode() ? m_largeUserWindowWidgeWidth : m_regularUserWindowWidgetWidth);

    if (ImGui::Button("Basic")) {
      switchMenu(UIType::Basic);
      mode = UIType::Basic;
    }
    ImGui::SameLine();
    if (ImGui::Button("Dev")) {
      switchMenu(UIType::Advanced);
      mode = UIType::Advanced;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      switchMenu(UIType::None);
      open = false;
    }

    RemixGui::Separator();
    showMemoryStats();
    RemixGui::Separator();

    if (ImGui::BeginTabBar("FreshRemixTabs")) {
      if (ImGui::BeginTabItem("Graphics")) {
        showFreshGeneralSettings(ctx);
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Rendering")) {
        showFreshRenderingSettings(ctx);
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Assets")) {
        showFreshContentSettings();
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Dev")) {
        showFreshDeveloperSettings();
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Config")) {
        showFreshConfigSettings();
        ImGui::EndTabItem();
      }

      ImGui::EndTabBar();
    }

    ImGui::PopItemWidth();
    ImGui::End();

    if (!open) {
      switchMenu(UIType::None);
    }
  }

  void ImGUI::showFreshGeneralSettings(const Rc<DxvkContext>& ctx) {
    auto common = ctx->getCommonObjects();
    DxvkDLSS& dlss = common->metaDLSS();
    DxvkRayReconstruction& rayReconstruction = common->metaRayReconstruction();

    const DlssPreset prevDlssPreset = RtxOptions::dlssPreset();
    freshDlssPresetCombo.getKey(&RtxOptions::dlssPresetObject());
    if (prevDlssPreset == DlssPreset::Off && RtxOptions::dlssPreset() == DlssPreset::Custom) {
      RtxOptions::resetUpscaler();
    }
    if (prevDlssPreset != RtxOptions::dlssPreset()) {
      RtxOptions::updateUpscalerFromDlssPreset(m_device);
    }

    separatorLabel("Upscaling");

    const UpscalerType oldUpscalerType = RtxOptions::upscalerType();
    const bool oldRayReconstruction = RtxOptions::enableRayReconstruction();
    getUpscalerCombo(dlss, rayReconstruction).getKey(&RtxOptions::upscalerTypeObject());

    showRayReconstructionEnable(rayReconstruction.supportsRayReconstruction());

    if (oldUpscalerType != RtxOptions::upscalerType() || oldRayReconstruction != RtxOptions::enableRayReconstruction()) {
      Logger::info("[Fresh UI] Upscaler changed; preserving user renderer settings");
      RtxOptions::updatePresetFromUpscaler();
    }

    const UpscalerType requestedUpscaler = RtxOptions::upscalerType();
    const UpscalerType runtimeUpscaler = RtxOptions::getSupportedUpscalerForDevice(m_device, requestedUpscaler);
    ImGui::TextWrapped(str::format("Requested: ", upscalerName(requestedUpscaler),
                                   " | Runtime: ", upscalerName(runtimeUpscaler)).c_str());

    switch (RtxOptions::upscalerType()) {
      case UpscalerType::DLSS: {
        dlssProfileCombo.getKey(&RtxOptions::qualityDLSSObject());

        DLSSProfile currentProfile = RtxOptions::qualityDLSS();
        uint32_t inputWidth = 0;
        uint32_t inputHeight = 0;
        if (RtxOptions::enableRayReconstruction() && rayReconstruction.supportsRayReconstruction()) {
          currentProfile = rayReconstruction.getCurrentProfile();
          rayReconstruction.getInputSize(inputWidth, inputHeight);
        } else {
          currentProfile = dlss.getCurrentProfile();
          dlss.getInputSize(inputWidth, inputHeight);
        }
        if (currentProfile == DLSSProfile::Invalid) {
          currentProfile = RtxOptions::qualityDLSS();
        }
        if (currentProfile == DLSSProfile::Invalid) {
          currentProfile = DLSSProfile::Auto;
        }
        ImGui::TextWrapped(str::format("DLSS Mode: ", dlssProfileToString(currentProfile),
                                       " | Render Resolution: ", inputWidth, "x", inputHeight).c_str());
        break;
      }

      case UpscalerType::NIS: {
        const NisPreset prevPreset = RtxOptions::nisPreset();
        freshNisPresetCombo.getKey(&RtxOptions::nisPresetObject());
        if (prevPreset != RtxOptions::nisPreset()) {
          RtxOptions::updateUpscalerFromNisPreset();
        }
        RemixGui::SliderFloat("Resolution Scale", &RtxOptions::resolutionScaleObject(), 0.5f, 1.0f, "%.2f", kFreshSliderFlags);
        RemixGui::SliderFloat("Sharpness", &common->metaNIS().m_sharpness, 0.1f, 1.0f, "%.2f", kFreshSliderFlags);
        RemixGui::Checkbox("Use FP16", &common->metaNIS().m_useFp16);
        break;
      }

      case UpscalerType::TAAU: {
        const TaauPreset prevPreset = RtxOptions::taauPreset();
        freshTaauPresetCombo.getKey(&RtxOptions::taauPresetObject());
        if (prevPreset != RtxOptions::taauPreset()) {
          RtxOptions::updateUpscalerFromTaauPreset();
        }
        RemixGui::SliderFloat("Resolution Scale", &RtxOptions::resolutionScaleObject(), 0.33f, 1.0f, "%.2f", kFreshSliderFlags);
        renderPresetResolution("TAA-U Resolution Scale");
        break;
      }

      case UpscalerType::XeSS: {
        xessPresetCombo.getKey(&DxvkXeSS::XessOptions::presetObject());
        RemixGui::SliderFloat("Resolution Scale", &RtxOptions::resolutionScaleObject(), 0.1f, 1.0f, "%.2f", kFreshSliderFlags);

        uint32_t inputWidth = 0;
        uint32_t inputHeight = 0;
        common->metaXeSS().getInputSize(inputWidth, inputHeight);
        ImGui::TextWrapped(str::format("XeSS Render Resolution: ", inputWidth, "x", inputHeight).c_str());
        break;
      }

      case UpscalerType::None:
        break;
    }

    separatorLabel("Frame Pacing");
    freshVsyncCombo.getKey(&RtxOptions::enableVsyncObject());
    RemixGui::Checkbox("Enable DLSS Frame Generation", &DxvkDLFG::enableObject());
    dlfgMfgModeCombo.getKey(&DxvkDLFG::maxInterpolatedFramesObject());
    reflexModeCombo.getKey(&RtxOptions::reflexModeObject());
    RemixGui::Checkbox("Allow Full Screen Exclusive", &RtxOptions::allowFSEObject());
  }

  void ImGUI::showFreshRenderingSettings(const Rc<DxvkContext>& ctx) {
    auto common = ctx->getCommonObjects();

    if (RemixGui::CollapsingHeader("Path Tracing", kFreshHeaderFlags)) {
      RemixGui::Checkbox("Raytracing Enabled", &RtxOptions::enableRaytracingObject());
      RemixGui::Checkbox("Direct Lighting", &RtxOptions::enableDirectLightingObject());
      RemixGui::Checkbox("Indirect Lighting", &RtxOptions::enableSecondaryBouncesObject());
      freshIndirectModeCombo.getKey(&RtxOptions::integrateIndirectModeObject());
      RemixGui::DragInt("Minimum Path Bounces", &RtxOptions::pathMinBouncesObject(), 1.0f, 0, 15, "%d", kFreshSliderFlags);
      RemixGui::DragInt("Maximum Path Bounces", &RtxOptions::pathMaxBouncesObject(), 1.0f, RtxOptions::pathMinBounces(), 15, "%d", kFreshSliderFlags);
      RemixGui::SliderInt("RIS Light Sample Count", &RtxOptions::risLightSampleCountObject(), 0, 64, "%d", kFreshSliderFlags);
      RemixGui::Checkbox("Use RTXDI", &RtxOptions::useRTXDIObject());
      RemixGui::Checkbox("Unordered Resolve In Indirect Rays", &RtxOptions::enableUnorderedResolveInIndirectRaysObject());
      RemixGui::Checkbox("Unordered Emissive Particles In Indirect Rays", &RtxOptions::enableUnorderedEmissiveParticlesInIndirectRaysObject());
    }

    if (RemixGui::CollapsingHeader("Denoising", kFreshHeaderFlags)) {
      RemixGui::Checkbox("Denoising Enabled", &RtxOptions::useDenoiserObject());
      RemixGui::Checkbox("Separate Direct And Indirect Denoisers", &RtxOptions::denoiseDirectAndIndirectLightingSeparatelyObject());
      RemixGui::Checkbox("Reference Accumulation", &RtxOptions::useDenoiserReferenceModeObject());
      RemixGui::Checkbox("Adaptive Resolution Denoising", &RtxOptions::adaptiveResolutionDenoisingObject());
      RemixGui::Checkbox("Adaptive Accumulation", &RtxOptions::adaptiveAccumulationObject());
      RemixGui::Checkbox("Reset History On Settings Change", &RtxOptions::resetDenoiserHistoryOnSettingsChangeObject());
    }

    if (RemixGui::CollapsingHeader("Lighting Guidance", kFreshClosedHeaderFlags)) {
      RemixGui::DragFloat("Emissive Intensity", &RtxOptions::emissiveIntensityObject(), 0.01f, 0.0f, FLT_MAX, "%.3f", kFreshSliderFlags);
      RemixGui::DragFloat("Firefly Luminance Threshold", &RtxOptions::fireflyFilteringLuminanceThresholdObject(), 0.1f, 0.0f, FLT_MAX, "%.3f", kFreshSliderFlags);
      RemixGui::DragFloat("Secondary Specular Firefly Threshold", &RtxOptions::secondarySpecularFireflyFilteringThresholdObject(), 0.1f, 0.0f, FLT_MAX, "%.3f", kFreshSliderFlags);
      RemixGui::Checkbox("Russian Roulette", &RtxOptions::enableRussianRouletteObject());
      RemixGui::DragFloat("Indirect Ray Spread Angle Factor", &RtxOptions::indirectRaySpreadAngleFactorObject(), 0.001f, 0.0f, 1.0f, "%.3f", kFreshSliderFlags);
    }

    if (RemixGui::CollapsingHeader("Runtime Modules", kFreshClosedHeaderFlags)) {
      common->metaReSTIRGIRayQuery().showImguiSettings();
      common->metaNeuralRadianceCache().showImguiSettings(*ctx);
      common->metaRtxdiRayQuery().showImguiSettings();
    }
  }

  void ImGUI::showFreshContentSettings() {
    RemixGui::Checkbox("Enable All Enhanced Assets", &RtxOptions::enableReplacementAssetsObject());
    RemixGui::Checkbox("Enable Enhanced Materials", &RtxOptions::enableReplacementMaterialsObject());
    RemixGui::Checkbox("Enable Enhanced Meshes", &RtxOptions::enableReplacementMeshesObject());
    RemixGui::Checkbox("Enable Enhanced Lights", &RtxOptions::enableReplacementLightsObject());

    separatorLabel("Geometry");
    RemixGui::DragFloat("Scene Unit Scale", &RtxOptions::sceneScaleObject(), 0.00001f, 0.00001f, FLT_MAX, "%.5f", kFreshSliderFlags);
    RemixGui::DragFloat("Unique Object Search Distance", &RtxOptions::uniqueObjectDistanceObject(), 0.01f, 0.0f, FLT_MAX, "%.3f", kFreshSliderFlags);
    RemixGui::Checkbox("Skip Objects Rendered With Unknown Camera", &RtxOptions::skipObjectsWithUnknownCameraObject());
    RemixGui::Checkbox("Validate CPU Index Data", &RtxOptions::validateCPUIndexDataObject());

    separatorLabel("Camera");
    RemixGui::Checkbox("Force Camera Jitter", &RtxOptions::forceCameraJitterObject());
    RemixGui::Checkbox("Override Near Plane", &RtxOptions::enableNearPlaneOverrideObject());
    RemixGui::DragFloat("Near Plane Override", &RtxOptions::nearPlaneOverrideObject(), 0.001f, 0.001f, 1000.0f, "%.3f", kFreshSliderFlags);
  }

  void ImGUI::showFreshDeveloperSettings() {
    freshGraphicsPresetCombo.getKey(&RtxOptions::graphicsPresetObject());

    separatorLabel("Interface");
    RemixGui::Checkbox("Default To Dev UI", &RtxOptions::defaultToAdvancedUIObject());
    RemixGui::Checkbox("Show UI Cursor", &RtxOptions::showUICursorObject());
    RemixGui::Checkbox("Block Input To Game In UI", &RtxOptions::blockInputToGameInUIObject());
    RemixGui::Checkbox("Restore Cursor Position", &RtxOptions::restoreCursorPositionObject());

    separatorLabel("Capture And Viewport");
    RemixGui::DragIntRange2("Draw Call Range Filter", &RtxOptions::drawCallRangeObject(), 1.0f, 0, INT32_MAX, nullptr, nullptr, kFreshSliderFlags);
    RemixGui::Checkbox("Anti-Cull Objects", &RtxOptions::AntiCulling::Object::enableObject());
    RemixGui::Checkbox("Anti-Cull Lights", &RtxOptions::AntiCulling::Light::enableObject());
    RemixGui::DragInt("Objects To Keep", &RtxOptions::AntiCulling::Object::numObjectsToKeepObject(), 1.0f, 0, INT32_MAX, "%d", kFreshSliderFlags);
    RemixGui::DragInt("Light Lifetime Frames", &RtxOptions::AntiCulling::Light::numFramesToExtendLightLifetimeObject(), 1.0f, 0, INT32_MAX, "%d", kFreshSliderFlags);

    separatorLabel("Shader Compilation");
    RemixGui::Checkbox("Async Shader Compilation", &RtxOptions::Shader::enableAsyncCompilationObject());
    RemixGui::Checkbox("Shader Compilation UI", &RtxOptions::Shader::enableAsyncCompilationUIObject());
    RemixGui::DragInt("Async Compilation Throttle", &RtxOptions::Shader::asyncCompilationThrottleMillisecondsObject(), 1.0f, 0, 1000, "%d ms", kFreshSliderFlags);
    RemixGui::DragInt("Active Scene Compile Throttle", &RtxOptions::Shader::activeSceneCompilationThrottleMillisecondsObject(), 1.0f, 0, 1000, "%d ms", kFreshSliderFlags);
    RemixGui::DragInt("Max Shader Compilations", &RtxOptions::Shader::maxConcurrentShaderCompilationsObject(), 1.0f, 1, 32, "%d", kFreshSliderFlags);
  }

  void ImGUI::showFreshConfigSettings() {
    separatorLabel("Option Layers");
    renderLayerControls("user.conf", mutableUserLayer(), "FreshUser");
    RemixGui::Separator();
    renderLayerControls("rtx.conf", RtxOptionLayer::getRtxConfLayer(), "FreshRtxConf");
  }
}
