#include "../../util/util_kenshi_telemetry.h"
/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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
#include <cstring>
#include <cmath>
#include <cassert>
#include <chrono>
#include <atomic>
#include <iomanip>

#include "dxvk_device.h"
#include "dxvk_gpu_query.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_shader_manager.h"
#include "dxvk_adapter.h"
#include "rtx_context.h"
#include "../../util/util_kenshi_fault.h"
#include "../../util/util_kenshi_terrain_audit.h"
#include "../../util/util_kenshi_material_probe.h"
#include "../../util/util_kenshi_terrain_profile.h"
// DX11_V225: complete type for the fixed-function-equivalent VS constant block.
#include "perf_debug.h"
#include "../../d3d11/d3d11_fixed_function.h"
#include "rtx_asset_exporter.h"
#include "rtx_options.h"
#include "rtx_kenshi_terrain_options.h"
#include "rtx_kenshi_options.h"
#include "rtx_bindless_resource_manager.h"
#include "rtx_opacity_micromap_manager.h"
#include "rtx_asset_replacer.h"
#include "rtx_terrain_baker.h"
#include "rtx_texture_manager.h"
#include "rtx_neural_radiance_cache.h"
#include "rtx_ray_reconstruction.h"
#include "rtx_xess.h"
#include "rtx_rtxdi_rayquery.h"
#include "rtx_restir_gi_rayquery.h"
#include "rtx_composite.h"
#include "rtx_debug_view.h"
// Kenshi heat-haze depth re-encode and sign overlay passes.
#include <rtx_shaders/kenshi_heat_haze_depth.h>
#include <rtx_shaders/kenshi_sign_overlay.h>
#include "rtx/pass/kenshi_sign_overlay.h"

#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/raytrace_args.h"
#include "rtx/pass/volume_args.h"
#include "rtx/pass/kenshi_heat_haze_depth.h"
#include "rtx/utility/debug_view_indices.h"
#include "rtx/utility/gpu_printing.h"
#include "rtx_nrd_settings.h"
#include "rtx_scene_manager.h"

#include "../d3d11/d3d11_state.h"
#include "../d3d11/d3d11_spec_constants.h"
// computeConstantBufferBinding, to rebind a patched b0 for the per-cube-face sky probe draws.
#include "../../dxbc/dxbc_util.h"

#include "../util/log/metrics.h"
#include "../util/util_defer.h"
#include "../util/util_global_time.h"
#include "../../util/util_env.h"

#include "rtx_imgui.h"
#include "dxvk_scoped_annotation.h"
#include "imgui/dxvk_imgui.h"

#include <cctype>
#include <ctime>
#include <map>
#include <nvapi.h>

#include <NvLowLatencyVk.h>
#include <pclstats.h>

#include "rtx_matrix_helpers.h"
#include "../util/util_fastops.h"

// Destructor requires the struct definitions
#include "rtx_sky.h"

namespace dxvk {

  // Owned by the context, allocated only while native highlights are submitted. These images never
  // enter the scene/material/denoiser caches.
  struct KenshiSignOverlay {
    Resources::Resource color;
    Resources::Resource depth;
    Matrix4 projection;
    Vector2 viewportDepth;
    uint32_t frame = ~0u;
  };

  Metrics Metrics::s_instance;

  bool g_allowSrgbConversionForOutput = true;
  bool g_forceKeepObjectPickingImage = false;

  // Defined within an unnamed namespace to ensure unique definition across the binary, matching the
  // other pass-local shader declarations.
  namespace {
    class KenshiSignOverlayShader : public ManagedShader {
      SHADER_SOURCE(KenshiSignOverlayShader, VK_SHADER_STAGE_COMPUTE_BIT, kenshi_sign_overlay)
      PUSH_CONSTANTS(KenshiSignOverlayArgs)
      BEGIN_PARAMETER()
        RW_TEXTURE2D(0)
        TEXTURE2D(1)
        TEXTURE2D(2)
        TEXTURE2D(3)
      END_PARAMETER()
    };

    class KenshiHeatHazeDepthShader : public ManagedShader {
      SHADER_SOURCE(KenshiHeatHazeDepthShader, VK_SHADER_STAGE_COMPUTE_BIT, kenshi_heat_haze_depth)

      PUSH_CONSTANTS(KenshiHeatHazeDepthArgs)

      BEGIN_PARAMETER()
        RW_TEXTURE2D(KENSHI_HEAT_HAZE_DEPTH_BINDING_OUTPUT)
        TEXTURE2D(KENSHI_HEAT_HAZE_DEPTH_BINDING_HIT_DISTANCE_INPUT)
      END_PARAMETER()

      inline static VkExtent3D groupSize = VkExtent3D { 16, 16, 1 };
    };

    PREWARM_SHADER_PIPELINE(KenshiHeatHazeDepthShader);
  }

  static std::string describeRaytracerImage(const Rc<DxvkImage>& image) {
    if (image == nullptr)
      return "missing";

    const auto& info = image->info();
    return str::format(
      info.extent.width, "x", info.extent.height, "x", info.extent.depth,
      "/fmt", static_cast<uint32_t>(info.format),
      "/layers", info.numLayers,
      "/samples", static_cast<uint32_t>(info.sampleCount));
  }

  void RtxContext::takeScreenshot(std::string imageName, Rc<DxvkImage> image) {
    if (!kenshi_telemetry::fileOutputEnabled()) return;
    // Optional debug resources are not guaranteed to be allocated in every
    // rendering mode. The developer-menu screenshot path used to pass a null
    // image into AssetExporter::exportImage, which dereferenced image->info()
    // and crashed the game. Skip absent buffers while still exporting every
    // resource that exists; the warning identifies which stage is unavailable.
    if (image == nullptr) {
      Logger::warn(str::format("RTX: Skipping screenshot for unallocated image '", imageName, "'"));
      return;
    }

    // NOTE: Improve this, I'd like all textures from the same frame to have the same time code...  Currently sampling the time on each "dump op" results in different timecodes.
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);

    std::string path = env::getEnvVar("DXVK_SCREENSHOT_PATH");

    if (path.empty()) {
      path = "./Screenshots/";
    } else if (*path.rbegin() != '/') {
      path += '/';
    }

    auto& exporter = getCommonObjects()->metaExporter();
    // The frame id disambiguates captures taken within the same second (the timestamp has one-second
    // resolution) and groups every image of one capture.
    exporter.dumpImageToFile(this, path, str::format(imageName, "_", tm.tm_mday, tm.tm_mon, tm.tm_year, "-", tm.tm_hour, tm.tm_min, tm.tm_sec, "-f", m_device->getCurrentFrameId(), ".dds"), image);
  }

  void RtxContext::blitImageHelper(Rc<DxvkContext> ctx, const Rc<DxvkImage>& srcImage, const Rc<DxvkImage>& dstImage, VkFilter filter) {
    const DxvkFormatInfo* dstFormatInfo = imageFormatInfo(dstImage->info().format);
    const DxvkFormatInfo* srcFormatInfo = imageFormatInfo(srcImage->info().format);

    const VkImageSubresource dstSubresource = { dstFormatInfo->aspectMask, 0, 0 };
    const VkImageSubresource srcSubresource = { srcFormatInfo->aspectMask, 0, 0 };

    VkExtent3D srcExtent = srcImage->mipLevelExtent(srcSubresource.mipLevel);
    VkExtent3D dstExtent = dstImage->mipLevelExtent(dstSubresource.mipLevel);

    VkImageSubresourceLayers dstSubresourceLayers = {
      dstSubresource.aspectMask,
      dstSubresource.mipLevel,
      dstSubresource.arrayLayer, 1 };

    VkImageSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspectMask,
      srcSubresource.mipLevel,
      srcSubresource.arrayLayer, 1 };

    VkImageBlit blitInfo;

    blitInfo.dstSubresource = dstSubresourceLayers;
    blitInfo.srcSubresource = srcSubresourceLayers;

    blitInfo.dstOffsets[0] = VkOffset3D{ 0,                        0,                       0 };
    blitInfo.dstOffsets[1] = VkOffset3D{ int32_t(dstExtent.width),  int32_t(dstExtent.height),  1 };

    blitInfo.srcOffsets[0] = VkOffset3D{ 0,                          0,                         0 };
    blitInfo.srcOffsets[1] = VkOffset3D{ int32_t(srcExtent.width),    int32_t(srcExtent.height),    1 };

    VkComponentMapping swizzle = {
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
    };

    ctx->blitImage(dstImage, swizzle, srcImage, swizzle, blitInfo, filter);
  }

  // Kenshi's heat-haze post process (data/materials/post/heathaze.hlsl) scales its distortion by
  // global_gbuffer target 2, an R32_FLOAT target the deferred pixel shaders fill with
  // length(worldPos - cameraPos)/farClip:
  //   sample  r1, v1.xy, t2, s2          // depthMap.r
  //   eq      r0.z, r1.x, l(0.0)
  //   mul_sat r0.w, r1.x, l(6.0)
  //   movc    r0.z, r0.z, l(1.0), r0.w   // depth == 0 -> FULL amplitude
  // Under path tracing the draws that fill it never reach raster, so every pixel takes the depth == 0
  // branch. m_primaryHitDistance is the same radial distance in world units, so this only re-encodes it:
  // farClip-normalised, output extent, and 0 instead of -1 for a miss.
  void RtxContext::kenshiWriteHeatHazeDepth(const Rc<DxvkImage>& gameDepthImage) {
    if (!KenshiOptions::kenshiHeatHazeDepth() || gameDepthImage == nullptr) {
      return;
    }

    Resources::RaytracingOutput& rtOutput = getResourceManager().getRaytracingOutput();
    if (!rtOutput.isReady() || rtOutput.m_primaryHitDistance.image == nullptr
     || rtOutput.m_primaryHitDistance.view == nullptr) {
      return;
    }

    const VkExtent3D renderExtent = rtOutput.m_primaryHitDistance.image->info().extent;
    if (renderExtent.width == 0u || renderExtent.height == 0u) {
      return;
    }

    // The game's own far clip, recovered with the projection matrix. If this is
    // not the value Kenshi's deferred shaders divide by, the ramp lands at the
    // wrong distance - which is the first thing to check if the falloff is
    // present but mis-scaled, so report it once.
    const float farPlane = getSceneManager().getCamera().getFarPlane();
    static bool sFarPlaneLogged = false;
    if (!sFarPlaneLogged) {
      sFarPlaneLogged = true;
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11][heat-haze] depth re-encode active: farClip=", farPlane,
        " distanceScale=", KenshiOptions::kenshiHeatHazeDistanceScale(),
        " render=", renderExtent.width, "x", renderExtent.height,
        " target=", gameDepthImage->info().extent.width, "x", gameDepthImage->info().extent.height,
        " targetFormat=", static_cast<uint32_t>(gameDepthImage->info().format)));
    }

    if (!std::isfinite(farPlane) || farPlane <= 0.0f) {
      ONCE(Logger::warn(str::format(
        "[Remix-DX11][heat-haze] skipped: implausible far clip ", farPlane)));
      return;
    }

    const float distanceScale = std::max(0.01f, KenshiOptions::kenshiHeatHazeDistanceScale());

    if (m_kenshiHeatHazeDepth.image == nullptr
     || m_kenshiHeatHazeDepthExtent.width != renderExtent.width
     || m_kenshiHeatHazeDepthExtent.height != renderExtent.height) {
      Rc<DxvkContext> ctx(this);
      m_kenshiHeatHazeDepth = Resources::createImageResource(
        ctx, "kenshi heat haze depth", renderExtent, VK_FORMAT_R32_SFLOAT);
      m_kenshiHeatHazeDepthExtent = renderExtent;
    }

    ScopedGpuProfileZone(this, "Kenshi Heat Haze Depth");

    KenshiHeatHazeDepthArgs args = {};
    args.extent = uint2(renderExtent.width, renderExtent.height);
    args.invFarClip = 1.0f / (farPlane * distanceScale);

    // This runs from the injection boundary, mid-frame, with the game's own
    // raster draws following on the same command list - the bank has to go back
    // afterwards or those draws read push constants from the wrong one. Same
    // contract as RtxGeometryUtils::dispatchKenshiBloodProjection.
    setPushConstantBank(DxvkPushConstantBank::RTX);
    pushConstants(0, sizeof(KenshiHeatHazeDepthArgs), &args);

    bindResourceView(KENSHI_HEAT_HAZE_DEPTH_BINDING_OUTPUT, m_kenshiHeatHazeDepth.view, nullptr);
    bindResourceView(KENSHI_HEAT_HAZE_DEPTH_BINDING_HIT_DISTANCE_INPUT, rtOutput.m_primaryHitDistance.view, nullptr);
    bindShader(VK_SHADER_STAGE_COMPUTE_BIT, KenshiHeatHazeDepthShader::getShader());

    const VkExtent3D workgroups = util::computeBlockCount(
      renderExtent, KenshiHeatHazeDepthShader::groupSize);
    dispatch(workgroups.width, workgroups.height, workgroups.depth);

    setPushConstantBank(DxvkPushConstantBank::D3D11);

    // The destination is a plain D3D11 render target with no UAV bind flag, so
    // it carries no storage usage and cannot be written by the dispatch above.
    // The blit also absorbs the render-to-output resolution change under DLSS.
    blitImageHelper(this, m_kenshiHeatHazeDepth.image, gameDepthImage, VkFilter::VK_FILTER_NEAREST);
  }

  RtxContext::RtxContext(const Rc<DxvkDevice>& device)
    : DxvkContext(device) {
    // DX11_V229_RTXOPTIONS_NULL_GUARD: guarantee the RtxOptions singleton exists before this
    // constructor touches any option. setIsOpacityMicromapSupported() (and other raw setters
    // below) dereference RtxOptions::s_instance directly with no lazy-init; if this RtxContext
    // is constructed before DxvkInstance's RtxOptions::Create() ran, s_instance is null and the
    // write faults (0xc0000005 at s_instance+8) at launch on every GPU/game. Create() is
    // idempotent (no-op when the singleton already exists), so this is safe and load-bearing.
    RtxOptions::Create();

    // Note: This may not be the best place to check for these features/properties, they ideally would be specified as
    // required upfront, but there's no good place to do that for this RTX extension (the D3D11 stuff does it before device
    // creation), so instead we just check for what is needed.
    // Note: When adding new extensions update DxvkAdapter::createDevice as it is what brings these features over.
    m_rayTracingSupported = (m_device->features().core.features.shaderInt16 &&
                             m_device->features().vulkan11Features.storageBuffer16BitAccess &&
                             m_device->features().vulkan11Features.uniformAndStorageBuffer16BitAccess &&
                             m_device->features().vulkan12Features.bufferDeviceAddress &&
                             m_device->features().vulkan12Features.descriptorIndexing &&
                             m_device->features().vulkan12Features.runtimeDescriptorArray &&
                             m_device->features().vulkan12Features.descriptorBindingPartiallyBound &&
                             m_device->features().vulkan12Features.shaderStorageBufferArrayNonUniformIndexing &&
                             m_device->features().vulkan12Features.shaderSampledImageArrayNonUniformIndexing &&
                             m_device->features().vulkan12Features.descriptorBindingVariableDescriptorCount &&
                             m_device->features().vulkan12Features.shaderInt8 &&
                             m_device->features().vulkan12Features.shaderFloat16 &&
                             m_device->features().vulkan12Features.uniformAndStorageBuffer8BitAccess &&
                             m_device->features().khrAccelerationStructureFeatures.accelerationStructure &&
                             m_device->features().khrRayQueryFeatures.rayQuery &&
                             m_device->features().khrDeviceRayTracingPipelineFeatures.rayTracingPipeline &&
                             m_device->extensions().khrShaderInt8Float16Types &&
                             m_device->properties().coreSubgroup.subgroupSize >= 1 &&
                             m_device->properties().coreSubgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT &&
                             m_device->properties().coreSubgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT);

    m_dlssSupported = (m_device->extensions().nvxBinaryImport &&
                       m_device->extensions().nvxImageViewHandle &&
                       m_device->extensions().khrPushDescriptor);


    if (env::getEnvVar("DXVK_DUMP_SCREENSHOT_FRAME") != "") {
      m_screenshotFrameNum = stoul(env::getEnvVar("DXVK_DUMP_SCREENSHOT_FRAME"));
      m_screenshotFrameEnabled = true;
    }

    if (env::getEnvVar("DXVK_TERMINATE_APP_FRAME") != "") {
      m_terminateAppFrameNum = stoul(env::getEnvVar("DXVK_TERMINATE_APP_FRAME"));
      m_triggerDelayedTerminate = true;
    }

    Metrics::TestTraceConfig testTraceConfig;
    testTraceConfig.enabled = RtxOptions::Automation::enableTestTrace();
    testTraceConfig.screenshotFrameEnabled = m_screenshotFrameEnabled;
    testTraceConfig.screenshotFrameNum = m_screenshotFrameNum;
    testTraceConfig.terminateAppFrameNum = m_terminateAppFrameNum;
    Metrics::configureTestTrace(testTraceConfig);

    m_prevRunningTime = std::chrono::steady_clock::now();

    checkOpacityMicromapSupport();
    checkShaderExecutionReorderingSupport();
    checkNeuralRadianceCacheSupport();
    reportCpuSimdSupport();

    GlobalTime::get().init(RtxOptions::timeDeltaBetweenFrames() * 0.001f);
    GlobalTime::get().setAdvanceTime(RtxOptions::advanceTime());

    // Initialize atmosphere system.
    m_atmosphere = std::make_unique<RtxAtmosphere>(m_device.ptr());
  }

  RtxContext::~RtxContext() {
    getCommonObjects()->metaExporter().waitForAllExportsToComplete();

    if (m_screenshotFrameNum != -1 || m_terminateAppFrameNum != -1) {
      Metrics::serialize();
    }

  }

  void RtxContext::captureKenshiSignOverlay(const DrawParameters& params, const Matrix4& projection) {
    const auto originalTargets = m_state.om.renderTargets;
    if (originalTargets.color[0].view == nullptr) return;
    const VkExtent3D extent = originalTargets.color[0].view->mipLevelExtent(0);
    const auto& viewport = m_state.vp.viewports[0];
    if (!extent.width || !extent.height || viewport.maxDepth <= viewport.minDepth) return;

    if (!m_kenshiSignOverlay || m_kenshiSignOverlay->color.image->info().extent.width != extent.width
                            || m_kenshiSignOverlay->color.image->info().extent.height != extent.height) {
      auto overlay = std::make_unique<KenshiSignOverlay>();
      const auto makeTarget = [&](bool depth) {
        DxvkImageCreateInfo info = {};
        info.type = VK_IMAGE_TYPE_2D;
        info.format = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
        info.extent = extent;
        info.numLayers = info.mipLevels = 1;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                   | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                    | (depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                             : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT
                    | (depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                             : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.layout = VK_IMAGE_LAYOUT_GENERAL;
        Resources::Resource result;
        result.image = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
          DxvkMemoryStats::Category::RTXRenderTarget, depth ? "Kenshi sign depth" : "Kenshi sign color");
        DxvkImageViewCreateInfo view;
        view.format = info.format;
        view.usage = info.usage & ~VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        view.aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        view.numLevels = view.numLayers = 1;
        result.view = m_device->createImageView(result.image, view);
        return result;
      };
      overlay->color = makeTarget(false);
      overlay->depth = makeTarget(true);
      m_kenshiSignOverlay = std::move(overlay);
    }
    auto& overlay = *m_kenshiSignOverlay;
    const uint32_t frame = m_device->getCurrentFrameId();
    if (overlay.frame != frame) {
      VkClearValue clear = {};
      DxvkContext::clearRenderTarget(overlay.color.view, VK_IMAGE_ASPECT_COLOR_BIT, clear);
      clear.depthStencil.depth = 1.0f;
      DxvkContext::clearRenderTarget(overlay.depth.view, VK_IMAGE_ASPECT_DEPTH_BIT, clear);
      overlay.frame = frame;
    }
    overlay.projection = projection;
    overlay.viewportDepth = Vector2(viewport.minDepth, 1.0f / (viewport.maxDepth - viewport.minDepth));

    // Execute the original VS/PS and original buffers now, while their native
    // constants are valid. Keep culling, depth test, depth writes and depth bias.
    // Disable blending only in this capture: store straight RGBA for composition.
    const auto blend = m_state.gp.state.omBlend[0].state();
    DxvkBlendMode savedBlend = { blend.blendEnable, blend.srcColorBlendFactor, blend.dstColorBlendFactor,
      blend.colorBlendOp, blend.srcAlphaBlendFactor, blend.dstAlphaBlendFactor,
      blend.alphaBlendOp, blend.colorWriteMask };
    DxvkBlendMode captureBlend = savedBlend;
    captureBlend.enableBlending = VK_FALSE;
    captureBlend.writeMask = 0xf;
    setBlendMode(0, captureBlend);
    DxvkRenderTargets targets;
    targets.color[0] = { overlay.color.view, VK_IMAGE_LAYOUT_GENERAL };
    targets.depth = { overlay.depth.view, VK_IMAGE_LAYOUT_GENERAL };
    bindRenderTargets(targets);
    if (params.indexCount)
      DxvkContext::drawIndexed(params.indexCount, 1, params.firstIndex, params.vertexOffset, 0);
    else
      DxvkContext::draw(params.vertexCount, 1, params.vertexOffset, 0);
    bindRenderTargets(originalTargets);
    setBlendMode(0, savedBlend);
  }

  void RtxContext::compositeKenshiSignOverlay(const Resources::RaytracingOutput& rtOutput, bool outputIsGammaEncoded) {
    if (!m_kenshiSignOverlay || m_kenshiSignOverlay->frame != m_device->getCurrentFrameId()
     || m_common->metaDebugView().debugViewIdx() != DEBUG_VIEW_DISABLED) return;
    const auto& overlay = *m_kenshiSignOverlay;
    const auto output = rtOutput.m_finalOutput.resource(Resources::AccessType::ReadWrite);
    const auto extent = output.image->info().extent;
    const auto depthExtent = rtOutput.m_primaryLinearViewZ.image->info().extent;
    const auto signExtent = overlay.color.image->info().extent;
    const auto& p = overlay.projection;
    const auto& jittered = getSceneManager().getCamera().getViewToProjectionJittered();
    if (std::abs(p[2][3]) < 0.5f || std::abs(jittered[2][3]) < 0.5) return;

    KenshiSignOverlayArgs args = {};
    args.outputExtent = uint2(extent.width, extent.height);
    args.signExtent = uint2(signExtent.width, signExtent.height);
    args.depthExtent = uint2(depthExtent.width, depthExtent.height);
    args.depthPixelShift = float2(
      float((jittered[2][0] / jittered[2][3] - p[2][0] / p[2][3]) * 0.5 * depthExtent.width),
      float((jittered[2][1] / jittered[2][3] - p[2][1] / p[2][3]) * -0.5 * depthExtent.height));
    args.projection = float4(p[2][2], p[3][2], p[2][3], p[3][3]);
    args.viewportDepth = float2(overlay.viewportDepth.x, overlay.viewportDepth.y);
    args.missViewZ = rtOutput.m_raytraceArgs.primaryDirectMissLinearViewZ;
    args.outputIsGammaEncoded = outputIsGammaEncoded ? 1u : 0u;
    setPushConstantBank(DxvkPushConstantBank::RTX);
    pushConstants(0, sizeof(args), &args);
    bindResourceView(0, output.view, nullptr);
    bindResourceView(1, overlay.color.view, nullptr);
    bindResourceView(2, overlay.depth.view, nullptr);
    bindResourceView(3, rtOutput.m_primaryLinearViewZ.view, nullptr);
    bindShader(VK_SHADER_STAGE_COMPUTE_BIT, KenshiSignOverlayShader::getShader());
    dispatch((extent.width + 15u) / 16u, (extent.height + 15u) / 16u, 1);
  }

  SceneManager& RtxContext::getSceneManager() {
    return getCommonObjects()->getSceneManager();
  }
  Resources& RtxContext::getResourceManager() {
    return getCommonObjects()->getResources();
  }

  // Returns GPU idle time between calls to this in milliseconds
  float RtxContext::getGpuIdleTimeSinceLastCall() {
    uint64_t currGpuIdleTicks = m_device->getStatCounters().getCtr(DxvkStatCounter::GpuIdleTicks);
    if (!m_prevGpuIdleTicksInitialized) {
      // DxvkSubmissionQueue::gpuIdleTicks() is a monotonic accumulator, so the
      // only invalid sample here is the first one before we've established a baseline.
      m_prevGpuIdleTicks = currGpuIdleTicks;
      m_prevGpuIdleTicksInitialized = true;
      return 0.0f;
    }

    uint64_t delta = currGpuIdleTicks - m_prevGpuIdleTicks;
    m_prevGpuIdleTicks = currGpuIdleTicks;

    return static_cast<float>(delta) * 0.001f; // to milliseconds
  }

  VkExtent3D RtxContext::setDownscaleExtent(const VkExtent3D& upscaleExtent) {
    ScopedCpuProfileZone();
    VkExtent3D downscaleExtent;
    if (shouldUseDLSS()) {
      DxvkDLSS& dlss = m_common->metaDLSS();
      uint32_t displaySize[2] = { upscaleExtent.width, upscaleExtent.height };
      uint32_t renderSize[2];
      dlss.setSetting(displaySize, RtxOptions::qualityDLSS(), renderSize);
      downscaleExtent.width = renderSize[0];
      downscaleExtent.height = renderSize[1];
      downscaleExtent.depth = 1;
    } else if (shouldUseRayReconstruction()) {
      DxvkRayReconstruction& rayReconstruction = m_common->metaRayReconstruction();
      uint32_t displaySize[2] = { upscaleExtent.width, upscaleExtent.height };
      uint32_t renderSize[2];
      rayReconstruction.setSettings(displaySize, RtxOptions::qualityDLSS(), renderSize);
      downscaleExtent.width = renderSize[0];
      downscaleExtent.height = renderSize[1];
      downscaleExtent.depth = 1;
    } else if (shouldUseXeSS()) {
      DxvkXeSS& xess = m_common->metaXeSS();
      uint32_t displaySize[2] = { upscaleExtent.width, upscaleExtent.height };
      uint32_t renderSize[2];
      xess.setSetting(displaySize, DxvkXeSS::XessOptions::preset(), renderSize);
      downscaleExtent.width = renderSize[0];
      downscaleExtent.height = renderSize[1];
      downscaleExtent.depth = 1;
      
      // XeSS: Apply recommended jitter sequence length if enabled
      if (DxvkXeSS::XessOptions::useRecommendedJitterSequenceLength() && xess.isActive()) {
        uint32_t recommendedJitterLength = xess.calcRecommendedJitterSequenceLength();
        uint32_t currentJitterLength = RtxOptions::cameraJitterSequenceLength();
      }
    } else if (shouldUseNIS() || shouldUseTAA()) {
      auto resolutionScale = RtxOptions::resolutionScale();
      downscaleExtent.width = uint32_t(std::roundf(upscaleExtent.width * resolutionScale));
      downscaleExtent.height = uint32_t(std::roundf(upscaleExtent.height * resolutionScale));
      downscaleExtent.depth = 1;
    } else {
      downscaleExtent = upscaleExtent;
    }
    downscaleExtent.width = std::max(downscaleExtent.width, 1u);
    downscaleExtent.height = std::max(downscaleExtent.height, 1u);

    return downscaleExtent;
  }

  void RtxContext::resetScreenResolution(const VkExtent3D& upscaleExtent) {
    // Calculate extents based on if DLSS is enabled or not
    const VkExtent3D downscaleExtent = setDownscaleExtent(upscaleExtent);

    // Resize the RT screen dependant buffers (if needed)
    getResourceManager().onResize(this, downscaleExtent, upscaleExtent);

    uint32_t renderSize[] = { downscaleExtent.width, downscaleExtent.height };
    uint32_t displaySize[] = { upscaleExtent.width, upscaleExtent.height };

    // Set resolution to cameras for jittering
    for (int i = 0; i < CameraType::Count; i++) {
      if (i == CameraType::Unknown) {
        continue;
      }
      RtCamera& camera = getSceneManager().getCameraManager().getCamera(static_cast<CameraType::Enum>(i));
      camera.setResolution(renderSize, displaySize);
    }

    // Note: Ensure the rendering resolution is not more than 2^14 - 1. This is due to assuming only
    // 14 of the 16 bits of an integer will be used for these pixel coordinates to pack additional data
    // into the free bits in memory payload structures on the GPU.
    assert((renderSize[0] < (1 << 14)) && (renderSize[1] < (1 << 14)));

    // With reloadTextureWhenResolutionChanged ON, textures will get reloaded when resolution is changed,
    // which may cause long wait when changing DLSS-RR or other upscalers' settings.
    // Therefore reloadTextureWhenResolutionChanged is set to OFF by default to improve performance. 
    if (RtxOptions::reloadTextureWhenResolutionChanged()) {
      getSceneManager().requestTextureVramFree();
    }
  }

  bool RtxContext::useRayReconstruction() const {
    return m_common->metaRayReconstruction().useRayReconstruction();
  }

  RtxContext::InternalUpscaler RtxContext::getCurrentFrameUpscaler() {
    if (shouldUseDLSS() && m_common->metaDLSS().isActive()) {
      return InternalUpscaler::DLSS;
    } else if (shouldUseRayReconstruction() && m_common->metaRayReconstruction().isActive()) {
      return InternalUpscaler::DLSS_RR;
    } else if (shouldUseXeSS() && m_common->metaXeSS().isActive()) {
      return InternalUpscaler::XeSS;
    } else if (shouldUseNIS()) {
      return InternalUpscaler::NIS;
    } else if (shouldUseTAA()) {
      return InternalUpscaler::TAAU;
    } else {
      return InternalUpscaler::None;
    }
  }

  VkExtent3D RtxContext::onInjectRtxFrameBegin(const VkExtent3D& upscaledExtent) {
    auto logRenderPassRaytraceModeRayQuery = [=](const char* renderPassName, auto mode) {
      switch (mode) {
      case decltype(mode)::RayQuery:
        Logger::info(str::format("RenderPass ", renderPassName, " Raytrace Mode: Ray Query (CS)"));
        break;
      case decltype(mode)::RayQueryRayGen:
        Logger::info(str::format("RenderPass ", renderPassName, " Raytrace Mode: Ray Query (RGS)"));
        break;
      default: 
        assert(false && "invalid RaytraceMode in logRenderPassRaytraceModeRayQuery");
        break;
      }
    };

    auto logRenderPassRaytraceMode = [=](const char* renderPassName, auto mode) {
      switch (mode) {
      case decltype(mode)::RayQuery:
      case decltype(mode)::RayQueryRayGen:
        logRenderPassRaytraceModeRayQuery(renderPassName, mode);
        break;
      case decltype(mode)::TraceRay:
        Logger::info(str::format("RenderPass ", renderPassName, " Raytrace Mode: Trace Ray (RGS)"));
        break;
      case decltype(mode)::Count:
        assert(false && "invalid RaytraceMode in logRenderPassRaytraceMode");
        break;
      }
    };

    // Log used raytracing mode
    static RenderPassGBufferRaytraceMode sPrevRenderPassGBufferRaytraceMode = RenderPassGBufferRaytraceMode::Count;
    static RenderPassIntegrateDirectRaytraceMode sPrevRenderPassIntegrateDirectRaytraceMode = RenderPassIntegrateDirectRaytraceMode::Count;
    static RenderPassIntegrateIndirectRaytraceMode sPrevRenderPassIntegrateIndirectRaytraceMode = RenderPassIntegrateIndirectRaytraceMode::Count;
    static UpscalerType sPrevUpscalerType = UpscalerType::None;
    static uint32_t sPrevKenshiTerrainSecondaryShadingMode = UINT32_MAX;

    if (sPrevRenderPassGBufferRaytraceMode != RtxOptions::renderPassGBufferRaytraceMode() ||
        sPrevRenderPassIntegrateDirectRaytraceMode != RtxOptions::renderPassIntegrateDirectRaytraceMode() ||
        sPrevRenderPassIntegrateIndirectRaytraceMode != RtxOptions::renderPassIntegrateIndirectRaytraceMode() ||
        sPrevUpscalerType != RtxOptions::upscalerType()) {

      sPrevRenderPassGBufferRaytraceMode = RtxOptions::renderPassGBufferRaytraceMode();
      sPrevRenderPassIntegrateDirectRaytraceMode = RtxOptions::renderPassIntegrateDirectRaytraceMode();
      sPrevRenderPassIntegrateIndirectRaytraceMode = RtxOptions::renderPassIntegrateIndirectRaytraceMode();
      sPrevUpscalerType = RtxOptions::upscalerType();

      logRenderPassRaytraceMode("GBuffer", RtxOptions::renderPassGBufferRaytraceMode());
      logRenderPassRaytraceModeRayQuery("Integrate Direct", RtxOptions::renderPassIntegrateDirectRaytraceMode());
      logRenderPassRaytraceMode("Integrate Indirect", RtxOptions::renderPassIntegrateIndirectRaytraceMode());

      m_resetHistory = true;
    }
    // Secondary terrain material evaluation changes the indirect radiance
    // signal, so discard temporal data once when the live comparison control
    // changes.
    if (sPrevKenshiTerrainSecondaryShadingMode != KenshiTerrainOptions::secondaryShadingMode()) {
      sPrevKenshiTerrainSecondaryShadingMode = KenshiTerrainOptions::secondaryShadingMode();
      m_resetHistory = true;
    }

    // Calculate extents based on if DLSS is enabled or not
    VkExtent3D downscaledExtent = setDownscaleExtent(upscaledExtent);

    if (!getResourceManager().validateRaytracingOutput(downscaledExtent, upscaledExtent)) {
      Logger::debug("Raytracing output resources were not available to use this frame, so we must re-create inline.");

      resetScreenResolution(upscaledExtent);
    }

    const RtCamera& mainCamera = getSceneManager().getCamera();

    // Call onFrameBegin callbacks for RtxPases
    // Note: this needs to be called after resetScreenResolution() call in a frame
    // since an RtxPass may alias some of its resources with the ones created in createRaytracingOutput()
    getResourceManager().onFrameBegin(this, getCommonObjects()->getTextureManager(), getSceneManager(), downscaledExtent,
                                      upscaledExtent, m_resetHistory, mainCamera.isCameraCut());

    // Force history reset on integrate indirect mode change to discard incompatible history 
    if (RtxOptions::integrateIndirectMode() != m_prevIntegrateIndirectMode) {
      m_resetHistory = true;
      m_prevIntegrateIndirectMode = RtxOptions::integrateIndirectMode();
    }

    if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache &&
        m_common->metaNeuralRadianceCache().isResettingHistory()) {
      m_resetHistory = true;
    }

    // Release resources when switching upscalers
    m_currentUpscaler = getCurrentFrameUpscaler();
    if (m_currentUpscaler != m_previousUpscaler) {
      // Need to wait before the previous frame is executed.
      getDevice()->waitForIdle();

      // Release resources
      if (m_previousUpscaler == InternalUpscaler::DLSS_RR) {
        DxvkRayReconstruction& rayReconstruction = m_common->metaRayReconstruction();
        rayReconstruction.release();
      } else if (m_previousUpscaler == InternalUpscaler::DLSS) {
        DxvkDLSS& dlss = m_common->metaDLSS();
        dlss.release();
      }
    }

    return downscaledExtent;
  }

  void RtxContext::onInjectRtxFrameEnd(bool rayTracedThisFrame) {
    if (m_kenshiSignOverlay && (!rayTracedThisFrame
        || m_kenshiSignOverlay->frame != m_device->getCurrentFrameId()))
      m_kenshiSignOverlay.reset();
    if (rayTracedThisFrame) {
      Resources::RaytracingOutput& rtOutput = getResourceManager().getRaytracingOutput();

      m_common->metaNeuralRadianceCache().onFrameEnd(rtOutput);
      rtOutput.onFrameEnd();
    }

    getSceneManager().onFrameEnd(this, rayTracedThisFrame);
  }

  // Hooked into D3D11 presentImage (same place HUD rendering is)
  void RtxContext::injectRTX(std::uint64_t cachedReflexFrameId, Rc<DxvkImage> targetImage) {
    if (!kenshi_telemetry::enabled()) {
      s_triggerDebugScreenshot = false;
      s_gbufferBurstFramesRemaining = 0;
      s_gpuPrintWindowFramesRemaining = 0;
    }
    ScopedCpuProfileZone();
#ifdef REMIX_DEVELOPMENT
    m_currentPassStage = RtxFramePassStage::FrameBegin;
#endif

    if (RtxOptions::enableBreakIntoDebuggerOnPressingB() && ImGUI::checkHotkeyState({VirtualKey{ 'B' }}, true)) {
      while (!::IsDebuggerPresent()) {
        ::Sleep(100);
      }
      __debugbreak();
    }

#ifdef REMIX_DEVELOPMENT
    // Crash Hotkey Feature: When armed via the Development tab checkbox, pressing the crash hotkey
    // triggers a deliberate null pointer dereference crash. This is useful for testing crash handling,
    // crash dumps, and crash reporting systems.
    {
      static bool crashHotkeyStartupLogged = false;
      if (!crashHotkeyStartupLogged && RtxOptions::enableCrashHotkey()) {
        const auto crashHotkeyStr = buildKeyBindDescriptorString(RtxOptions::crashHotkey());
        Logger::warn(str::format("Crash hotkey is ARMED at startup (via config/environment) - press ", crashHotkeyStr, " to trigger crash"));
        crashHotkeyStartupLogged = true;
      }
      
      if (RtxOptions::enableCrashHotkey() && ImGUI::checkHotkeyState(RtxOptions::crashHotkey(), false)) {
        const auto crashHotkeyStr = buildKeyBindDescriptorString(RtxOptions::crashHotkey());
        Logger::err(str::format("Deliberate crash triggered via crash hotkey (", crashHotkeyStr, ")"));
        // Trigger a null pointer dereference to cause a crash
        volatile int* nullPtr = nullptr;
        *nullPtr = 0xDEAD;
      }
    }
#endif

    commitGraphicsState<true, false>();

    auto common = getCommonObjects();
    const auto isRaytracingEnabled = RtxOptions::enableRaytracing();
    const auto asyncShaderCompilationActive = RtxOptions::Shader::enableAsyncCompilation() && common->pipelineManager().remixShaderCompilationCount() > 0;

    // Determine and set present throttle delay
    // Note: This must be done before the early out returns below which is why some logic here is redundant (e.g. checking if ray tracing is supported again)
    // just to ensure the present throttle delay is always being set properly.

    const auto requestedPresentThrottleDelay = RtxOptions::enablePresentThrottle() ? RtxOptions::presentThrottleDelay() : 0;
    std::uint32_t requestedAsyncShaderCompilationDelay = 0U;

    // Note: Only use the async shader compilation throttle delay when rendering which uses Remix shaders would actually take place. As such this delay is not
    // needed when ray tracing is not supported or enabled as Remix shaders will not be used in that case.
    if (m_rayTracingSupported && isRaytracingEnabled && asyncShaderCompilationActive) {
      requestedAsyncShaderCompilationDelay = RtxOptions::Shader::asyncCompilationThrottleMilliseconds();
    }

    // Note: Determine the throttle delay to use based on the larger of the two requested delay values as the larger should satisfy the requests of both.
    // A sum is also potentially a valid way of going about this, but a maximum makes more sense in that these delays aren't expected to stack but rather
    // are just requests for some minimum amount of time to spend waiting per frame.
    const auto computedPresentThrottleDelay = std::max(requestedPresentThrottleDelay, requestedAsyncShaderCompilationDelay);

    m_device->setPresentThrottleDelay(computedPresentThrottleDelay);

    // Early out if ray tracing is not supported or if Remix has already been injected

    if (!m_rayTracingSupported) {
      ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Raytracing doesn't appear to be supported on this HW.")));
      return;
    }

    if (m_frameLastInjected == m_device->getCurrentFrameId()) {
      return;
    }

    const uint32_t currentFrameId = m_device->getCurrentFrameId();
    const bool logRaytracerFrame = s_triggerDebugScreenshot || (terrain_profile::diagnosticsEnabled() && (
      m_lastRaytracerDiagnosticFrame == kInvalidFrameIndex ||
      currentFrameId - m_lastRaytracerDiagnosticFrame >= 120u));

    if (logRaytracerFrame)
      m_lastRaytracerDiagnosticFrame = currentFrameId;

    const bool isCameraValid = getSceneManager().getCamera().isValid(currentFrameId);
    if (!isCameraValid) {
      ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Trying to raytrace but not detecting a valid camera.")));
    }

    if (logRaytracerFrame) {
      const auto& sceneManager = getSceneManager();
      const auto& camera = sceneManager.getCamera();
      Logger::info(str::format(
        "[Remix-RayTracer] frame=", currentFrameId,
        " decision enabled=", isRaytracingEnabled,
        " hwSupported=", m_rayTracingSupported,
        " cameraValid=", isCameraValid,
        " cameraLastUpdate=", camera.getLastUpdateFrame(),
        " shadersPending=", common->pipelineManager().remixShaderCompilationCount(),
        " surfaceBuffer=", sceneManager.getSurfaceBuffer() != nullptr,
        " surfaces=", sceneManager.getAccelManager().getSurfaceCount(),
        " instances=", sceneManager.getInstanceManager().getActiveCount(),
        " lights=", sceneManager.getLightManager().getActiveCount(),
        " target=", describeRaytracerImage(targetImage)));

      if (isCameraValid) {
        const auto& projection = camera.getViewToProjection();
        bool projectionFinite = true;
        for (uint32_t row = 0; row < 4; row++) {
          for (uint32_t column = 0; column < 4; column++)
            projectionFinite &= std::isfinite(projection[row][column]);
        }

        Logger::info(str::format(
          "[Remix-RayTracer] frame=", currentFrameId,
          " camera near=", camera.getNearPlane(),
          " far=", camera.getFarPlane(),
          " projectionFinite=", projectionFinite,
          " projectionDiag=", projection[0][0], ",", projection[1][1], ",",
          projection[2][2], ",", projection[3][3],
          " projectionZW=", projection[2][3], ",", projection[3][2]));
      }
    }

    // Update frame counter only after actual rendering
    if (isCameraValid) {
      m_frameLastInjected = m_device->getCurrentFrameId();
    }

    if (RtxOptions::upscalerType() == UpscalerType::DLSS && !common->metaDLSS().supportsDLSS()) {
      RtxOptions::upscalerType.setDeferred(UpscalerType::TAAU);
    }

    if (DxvkDLFG::enable() && !common->metaDLFG().supportsDLFG()) {
      DxvkDLFG::enable.setDeferred(false);
    }
    
#ifdef REMIX_DEVELOPMENT
    // Update the Shader Manager

    ShaderManager::getInstance()->update();
#endif

    common->getTextureManager().processAllHotReloadRequests();

    const float gpuIdleTimeMilliseconds = getGpuIdleTimeSinceLastCall();
    Metrics::TestTraceSample testTraceSample;
    testTraceSample.frameId = m_device->getCurrentFrameId();
    testTraceSample.effectiveDeltaMs = GlobalTime::get().deltaTimeMs();
    testTraceSample.realWallDeltaMs = GlobalTime::get().realDeltaTimeMs();
    testTraceSample.gpuIdleTimeMs = gpuIdleTimeMilliseconds;
    testTraceSample.surfaceCount = getSceneManager().getAccelManager().getSurfaceCount();
    testTraceSample.shaderCompileInflightCount = getCommonObjects()->pipelineManager().remixShaderCompilationCount();
    testTraceSample.debugViewMode = m_common->metaDebugView().getDebugViewIndex();
    testTraceSample.compositeDebugViewMode = m_common->metaDebugView().getCompositeDebugViewIndex();
    testTraceSample.raytracingEnabled = isRaytracingEnabled;
    testTraceSample.cameraValid = isCameraValid;
    testTraceSample.asyncShaderPrewarming = RtxInitializer::asyncShaderPrewarming();
    testTraceSample.asyncCompilationEnabled = RtxOptions::Shader::enableAsyncCompilation();
    testTraceSample.asyncCompilationActive = asyncShaderCompilationActive;
    testTraceSample.surfaceBufferAvailable = getSceneManager().getSurfaceBuffer() != nullptr;
    Metrics::recordTestTrace(testTraceSample);

    bool raytracedThisFrame = false;

    // Note: Only engage ray tracing when it is enabled, the camera is valid and when no shaders are currently being compiled asynchronously (as
    // trying to render before shaders are done compiling will cause Remix to block).
    if (isRaytracingEnabled && isCameraValid && !asyncShaderCompilationActive) {
      if (targetImage == nullptr) {
        targetImage = m_state.om.renderTargets.color[0].view->image();  
      }

      const bool captureTestScreenshot = (m_screenshotFrameEnabled && m_device->getCurrentFrameId() == m_screenshotFrameNum);
      const bool captureScreenImage = s_triggerScreenshot || (captureTestScreenshot && !s_capturePrePresentTestScreenshot);
      const bool captureDebugImage = kenshi_telemetry::enabled() && (RtxOptions::captureDebugImage() || s_triggerDebugScreenshot);
      
      if (s_triggerUsdCapture) {
        s_triggerUsdCapture = false;
        m_common->capturer()->triggerNewCapture();
      }

      if (captureTestScreenshot) {
        Logger::info(str::format("RTX: Test screenshot capture triggered"));
        Logger::info(str::format("RTX: Use separate denoiser ", RtxOptions::denoiseDirectAndIndirectLightingSeparately()));
        Logger::info(str::format("RTX: Use rtxdi ", RtxOptions::useRTXDI()));
        Logger::info(str::format("RTX: Use dlss ", RtxOptions::isDLSSOrRayReconstructionEnabled()));
        Logger::info(str::format("RTX: Use ray reconstruction ", RtxOptions::isRayReconstructionEnabled()));
        Logger::info(str::format("RTX: Use nis ", RtxOptions::isNISEnabled()));
        if (!s_capturePrePresentTestScreenshot) {
          m_screenshotFrameEnabled = false;
          Metrics::setTestTraceScreenshotFrameEnabled(false);
        }
      }

      if (captureScreenImage && captureDebugImage) {
        takeScreenshot("orgImage", targetImage);
      }

      RtxParticleSystemManager& particles = m_device->getCommon()->metaParticleSystem();
      particles.submitDrawState(this);

      this->spillRenderPass(false);

      getCommonObjects()->getTextureManager().submitTexturesToDeviceLocal(this, m_execBarriers, m_execAcquires);

      m_execBarriers.recordCommands(m_cmd);

      ScopedGpuProfileZone(this, "InjectRTX");

      // Signal Reflex rendering start

      RtxReflex& reflex = m_common->metaReflex();

      // Note: Update the Reflex mode in case the option has changed.
      reflex.updateMode();

      m_submitContainsInjectRtx = true;
      m_cachedReflexFrameId = cachedReflexFrameId;

      // Update all the GPU buffers needed to describe the scene
      getSceneManager().prepareSceneData(this, m_execBarriers);
      
      // If we really don't have any RT to do, just bail early (could be UI/menus rendering)
      if (getSceneManager().getSurfaceBuffer() != nullptr) {

        VkExtent3D downscaledExtent = onInjectRtxFrameBegin(targetImage->info().extent);

        Resources::RaytracingOutput& rtOutput = getResourceManager().getRaytracingOutput();

        if (common->metaNGXContext().supportsDLFG()) {
          rtOutput.m_primaryDepthQueue.next();
          rtOutput.m_primaryScreenSpaceMotionVectorQueue.next();
        }

        rtOutput.m_primaryDepth = rtOutput.m_primaryDepthQueue.get();
        rtOutput.m_primaryScreenSpaceMotionVector = rtOutput.m_primaryScreenSpaceMotionVectorQueue.get();

        getCommonObjects()->getTextureManager().prepareSamplerFeedback(this);

        // Generate ray tracing constant buffer
        const uint32_t materialProbeTicket = kenshi_material_probe::active(currentFrameId)
          ? kenshi_material_probe::ticket : 0u;
        if (materialProbeTicket && !rtOutput.m_primaryObjectPicking.isValid()) {
          auto materialProbeContext = Rc<DxvkContext>{this};
          rtOutput.m_primaryObjectPicking = Resources::createImageResource(
            materialProbeContext, "material probe object picking", downscaledExtent, VK_FORMAT_R32_UINT);
        }
        updateRaytraceArgsConstantBuffer(rtOutput, downscaledExtent, targetImage->info().extent);

        if (logRaytracerFrame) {
          Logger::info(str::format(
            "[Remix-RayTracer] frame=", currentFrameId,
            " begin renderExtent=", downscaledExtent.width, "x", downscaledExtent.height,
            " outputExtent=", targetImage->info().extent.width, "x", targetImage->info().extent.height,
            " outputReady=", rtOutput.isReady(),
            " albedo=", describeRaytracerImage(rtOutput.m_primaryAlbedo.image),
            " normal=", describeRaytracerImage(rtOutput.m_primaryWorldShadingNormal.image),
            " linearZ=", describeRaytracerImage(rtOutput.m_primaryLinearViewZ.image)));
        }

        // Volumetric Lighting
        dispatchVolumetrics(rtOutput);
        
        // Path Tracing
        dispatchPathTracing(rtOutput);
        if (materialProbeTicket) {
          const std::string prefix = str::format("materialProbe-", materialProbeTicket, "-");
          takeScreenshot(prefix + "albedo", rtOutput.m_primaryAlbedo.image);
          takeScreenshot(prefix + "picking", rtOutput.m_primaryObjectPicking.image);
          takeScreenshot(prefix + "worldPosition", rtOutput.getCurrentPrimaryWorldPositionWorldTriangleNormal().image(Resources::AccessType::Read));
          KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] rendered ticket=", materialProbeTicket,
            " frame=", currentFrameId, " sceneRows=", kenshi_material_probe::sceneRows,
            " sceneSuppressed=", kenshi_material_probe::sceneSuppressed));
          kenshi_material_probe::ticket = 0;
        }

        if (logRaytracerFrame) {
          Logger::info(str::format(
            "[Remix-RayTracer] frame=", currentFrameId,
            " pathTraceDone albedo=", describeRaytracerImage(rtOutput.m_primaryAlbedo.image),
            " normal=", describeRaytracerImage(rtOutput.m_primaryWorldShadingNormal.image),
            " linearZ=", describeRaytracerImage(rtOutput.m_primaryLinearViewZ.image),
            " directDiffuse=", describeRaytracerImage(rtOutput.m_primaryDirectDiffuseRadiance.image(Resources::AccessType::Read)),
            " directSpecular=", describeRaytracerImage(rtOutput.m_primaryDirectSpecularRadiance.image(Resources::AccessType::Read))));
        }

        // Neural Radiance Cache
        m_common->metaNeuralRadianceCache().dispatchTrainingAndResolve(*this, rtOutput);

        // RTXDI confidence
        m_common->metaRtxdiRayQuery().dispatchConfidence(this, rtOutput);

        // ReSTIR GI
        m_common->metaReSTIRGIRayQuery().dispatch(this, rtOutput);
        
        if (captureScreenImage && captureDebugImage) {
          takeScreenshot("baseReflectivity", rtOutput.m_primaryBaseReflectivity.image(Resources::AccessType::Read));
          takeScreenshot("sharedSubsurfaceData", rtOutput.m_sharedSubsurfaceData.image);
          takeScreenshot("sharedSubsurfaceDiffusionProfileData", rtOutput.m_sharedSubsurfaceDiffusionProfileData.image);
        }

        // Demodulation
        dispatchDemodulate(rtOutput);

        if (logRaytracerFrame) {
          Logger::info(str::format(
            "[Remix-RayTracer] frame=", currentFrameId,
            " demodulateDone diffuse=", describeRaytracerImage(rtOutput.m_primaryDirectDiffuseRadiance.image(Resources::AccessType::Read)),
            " specular=", describeRaytracerImage(rtOutput.m_primaryDirectSpecularRadiance.image(Resources::AccessType::Read))));
        }

        // Note: Primary direct diffuse/specular radiance textures noisy and in a demodulated state after demodulation step.
        if (captureScreenImage && captureDebugImage) {
          takeScreenshot("noisyDiffuse", rtOutput.m_primaryDirectDiffuseRadiance.image(Resources::AccessType::Read));
          takeScreenshot("noisySpecular", rtOutput.m_primaryDirectSpecularRadiance.image(Resources::AccessType::Read));
        }

        // Denoising
        dispatchDenoise(rtOutput);

        if (logRaytracerFrame) {
          Logger::info(str::format(
            "[Remix-RayTracer] frame=", currentFrameId,
            " denoiseDone enabled=", RtxOptions::useDenoiser(),
            " referenceMode=", RtxOptions::useDenoiserReferenceMode(),
            " diffuse=", describeRaytracerImage(rtOutput.m_primaryDirectDiffuseRadiance.image(Resources::AccessType::Read)),
            " specular=", describeRaytracerImage(rtOutput.m_primaryDirectSpecularRadiance.image(Resources::AccessType::Read))));
        }

        // Note: Primary direct diffuse/specular radiance textures denoised but in a still demodulated state after denoising step.
        if (captureScreenImage && captureDebugImage) {
          takeScreenshot("denoisedDiffuse", rtOutput.m_primaryDirectDiffuseRadiance.image(Resources::AccessType::Read));
          takeScreenshot("denoisedSpecular", rtOutput.m_primaryDirectSpecularRadiance.image(Resources::AccessType::Read));
        }

        // Composition
        if (captureScreenImage && captureDebugImage) {
          takeScreenshot("lightingAlbedo", rtOutput.m_primaryAlbedo.image);
          takeScreenshot("lightingNormalRoughness", rtOutput.m_primaryVirtualWorldShadingNormalPerceptualRoughness.image);
        }
        dispatchComposite(rtOutput, captureScreenImage && captureDebugImage);
        if (captureScreenImage && captureDebugImage) {
          takeScreenshot("lightingPostComposite", rtOutput.m_compositeOutput.resource(Resources::AccessType::Read).image);
        }

        if (logRaytracerFrame) {
          Logger::info(str::format(
            "[Remix-RayTracer] frame=", currentFrameId,
            " compositeDone image=", describeRaytracerImage(rtOutput.m_compositeOutput.resource(Resources::AccessType::Read).image),
            " extent=", rtOutput.m_compositeOutputExtent.width, "x", rtOutput.m_compositeOutputExtent.height));
        }

        // Post composite Debug View that may overwrite Composite output
        dispatchReplaceCompositeWithDebugView(rtOutput);
        
        if (captureScreenImage && captureDebugImage) {
          takeScreenshot("rtxImagePostComposite", rtOutput.m_compositeOutput.resource(Resources::AccessType::Read).image);
        }

        getCommonObjects()->getTextureManager().copySamplerFeedbackToHost(this);
        dispatchObjectPicking(rtOutput, downscaledExtent, targetImage->info().extent);

        // Upscaling if DLSS/NIS enabled, or the Composition Pass will do upscaling
        if (m_currentUpscaler == InternalUpscaler::DLSS) {
          // xxxnsubtil: the DLSS indicator reads our exposure texture even with DLSS autoexposure on
          // make sure it has been created, otherwise we run into trouble on the first frame
          m_common->metaAutoExposure().createResources(this);
          dispatchDLSS(rtOutput);
        } else if (m_currentUpscaler == InternalUpscaler::DLSS_RR) {
          m_common->metaAutoExposure().createResources(this);
          dispatchRayReconstruction(rtOutput);
        } else if (m_currentUpscaler == InternalUpscaler::XeSS) {
          m_common->metaAutoExposure().createResources(this);
          dispatchXeSS(rtOutput);
        } else if (m_currentUpscaler == InternalUpscaler::NIS) {
          dispatchNIS(rtOutput);
        } else if (m_currentUpscaler == InternalUpscaler::TAAU){
          dispatchTemporalAA(rtOutput);
        } else {
          copyImage(
            rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            { 0, 0, 0 },
            rtOutput.m_compositeOutput.image(Resources::AccessType::Read),
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            { 0, 0, 0 },
            rtOutput.m_compositeOutputExtent);
        }
        m_previousUpscaler = m_currentUpscaler;

        RtxDustParticles& dust = m_common->metaDustParticles();
        dust.simulateAndDraw(this, m_state, rtOutput);

        dispatchBloom(rtOutput);
        dispatchPostFx(rtOutput);

        // Tone mapping
        // Lighting capture here: after RR/upscaling, dust, bloom and postfx, immediately before exposure/tone
        // mapping. Surface-lobe captures are pre-RR.
        if (captureScreenImage && captureDebugImage) {
          takeScreenshot("lightingPreTonemap", rtOutput.m_finalOutput.resource(Resources::AccessType::Read).image);
          KENSHI_DIAGNOSTIC_INFO(str::format("[LightingCapture V791] frame=", m_device->getCurrentFrameId(),
            " stage=pre-tonemap rr=", useRayReconstruction(),
            " tonemappingMode=", static_cast<int>(RtxOptions::tonemappingMode()),
            " userEV=", RtxOptions::calcUserEVBias(),
            " autoExposure=", m_common->metaAutoExposure().enabled(),
            " localExposure=", m_common->metaLocalToneMapping().exposure(),
            " localShadows=", m_common->metaLocalToneMapping().shadows(),
            " localHighlights=", m_common->metaLocalToneMapping().highlights(),
            " localFinalizeACES=", m_common->metaLocalToneMapping().finalizeWithACES(),
            " postTonemapDDS=linear-display-rgb (apply-sRGB-for-viewing)"));
        }
        // WAR for TREX-553 - disable sRGB conversion as NVTT implicitly applies it during dds->png
        // conversion for 16bit float formats
        const bool performSRGBConversion = !captureScreenImage && g_allowSrgbConversionForOutput;
        dispatchToneMapping(rtOutput, performSRGBConversion);
        // A readability overlay: applying the scene's exposure, bloom and tone curve to its blend can wash out
        // yellow and text. Blend the authored display colour/opacity over the finished image.
        compositeKenshiSignOverlay(rtOutput, performSRGBConversion);
        if (materialProbeTicket) {
          takeScreenshot(str::format("materialProbe-", materialProbeTicket, "-display"),
            rtOutput.m_finalOutput.resource(Resources::AccessType::Read).image);
        }

        if (captureScreenImage) {
          if (m_common->metaDebugView().debugViewIdx() == DEBUG_VIEW_DISABLED) {
            takeScreenshot("rtxImagePostTonemapping", rtOutput.m_finalOutput.resource(Resources::AccessType::Read).image);
          }
          
          if (captureDebugImage) {
            takeScreenshot("albedo", rtOutput.m_primaryAlbedo.image);
            takeScreenshot("worldNormals", rtOutput.m_primaryWorldShadingNormal.image);
            takeScreenshot("worldMotion", rtOutput.m_primaryVirtualMotionVector.image(Resources::AccessType::Read));
            takeScreenshot("screenMotion", rtOutput.m_primaryScreenSpaceMotionVector.image);
            takeScreenshot("linearZ", rtOutput.m_primaryLinearViewZ.image);
          }
        }

        // Dump primary linear Z for a run of consecutive frames: if a building is present in frame N and absent
        // in N+1, the flicker is at or before the G-buffer; if every frame is identical, it is after it.
        // linearZ only: the cleanest hit/miss signal, one small image per frame, and coverage per frame reduces
        // to one number. Exactly one of this block and the debug-view burst below consumes a frame of the
        // budget.
        if (kenshi_telemetry::enabled() && s_gbufferBurstFramesRemaining.load(std::memory_order_relaxed) > 0u
         && m_common->metaDebugView().debugViewIdx() == DEBUG_VIEW_DISABLED) {
          s_gbufferBurstFramesRemaining.fetch_sub(1u, std::memory_order_relaxed);
          takeScreenshot("burstZ", rtOutput.m_primaryLinearViewZ.image);
        }

        // Set up output src
        Rc<DxvkImage> srcImage = rtOutput.m_finalOutput.resource(Resources::AccessType::Read).image;

        // Debug view
        dispatchDebugView(srcImage, rtOutput, captureScreenImage);

        // When a debug view is active, dump it for the burst instead of linearZ. With the Geometry Hash view
        // (277), a surface whose colour is constant frame to frame has a stable geometry hash; the mean
        // per-pixel colour delta between consecutive frames measures that numerically.
        if (kenshi_telemetry::enabled() && s_gbufferBurstFramesRemaining.load(std::memory_order_relaxed) > 0u
         && m_common->metaDebugView().debugViewIdx() != DEBUG_VIEW_DISABLED) {
          s_gbufferBurstFramesRemaining.fetch_sub(1u, std::memory_order_relaxed);
          takeScreenshot("burstDV", srcImage);
        }

        if (logRaytracerFrame) {
          Logger::info(str::format(
            "[Remix-RayTracer] frame=", currentFrameId,
            " final image=", describeRaytracerImage(srcImage),
            " upscaler=", static_cast<uint32_t>(m_currentUpscaler),
            " debugView=", m_common->metaDebugView().getDebugViewIndex(),
            " captureDebug=", captureDebugImage));
        }

        dispatchDLFG();

        // Blit to the game target
        {
          ScopedGpuProfileZone(this, "Blit to Game");
          
          // Note: the resolution between srcImage and dstImage always matches
          // so we can use the same blit with nearest neighbor filtering
          assert(srcImage->info().extent == targetImage->info().extent);
          blitImageHelper(this, srcImage, targetImage, VkFilter::VK_FILTER_NEAREST);
        }

        // Log stats when an image is taken
        if (captureScreenImage) {
          getSceneManager().logStatistics();
        }

        raytracedThisFrame = true;
      } else if (logRaytracerFrame) {
        Logger::warn(str::format(
          "[Remix-RayTracer] frame=", currentFrameId,
          " skipped: scene preparation produced no surface buffer (instances=",
          getSceneManager().getInstanceManager().getActiveCount(), ")"));
      }

      m_framesWithoutValidScene = 0;
    } else {
      // If raytracing is only disabled because we don't have shaders available, we don't want to clear the scene.
      // This frequently happens for a single frame when a cached shader is being fetched, and causes the Logic 
      // graph state to be reset - which is problematic since Logic graphs often trigger shader fetches.
      // It might be safe to remove this clear entirely - it was added before we had any garbage collection
      // in the scene manager, so it may not be needed anymore.
      if (!isRaytracingEnabled || !isCameraValid) {
        m_framesWithoutValidScene++;
        // Some games may have invalid cameras for a brief period during camera cuts, but clearing the scene
        // during these cuts causes all textures to need to be reloaded, which is slow.
        if (m_framesWithoutValidScene > RtxOptions::sceneKeepAliveFrames()) {
          // Only perform Wait For Idle on the first clear to avoid expensive GPU sync on every frame
          const bool needWfi = (m_framesWithoutValidScene == RtxOptions::sceneKeepAliveFrames() + 1);
          getSceneManager().clear(this, needWfi);
        }
      } else {
        m_framesWithoutValidScene = 0;
      }
    }

    onInjectRtxFrameEnd(raytracedThisFrame);

    // apply changes to RtxOptions after the frame has ended
    RtxOptionManager::applyPendingValues(m_device.ptr(), /* forceOnChange */ false);

    // Update stats
    updateMetrics(gpuIdleTimeMilliseconds);

    m_resetHistory = false;
  }

void RtxContext::endFrame(std::uint64_t cachedReflexFrameId, Rc<DxvkImage> targetImage, bool callInjectRtx) {

    // Start the performance debug frame timer
    PerfDebug_BeginFrame();

    if (callInjectRtx) {
      // Wrap the main RTX injection pass (Path Tracing, Denoising, Post-Processing)
      if (!PerfDebug_IsFeatureDisabled(FEATURE_RAYTRACING)) {
        PerfDebug_BeginFeature(FEATURE_RAYTRACING);
        
        // Fallback inject (is a no-op if already injected this frame, or no valid RT scene)
        injectRTX(cachedReflexFrameId, targetImage);
        
        PerfDebug_EndFeature(FEATURE_RAYTRACING);
      }
    } else if (m_frameLastInjected != m_device->getCurrentFrameId()) {
      // A raster pass-through frame still submitted candidate geometry to the
      // Remix scene before its UI/loading classification was known. It must
      // therefore finalize the non-rendered RT frame as well. Without this,
      // SceneManager::onFrameEnd never clears the per-frame bindless buffer
      // table; Unreal menu/scene transitions accumulated it to the 65,525
      // entry limit, then prepared an invalid/empty surface set and lost the
      // Vulkan device while UE's render thread waited indefinitely.
      const uint32_t transientBufferCount =
        terrain_profile::diagnosticsEnabled()
          ? static_cast<uint32_t>(getSceneManager().getBufferTable().size()) : 0u;
      static uint32_t sRasterPassThroughFinalizeLogCount = 0;
      if (transientBufferCount > 0u && sRasterPassThroughFinalizeLogCount < 16u) {
        ++sRasterPassThroughFinalizeLogCount;
        Logger::info(str::format(
          "[Remix-RayTracer] finalizing raster pass-through frame without injection: frame=",
          m_device->getCurrentFrameId(),
          " transientBuffers=", transientBufferCount));
      }
      onInjectRtxFrameEnd(false);
    }

    // End the performance debug frame timer and write to log
    PerfDebug_EndFrame();

#ifdef REMIX_DEVELOPMENT
    queryAvailableResourceAliasing();
    analyzeResourceAliasing();
    clearResourceAliasingCache();
#endif

    // Update time on the frame end so all other systems can benefit from a global time
    GlobalTime::get().update();
}

  // Called right before D3D11 present
  void RtxContext::onPresent(Rc<DxvkImage> targetImage) {
    // If injectRTX couldn't screenshot a final image or a pre-present screenshot is requested,
    // take a screenshot of a present image (with UI and others)
    {
      const bool isRaytracingEnabled = RtxOptions::enableRaytracing();
      const bool isCameraValid = getSceneManager().getCamera().isValid(m_device->getCurrentFrameId());

      if (!isRaytracingEnabled || !isCameraValid || s_capturePrePresentTestScreenshot) {
        const bool captureTestScreenshot = (m_screenshotFrameEnabled && m_device->getCurrentFrameId() == m_screenshotFrameNum);
        const bool captureDxvkScreenImage = s_triggerScreenshot || captureTestScreenshot;
        if (captureDxvkScreenImage) {
          if (targetImage == nullptr) {
            targetImage = m_state.om.renderTargets.color[0].view->image();
          }
          takeScreenshot("rtxImageDxvkView", targetImage);
        }
      }
    }
    s_triggerScreenshot = false;
    s_triggerDebugScreenshot = false;

    // Some time in the future kill process
    if (m_triggerDelayedTerminate &&
        (m_device->getCurrentFrameId() > m_terminateAppFrameNum) &&
        m_common->capturer()->isIdle()) {
      Logger::info(str::format("RTX: Terminating application"));
      Metrics::serialize();
      getCommonObjects()->metaExporter().waitForAllExportsToComplete();

      env::killProcess();
    }

    // This needs to happen at the end of frame, after ImGUI rendering
    GpuMemoryTracker::onFrameEnd();
  }

  void RtxContext::updateMetrics(const float gpuIdleTimeMilliseconds) const {
    ScopedCpuProfileZone();
    Metrics::logRollingAverage(Metric::dxvk_average_frame_time_ms, GlobalTime::get().realDeltaTimeMs()); // In milliseconds
    Metrics::logRollingAverage(Metric::dxvk_gpu_idle_time_ms, gpuIdleTimeMilliseconds); // In milliseconds
    uint64_t vidUsageMib = 0;
    uint64_t sysUsageMib = 0;
    const VkPhysicalDeviceMemoryProperties memprops = m_device->adapter()->memoryProperties();
    // Calc memory usage
    for (uint32_t i = 0; i < memprops.memoryHeapCount; i++) {
      bool isDeviceLocal = memprops.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

      if (isDeviceLocal) {
        vidUsageMib += m_device->getMemoryStats(i).totalUsed() >> 20;
      }
      else {
        sysUsageMib += m_device->getMemoryStats(i).totalUsed() >> 20;
      }
    }
    Metrics::logRollingAverage(Metric::dxvk_vid_memory_usage_mb, static_cast<float>(vidUsageMib)); // In MB
    Metrics::logRollingAverage(Metric::dxvk_sys_memory_usage_mb, static_cast<float>(sysUsageMib)); // In MB
    Metrics::logFloat(Metric::dxvk_total_time_ms, static_cast<float>(GlobalTime::get().realTimeSinceStartMs()));
    Metrics::logFloat(Metric::dxvk_frame_count, static_cast<float>(m_device->getCurrentFrameId()));
  }

  void RtxContext::setConstantBuffers(const uint32_t vsFixedFunctionConstants, const uint32_t psSharedStateConstants, Rc<DxvkBuffer> vertexCaptureCB) {
    m_rtState.vsFixedFunctionCB = m_rc[vsFixedFunctionConstants].bufferSlice.buffer();
    m_rtState.psSharedStateCB = m_rc[psSharedStateConstants].bufferSlice.buffer();
    m_rtState.vertexCaptureCB = vertexCaptureCB;
  }

  void RtxContext::addLights(const Dx11LightDesc* pLights, const uint32_t numLights) {
    for (uint32_t i = 0; i < numLights; i++) {
      getSceneManager().addLight(pLights[i]);
    }
  }

  std::vector<uint8_t> RtxContext::snapshotTerrainNativeState() const {
    std::vector<uint8_t> bytes;
    auto append = [&bytes](const auto& value) {
      const auto* first = reinterpret_cast<const uint8_t*>(&value);
      bytes.insert(bytes.end(), first, first + sizeof(value));
    };
    auto pointer = [&append](const auto& value) { append(reinterpret_cast<uintptr_t>(value.ptr())); };
    auto slice = [&append, &pointer](const DxvkBufferSlice& value) {
      pointer(value.buffer()); append(value.offset()); append(value.length());
    };
    pointer(m_state.gp.shaders.vs); pointer(m_state.gp.shaders.tcs);
    pointer(m_state.gp.shaders.tes); pointer(m_state.gp.shaders.gs);
    pointer(m_state.gp.shaders.fs); pointer(m_state.cp.shaders.cs);
    // Do not byte-copy the enclosing pointer-owning state. Packed state starts zeroed, but individual
    // attribute constructors leave reserved bits unset. setInputLayout stores a zero stride placeholder
    // that a graphics draw materializes from vi.vertexStrides, so compare the effective pipeline state on a
    // copy; an eager restore's cache invalidation is then not a false mismatch.
    auto graphicsState = m_state.gp.state;
    for (uint32_t i = 0; i < graphicsState.il.bindingCount(); ++i) {
      const uint32_t binding = graphicsState.ilBindings[i].binding();
      graphicsState.ilBindings[i].setStride(m_state.vi.vertexStrides[binding]);
    }
    // Compare vertex attribute semantics, excluding only reserved bits: DxvkIlAttribute's constructor does
    // not initialize its four reserved bits, so reapplying an identical input layout may change raw cache
    // bytes.
    std::array<VkVertexInputAttributeDescription, DxvkLimits::MaxNumVertexAttributes> attributes;
    for (size_t i = 0; i < attributes.size(); ++i)
      attributes[i] = graphicsState.ilAttributes[i].description();
    std::memset(graphicsState.ilAttributes, 0, sizeof(graphicsState.ilAttributes));
    append(graphicsState); append(m_state.cp.state);
    for (const auto& attribute : attributes) {
      append(attribute.location); append(attribute.binding);
      append(attribute.format); append(attribute.offset);
    }
    slice(m_state.id.argBuffer); slice(m_state.id.cntBuffer);
    slice(m_state.vi.indexBuffer); append(m_state.vi.indexType);
    for (size_t i = 0; i < m_state.vi.vertexBuffers.size(); ++i) {
      slice(m_state.vi.vertexBuffers[i]); append(m_state.vi.vertexStrides[i]);
    }
    for (size_t i = 0; i < m_state.xfb.buffers.size(); ++i) {
      slice(m_state.xfb.buffers[i]); slice(m_state.xfb.counters[i]);
    }
    pointer(m_state.om.renderTargets.depth.view);
    append(m_state.om.renderTargets.depth.layout);
    for (const auto& target : m_state.om.renderTargets.color) {
      pointer(target.view); append(target.layout);
    }
    for (const auto& viewport : m_state.vp.viewports) append(viewport);
    for (const auto& scissor : m_state.vp.scissorRects) append(scissor);
    append(m_state.dyn.blendConstants); append(m_state.dyn.depthBias);
    append(m_state.dyn.stencilReference);
    // Depth-bounds padding is not guaranteed initialized: serialize its fields.
    append(m_state.dyn.depthBounds.enableDepthBounds);
    append(m_state.dyn.depthBounds.minDepthBounds); append(m_state.dyn.depthBounds.maxDepthBounds);
    for (const auto& resource : m_rc) {
      pointer(resource.sampler); pointer(resource.imageView); pointer(resource.bufferView);
      slice(resource.bufferSlice);
    }
    return bytes;
  }

  void RtxContext::commitGeometryToRT(const DrawParameters& params, DrawCallState& drawCallState, bool geometryCacheOnlySceneSubmit,
                                    bool geometrySkyDisabledAtSubmission){
    ScopedCpuProfileZone();
    // Do not force pending geometry futures just for the entry-stage trace.
    const uint32_t auditLod = terrain_audit::family(drawCallState.programmableVertexShaderBytecodeHash);
    if (auditLod && terrain_audit::row(m_device->getCurrentFrameId(), 0))
      KENSHI_DIAGNOSTIC_INFO(str::format("[TerrainAudit V781] frame=", m_device->getCurrentFrameId(),
        " stage=0 draw=", drawCallState.drawCallID, " lod=", auditLod,
        " indices=", params.indexCount, " cacheOnly=", geometryCacheOnlySceneSubmit));

    RasterGeometry& geoData = drawCallState.geometryData;
    DrawCallTransforms& transformData = drawCallState.transformData;

    // Static geometry hash memoization can pre-populate hashes without a future.
    assert(geoData.futureGeometryHashes.valid() || geoData.hashes[HashComponents::VertexPosition] != kEmptyHash);
    assert(geoData.positionBuffer.defined());

    // DX11_V284: Force fusedWorldViewMode to None.
    // Fusing the world and view matrices bakes the camera into the geometry (objectToWorld),
    // which causes the "geometry follows player" bug and breaks ray tracing camera motion.
    // We must keep World and View matrices separate.
      const auto fusedMode = RtxOptions::fusedWorldViewMode();
    if (unlikely(fusedMode != FusedWorldViewMode::None)) {
      if (fusedMode == FusedWorldViewMode::View) {
        // Set World from WorldView transform
        transformData.objectToWorld = transformData.objectToView;
        // Set camera to identity
        transformData.worldToView = Matrix4();
      } else if (fusedMode == FusedWorldViewMode::World) {
        // Nothing to do...
      }
    }

    auto& cameraManager = getSceneManager().getCameraManager();

    // TODO: a last camera is used to finalize skinning...
    // processCameraData can be called only after finalizePendingFutures,
    // as we need geometry hash to check sky geometries
    const RtCamera* lastCamera =
      cameraManager.isCameraValid(cameraManager.getLastSetCameraType())
        ? &cameraManager.getCamera(cameraManager.getLastSetCameraType())
        : nullptr;

    // Log every term of the path a sky draw has to survive to reach the probe, at the gate itself.
    // finalizePendingFutures is hoisted into a variable so its result can be reported; it is still called
    // exactly once, in the same place.
    const bool skyGateDraw = drawCallState.testCategoryFlags(InstanceCategories::Sky);
    static uint32_t s_skyGateLogCount = 0;
    const bool skyGateLog = skyGateDraw && s_skyGateLogCount < 32u;
    if (skyGateLog) {
      ++s_skyGateLogCount;
    }

    // Sync any pending work with geometry processing threads
    terrain_profile::Scope terrainCpuFutures(terrain_profile::Stage::Futures);
    const bool skyGateFuturesResolved = drawCallState.finalizePendingFutures(lastCamera, geometrySkyDisabledAtSubmission);
    terrainCpuFutures.finish();

    if (skyGateLog) {
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[RTX Sky][gate] sky-flagged draw: finalizePendingFutures=",
        skyGateFuturesResolved ? 1 : 0,
        " skyMode=", uint32_t(RtxOptions::skyMode()),
        " (SkyboxRasterization=", uint32_t(SkyMode::SkyboxRasterization),
        " PhysicalAtmosphere=", uint32_t(SkyMode::PhysicalAtmosphere), ")",
        " probeUnavailableFlag=", m_skyProbeReprojectionUnavailable ? 1 : 0,
        " lastCamera=", lastCamera != nullptr ? 1 : 0,
        " indexCount=", params.indexCount, " vertexCount=", params.vertexCount));
    }

    if (skyGateFuturesResolved) {
      drawCallState.cameraType = cameraManager.processCameraData(drawCallState);

      if (skyGateLog) {
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[RTX Sky][gate] processCameraData -> cameraType=",
          uint32_t(drawCallState.cameraType),
          " (Sky=", uint32_t(CameraType::Sky),
          " Main=", uint32_t(CameraType::Main),
          " Unknown=", uint32_t(CameraType::Unknown), ")",
          " skyCategoryStillSet=",
          drawCallState.testCategoryFlags(InstanceCategories::Sky) ? 1 : 0,
          " skipObjectsWithUnknownCamera=",
          RtxOptions::skipObjectsWithUnknownCamera() ? 1 : 0));
      }

      if (drawCallState.cameraType == CameraType::Unknown) {
        if (RtxOptions::skipObjectsWithUnknownCamera()) {
          if (skyGateLog) {
            KENSHI_DIAGNOSTIC_INFO("[RTX Sky][gate] DROPPED: unknown camera and skipObjectsWithUnknownCamera is on");
          }
          return;
        }
        // fallback
        drawCallState.cameraType = CameraType::Enum::Main;
        if (skyGateLog) {
          KENSHI_DIAGNOSTIC_INFO("[RTX Sky][gate] unknown camera -> fell back to Main, so tryHandleSky will NOT treat it as sky");
        }
      }

      if (tryHandleSky(&params, &drawCallState) == TryHandleSkyResult::SkipSubmit) {
        if (skyGateLog) {
          KENSHI_DIAGNOSTIC_INFO("[RTX Sky][gate] tryHandleSky returned SkipSubmit");
        }
        return;
      }

      // Bake the terrain
      const MaterialData* overrideMaterialData = nullptr;
      bakeTerrain(params, drawCallState, &overrideMaterialData);

    
      // An attempt to resolve cases where games pre-combine view and world matrices
      if (RtxOptions::resolvePreCombinedMatrices() &&
        isIdentityExact(drawCallState.getTransformData().worldToView) &&
        !drawCallState.getTransformData().cameraRelativeView) {
        const auto* referenceCamera = &cameraManager.getCamera(drawCallState.cameraType);
        // Note: we may accept a data even from a prev frame, as we need any information to restore;
        // but if camera data is stale, it introduces an scene object transform's lag
        if (!referenceCamera->isValid(m_device->getCurrentFrameId()) &&
          !referenceCamera->isValid(m_device->getCurrentFrameId() - 1)) {
          referenceCamera = &cameraManager.getCamera(CameraType::Main);
        }
        transformData.objectToWorld = referenceCamera->getViewToWorld(false) * drawCallState.getTransformData().objectToView;
        transformData.worldToView = referenceCamera->getWorldToView(false);
      }
      
      // Apply free camera transform when view space texGenMode is used.
      // Note: TerrainBaking already applies this transform for TexGenMode::CascadedViewPositions 
      if ((transformData.texgenMode == TexGenMode::ViewPositions
           || transformData.texgenMode == TexGenMode::ViewNormals)
          && RtCamera::enableFreeCamera()) {
        if (cameraManager.isCameraValid(CameraType::Main)) {
          const RtCamera& camera = cameraManager.getMainCamera();
          // Revert the main camera's viewToWorld transform and then apply the free camera's one
          transformData.textureTransform *= camera.getViewToWorldToFreeCamViewToWorld();
        } else {
          ONCE(Logger::warn(str::format("[RTX] Tried to update surface transform with Free Camera's transform "
                                        "but main camera has not been processed this frame yet. Skipping the transform update")));
        }
      }

      getSceneManager().submitDrawState(this, drawCallState, overrideMaterialData, geometryCacheOnlySceneSubmit);
    }
  }

  void RtxContext::commitExternalGeometryToRT(ExternalDrawState&& state) {
    getSceneManager().submitExternalDraw(this, std::move(state));
  }

  static uint32_t jenkinsHash(uint32_t a) {
    // http://burtleburtle.net/bob/hash/integer.html
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
  }

  void RtxContext::getDenoiseArgs(NrdArgs& outPrimaryDirectNrdArgs, NrdArgs& outPrimaryIndirectNrdArgs, NrdArgs& outSecondaryNrdArgs) {
    const bool realtimeDenoiserEnabled = RtxOptions::useDenoiser() && !RtxOptions::useDenoiserReferenceMode();
    const bool separateDenoiserEnabled = RtxOptions::denoiseDirectAndIndirectLightingSeparately();

    auto& denoiser0 = (separateDenoiserEnabled ? m_common->metaPrimaryDirectLightDenoiser() : m_common->metaPrimaryCombinedLightDenoiser());
    auto& denoiser1 = (separateDenoiserEnabled ? m_common->metaPrimaryIndirectLightDenoiser() : m_common->metaPrimaryCombinedLightDenoiser());
    auto& denoiser2 = m_common->metaSecondaryCombinedLightDenoiser();

    outPrimaryDirectNrdArgs = denoiser0.getNrdArgs();
    outPrimaryIndirectNrdArgs = denoiser1.getNrdArgs();
    outSecondaryNrdArgs = denoiser2.getNrdArgs();

    // Disable ReBLUR when RR is on because ReBLUR uses a different buffer encoding
    bool useRR = useRayReconstruction();
    if (useRR) {
      outPrimaryDirectNrdArgs.isReblurEnabled = false;
      outPrimaryIndirectNrdArgs.isReblurEnabled = false;
    }
  }

  void RtxContext::updateRaytraceArgsConstantBuffer(Resources::RaytracingOutput& rtOutput,
                                                    const VkExtent3D& downscaledExtent, const VkExtent3D& targetExtent) {
    ScopedCpuProfileZone();
    // Prepare shader arguments
    RaytraceArgs &constants = rtOutput.m_raytraceArgs;
    constants = {}; 

    auto const& camera{ getSceneManager().getCamera() };
    const uint32_t frameIdx = m_device->getCurrentFrameId();

    constants.camera = camera.getShaderConstants();

    // Set the Raytraced Render Target camera matrices
    auto const& renderTargetCamera { getSceneManager().getCameraManager().getCamera(CameraType::RenderToTexture) };
    constants.renderTargetCamera = renderTargetCamera.getShaderConstants(/*freecam =*/ false);
    constants.enableRaytracedRenderTarget = renderTargetCamera.isValid(m_device->getCurrentFrameId()) && RtxOptions::RaytracedRenderTarget::enable();
    const CameraManager& cameraManager = getSceneManager().getCameraManager();

    const bool enablePortalVolumes = RtxGlobalVolumetrics::enableInPortals() &&
      cameraManager.isCameraValid(CameraType::Portal0) &&
      cameraManager.isCameraValid(CameraType::Portal1);
    
    // Note: Ensure the number of lights can fit into the ray tracing args.
    assert(getSceneManager().getLightManager().getActiveCount() <= std::numeric_limits<uint16_t>::max());
    bool useRR = shouldUseRayReconstruction();

    constants.frameIdx = RtxOptions::rngSeedWithFrameIndex() ? m_device->getCurrentFrameId() : 0;
    constants.lightCount = static_cast<uint16_t>(getSceneManager().getLightManager().getActiveCount());

    constants.fireflyFilteringLuminanceThreshold = RtxOptions::fireflyFilteringLuminanceThreshold();
    constants.secondarySpecularFireflyFilteringThreshold = RtxOptions::secondarySpecularFireflyFilteringThreshold();
    constants.primaryRayMaxInteractions = RtxOptions::primaryRayMaxInteractions();
    constants.psrRayMaxInteractions = RtxOptions::psrRayMaxInteractions();
    constants.secondaryRayMaxInteractions = RtxOptions::secondaryRayMaxInteractions();

    // Todo: Potentially move this to the volume manager in the future to be more organized.
    constants.volumeTemporalReuseMaxSampleCount = RtxGlobalVolumetrics::temporalReuseMaxSampleCount();
    
    constants.russianRouletteMode = RtxOptions::russianRouletteMode();
    constants.russianRouletteDiffuseContinueProbability = RtxOptions::russianRouletteDiffuseContinueProbability();
    constants.russianRouletteSpecularContinueProbability = RtxOptions::russianRouletteSpecularContinueProbability();
    constants.russianRouletteDistanceFactor = RtxOptions::russianRouletteDistanceFactor();
    constants.russianRouletteMaxContinueProbability = RtxOptions::russianRouletteMaxContinueProbability();
    constants.russianRoulette1stBounceMinContinueProbability = RtxOptions::russianRoulette1stBounceMinContinueProbability();
    constants.russianRoulette1stBounceMaxContinueProbability = RtxOptions::russianRoulette1stBounceMaxContinueProbability();
    constants.pathMinBounces = RtxOptions::pathMinBounces();
    constants.pathMaxBounces = RtxOptions::pathMaxBounces();
    // Note: Probability adjustments always in the 0-1 range and therefore less than FLOAT16_MAX.
    constants.opaqueDiffuseLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::opaqueDiffuseLobeSamplingProbabilityZeroThreshold());
    constants.minOpaqueDiffuseLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minOpaqueDiffuseLobeSamplingProbability());
    constants.opaqueSpecularLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::opaqueSpecularLobeSamplingProbabilityZeroThreshold());
    constants.minOpaqueSpecularLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minOpaqueSpecularLobeSamplingProbability());
    constants.opaqueOpacityTransmissionLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::opaqueOpacityTransmissionLobeSamplingProbabilityZeroThreshold());
    constants.minOpaqueOpacityTransmissionLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minOpaqueOpacityTransmissionLobeSamplingProbability());
    constants.opaqueDiffuseTransmissionLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::opaqueDiffuseTransmissionLobeSamplingProbabilityZeroThreshold());
    constants.minOpaqueDiffuseTransmissionLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minOpaqueDiffuseTransmissionLobeSamplingProbability());
    constants.translucentSpecularLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::translucentSpecularLobeSamplingProbabilityZeroThreshold());
    constants.minTranslucentSpecularLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minTranslucentSpecularLobeSamplingProbability());
    constants.translucentTransmissionLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::translucentTransmissionLobeSamplingProbabilityZeroThreshold());
    constants.minTranslucentTransmissionLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minTranslucentTransmissionLobeSamplingProbability());
    constants.indirectRaySpreadAngleFactor = RtxOptions::indirectRaySpreadAngleFactor();

    // Note: Emissibe blend override emissive intensity always clamped to FLOAT16_MAX, so this packing is fine.
    constants.emissiveBlendOverrideEmissiveIntensity = glm::packHalf1x16(RtxOptions::emissiveBlendOverrideEmissiveIntensity());
    constants.emissiveIntensity = glm::packHalf1x16(RtxOptions::emissiveIntensity());
    constants.particleSoftnessFactor = glm::packHalf1x16(RtxOptions::particleSoftnessFactor());

    constants.psrrMaxBounces = RtxOptions::psrrMaxBounces();
    constants.pstrMaxBounces = RtxOptions::pstrMaxBounces();

    auto& rayReconstruction = m_common->metaRayReconstruction();
    constants.outputParticleLayer = useRR && rayReconstruction.useParticleBuffer();

    auto& rtxdi = m_common->metaRtxdiRayQuery();
    constants.enableEmissiveBlendEmissiveOverride = RtxOptions::enableEmissiveBlendEmissiveOverride();
    constants.enableRtxdi = RtxOptions::useRTXDI();
    constants.enableSecondaryBounces = RtxOptions::enableSecondaryBounces();
    constants.enableSeparatedDenoisers = RtxOptions::denoiseDirectAndIndirectLightingSeparately();
    constants.enableCalculateVirtualShadingNormals = RtxOptions::useVirtualShadingNormalsForDenoising();
    constants.enableViewModelVirtualInstances = RtxOptions::ViewModel::enableVirtualInstances();
    constants.enablePSRR = RtxOptions::enablePSRR();
    constants.enablePSTR = RtxOptions::enablePSTR();
    constants.enablePSTROutgoingSplitApproximation = RtxOptions::enablePSTROutgoingSplitApproximation();
    constants.enablePSTRSecondaryIncidentSplitApproximation = RtxOptions::enablePSTRSecondaryIncidentSplitApproximation();
    constants.psrrNormalDetailThreshold = RtxOptions::psrrNormalDetailThreshold();
    constants.pstrNormalDetailThreshold = RtxOptions::pstrNormalDetailThreshold();
    constants.enableDirectLighting = RtxOptions::enableDirectLighting();
    constants.enableStochasticAlphaBlend = m_common->metaComposite().enableStochasticAlphaBlend();
    constants.enableSeparateUnorderedApproximations = RtxOptions::enableSeparateUnorderedApproximations() && getResourceManager().getTLAS(Tlas::Unordered).accelStructure != nullptr;
    constants.enableDirectTranslucentShadows = RtxOptions::enableDirectTranslucentShadows();
    constants.enableDirectAlphaBlendShadows = RtxOptions::enableDirectAlphaBlendShadows();
    constants.enableIndirectTranslucentShadows = RtxOptions::enableIndirectTranslucentShadows();
    constants.enableIndirectAlphaBlendShadows = RtxOptions::enableIndirectAlphaBlendShadows();
    constants.enableRussianRoulette = RtxOptions::enableRussianRoulette();
    constants.enableDemodulateRoughness = m_common->metaDemodulate().demodulateRoughness();
    constants.enableReplaceDirectSpecularHitTWithIndirectSpecularHitT = RtxOptions::replaceDirectSpecularHitTWithIndirectSpecularHitT();
    constants.enablePortalFadeInEffect = RtxOptions::enablePortalFadeInEffect();
    constants.enableEnhanceBSDFDetail = (shouldUseDLSS() || useRR || shouldUseTAA()) && m_common->metaComposite().enableDLSSEnhancement();
    constants.enhanceBSDFIndirectMode = (uint32_t)m_common->metaComposite().dlssEnhancementMode();
    constants.enhanceBSDFDirectLightPower = useRR ? 0.0 : m_common->metaComposite().dlssEnhancementDirectLightPower();
    constants.enhanceBSDFIndirectLightPower = m_common->metaComposite().dlssEnhancementIndirectLightPower();
    constants.enhanceBSDFDirectLightMaxValue = m_common->metaComposite().dlssEnhancementDirectLightMaxValue();
    constants.enhanceBSDFIndirectLightMaxValue = m_common->metaComposite().dlssEnhancementIndirectLightMaxValue();
    constants.enhanceBSDFIndirectLightMinRoughness = m_common->metaComposite().dlssEnhancementIndirectLightMinRoughness();
    constants.enableFirstBounceLobeProbabilityDithering = RtxOptions::enableFirstBounceLobeProbabilityDithering();
    constants.enableUnorderedResolveInIndirectRays = RtxOptions::enableUnorderedResolveInIndirectRays();
    constants.enableProbabilisticUnorderedResolveInIndirectRays = RtxOptions::enableProbabilisticUnorderedResolveInIndirectRays();
    constants.enableTransmissionApproximationInIndirectRays = RtxOptions::enableTransmissionApproximationInIndirectRays();
    constants.enableUnorderedEmissiveParticlesInIndirectRays = RtxOptions::enableUnorderedEmissiveParticlesInIndirectRays();
    constants.enableDecalMaterialBlending = RtxOptions::enableDecalMaterialBlending();
    constants.enableLegacyRectLightConeShaping = LightManager::enableLegacyRectLightConeShaping();
    constants.enableRectLightConeShapingRatioScaling = LightManager::enableRectLightConeShapingRatioScaling();
    constants.enableBillboardOrientationCorrection = RtxOptions::enableBillboardOrientationCorrection() && RtxOptions::enableSeparateUnorderedApproximations();
    constants.useIntersectionBillboardsOnPrimaryRays = RtxOptions::useIntersectionBillboardsOnPrimaryRays() && constants.enableBillboardOrientationCorrection;
    constants.enableDirectLightBoilingFilter = m_common->metaDemodulate().enableDirectLightBoilingFilter() && RtxOptions::useRTXDI();
    constants.directLightBoilingThreshold = m_common->metaDemodulate().directLightBoilingThreshold();
    constants.translucentDecalAlbedoFactor = RtxOptions::translucentDecalAlbedoFactor();
    constants.enablePlayerModelInPrimarySpace = RtxOptions::PlayerModel::enableInPrimarySpace();
    constants.enablePlayerModelPrimaryShadows = RtxOptions::PlayerModel::enablePrimaryShadows();
    constants.enablePreviousTLAS = RtxOptions::enablePreviousTLAS() && m_common->getSceneManager().isPreviousFrameSceneAvailable();

    constants.pomMode = getSceneManager().getActivePOMCount() > 0 ? RtxOptions::Displacement::mode() : DisplacementMode::Off;
    if (constants.pomMode == DisplacementMode::Off) {
      constants.pomEnableDirectLighting = false;
      constants.pomEnableIndirectLighting = false;
      constants.pomEnableNEECache = false;
      constants.pomEnableReSTIRGI = false;
      constants.pomEnablePSR = true; // enable PSR for materials with heightmaps if POM is completely disabled.
    } else {
      constants.pomEnableDirectLighting = RtxOptions::Displacement::enableDirectLighting();
      constants.pomEnableIndirectLighting = RtxOptions::Displacement::enableIndirectLighting();
      constants.pomEnableNEECache = RtxOptions::Displacement::enableNEECache();
      constants.pomEnableReSTIRGI = RtxOptions::Displacement::enableReSTIRGI();
      constants.pomEnablePSR = RtxOptions::Displacement::enablePSR();
    }
    constants.pomMaxIterations = RtxOptions::Displacement::maxIterations();

    constants.totalMipBias = getSceneManager().getTotalMipBias(); 

    constants.upscaleFactor = float2 {
      rtOutput.m_compositeOutputExtent.width / static_cast<float>(rtOutput.m_finalOutputExtent.width),
      rtOutput.m_compositeOutputExtent.height / static_cast<float>(rtOutput.m_finalOutputExtent.height) };

    constants.terrainArgs = getSceneManager().getTerrainBaker().getTerrainArgs();



    constants.sssArgs.enableThinOpaque = RtxOptions::SubsurfaceScattering::enableThinOpaque();
    constants.sssArgs.enableDiffusionProfile = RtxOptions::SubsurfaceScattering::enableDiffusionProfile();
    constants.sssArgs.diffusionProfileScale = std::max(RtxOptions::SubsurfaceScattering::diffusionProfileScale(), 0.001f);
    constants.enableSssTransmission = RtxOptions::SubsurfaceScattering::enableTransmission();
    constants.enableSssTransmissionSingleScattering = RtxOptions::SubsurfaceScattering::enableTransmissionSingleScattering();
    constants.sssTransmissionBsdfSampleCount = RtxOptions::SubsurfaceScattering::transmissionBsdfSampleCount();
    constants.sssTransmissionSingleScatteringSampleCount = RtxOptions::SubsurfaceScattering::transmissionSingleScatteringSampleCount();
    constants.enableTransmissionDiffusionProfileCorrection = RtxOptions::SubsurfaceScattering::enableTransmissionDiffusionProfileCorrection();
    constants.enableHeuristicSingleScatteringTransmission = RtxOptions::SubsurfaceScattering::enableHeuristicSingleScatteringTransmission();
    constants.sssArgs.diffusionProfileDebuggingPixel = u16vec2 {
      static_cast<uint16_t>(RtxOptions::SubsurfaceScattering::diffusionProfileDebugPixelPosition().x),
      static_cast<uint16_t>(RtxOptions::SubsurfaceScattering::diffusionProfileDebugPixelPosition().y) };

    auto& restirGI = m_common->metaReSTIRGIRayQuery();
    ReSTIRGISampleStealing restirGISampleStealingMode = restirGI.useSampleStealing();
    // Stealing pixels requires indirect light stored in separated buffers instead of combined with direct light,
    // steal samples if separated denoiser is disabled.
    if (restirGISampleStealingMode == ReSTIRGISampleStealing::StealPixel 
        && !RtxOptions::denoiseDirectAndIndirectLightingSeparately()) {
      restirGISampleStealingMode = ReSTIRGISampleStealing::StealSample;
    }
    constants.enableReSTIRGI = restirGI.isActive();
    constants.enableReSTIRGITemporalReuse = restirGI.useTemporalReuse();
    constants.enableReSTIRGISpatialReuse = restirGI.useSpatialReuse();
    constants.reSTIRGIMISMode = (uint32_t)restirGI.misMode();
    constants.enableReSTIRGIFinalVisibility = restirGI.useFinalVisibility();
    constants.enableReSTIRGIReflectionReprojection = restirGI.useReflectionReprojection();
    constants.restirGIReflectionMinParallax = restirGI.reflectionMinParallax();
    constants.enableReSTIRGIVirtualSample = restirGI.useVirtualSample();
    constants.reSTIRGIMISModePairwiseMISCentralWeight = restirGI.pairwiseMISCentralWeight();
    constants.reSTIRGIVirtualSampleLuminanceThreshold = restirGI.virtualSampleLuminanceThreshold();
    constants.reSTIRGIVirtualSampleRoughnessThreshold = restirGI.virtualSampleRoughnessThreshold();
    constants.reSTIRGIVirtualSampleSpecularThreshold = restirGI.virtualSampleSpecularThreshold();
    constants.reSTIRGIVirtualSampleMaxDistanceRatio = restirGI.virtualSampleMaxDistanceRatio();
    constants.reSTIRGIBiasCorrectionMode = (uint32_t) restirGI.biasCorrectionMode();
    constants.enableReSTIRGIPermutationSampling = restirGI.usePermutationSampling();
    constants.enableReSTIRGISampleStealing = (uint32_t)restirGISampleStealingMode;
    constants.reSTIRGISampleStealingJitter = restirGI.sampleStealingJitter();
    constants.enableReSTIRGIStealBoundaryPixelSamplesWhenOutsideOfScreen = (uint32_t)restirGI.stealBoundaryPixelSamplesWhenOutsideOfScreen();
    constants.enableReSTIRGIBoilingFilter = restirGI.useBoilingFilter();
    constants.boilingFilterLowerThreshold = restirGI.boilingFilterMinThreshold();
    constants.boilingFilterHigherThreshold = restirGI.boilingFilterMaxThreshold();
    constants.boilingFilterRemoveReservoirThreshold = restirGI.boilingFilterRemoveReservoirThreshold();
    constants.temporalHistoryLength = restirGI.getTemporalHistoryLength(GlobalTime::get().deltaTimeMs());
    constants.permutationSamplingSize = restirGI.permutationSamplingSize();
    constants.enableReSTIRGIDLSSRRCompatibilityMode = useRR ? restirGI.useDLSSRRCompatibilityMode() : 0;
    constants.reSTIRGIDLSSRRTemporalRandomizationRadius = constants.camera.resolution.x / 960.0f * restirGI.DLSSRRTemporalRandomizationRadius();
    constants.enableReSTIRGITemporalBiasCorrection = restirGI.useTemporalBiasCorrection();
    constants.enableReSTIRGIDiscardEnlargedPixels = restirGI.useDiscardEnlargedPixels();
    constants.reSTIRGIHistoryDiscardStrength = restirGI.historyDiscardStrength();
    constants.enableReSTIRGITemporalJacobian = restirGI.useTemporalJacobian();
    constants.reSTIRGIFireflyThreshold = restirGI.fireflyThreshold();
    constants.reSTIRGIRoughnessClamp = restirGI.roughnessClamp();
    constants.reSTIRGIMISRoughness = restirGI.misRoughness();
    constants.reSTIRGIMISParallaxAmount = restirGI.parallaxAmount();
    constants.enableReSTIRGIDemodulatedTargetFunction = restirGI.useDemodulatedTargetFunction();
    constants.enableReSTIRGILightingValidation = RtxOptions::useRTXDI() && rtxdi.getEnableDenoiserGradient(*this) && restirGI.validateLightingChange();
    constants.reSTIRGISampleValidationThreshold = restirGI.lightingValidationThreshold();
    constants.enableReSTIRGIVisibilityValidation = restirGI.validateVisibilityChange();
    constants.reSTIRGIVisibilityValidationRange = 1.0f + restirGI.visibilityValidationRange();

    // Neural Radiance Cache
    NeuralRadianceCache& nrc = m_common->metaNeuralRadianceCache();
    constants.enableNrc = nrc.isActive();
    constants.allowNrcTraining = NeuralRadianceCache::NrcOptions::trainCache();
    nrc.setRaytraceArgs(constants);

    m_common->metaNeeCache().setRaytraceArgs(constants, m_resetHistory);
    constants.surfaceCount = getSceneManager().getAccelManager().getSurfaceCount();

    auto* cameraTeleportDirectionInfo = getSceneManager().getRayPortalManager().getCameraTeleportationRayPortalDirectionInfo();
    constants.teleportationPortalIndex = cameraTeleportDirectionInfo ? cameraTeleportDirectionInfo->entryPortalInfo.portalIndex + 1 : 0;

    // Note: Use half of the vertical FoV for the main camera in radians divided by the vertical resolution to get the effective half angle of a single pixel.
    constants.screenSpacePixelSpreadHalfAngle = getSceneManager().getCamera().getFov() / 2.0f / constants.camera.resolution.y;

    // Note: This value is assumed to be positive (specifically not have the sign bit set) as otherwise it will break Ray Interaction encoding.
    assert(std::signbit(constants.screenSpacePixelSpreadHalfAngle) == false);

    // Enable object picking only when resource was created
    // TODO: should be a spec.const
    constants.enableObjectPicking = bool { rtOutput.m_primaryObjectPicking.isValid() };

    // Debug View
    {
      const DebugView& debugView = m_common->metaDebugView();
      constants.debugView = debugView.debugViewIdx();
      constants.debugKnob = debugView.debugKnob();
      constants.forceFirstHitInGBufferPass = debugView.showFirstGBufferHit();
      
      constants.gpuPrintThreadIndex = u16vec2 { kInvalidThreadIndex, kInvalidThreadIndex };
      constants.gpuPrintElementIndex = frameIdx % kMaxFramesInFlight;

      // Ctrl+Alt+O also opens the probe window (triggerGpuPrintWindow), so the probe samples wherever the
      // cursor sits and covers the same frames as the trace. Consumed one frame at a time next to the read of
      // the flag it guards, so the window cannot outlive the trace.
      bool probeWindowActive = false;
      if (s_gpuPrintWindowFramesRemaining.load(std::memory_order_relaxed) > 0u) {
        s_gpuPrintWindowFramesRemaining.fetch_sub(1u, std::memory_order_relaxed);
        probeWindowActive = true;
      }

      if (kenshi_telemetry::enabled() && debugView.gpuPrint.enable()
       && (probeWindowActive || ImGui::IsKeyDown(ImGuiKey_ModCtrl))) {
        if (debugView.gpuPrint.useMousePosition()) {
          Vector2 toDownscaledExtentScale{
            downscaledExtent.width / static_cast<float>(targetExtent.width),
            downscaledExtent.height / static_cast<float>(targetExtent.height)
          };

          const ImVec2 mousePos = ImGui::GetMousePos();
          constants.gpuPrintThreadIndex = u16vec2 {
            static_cast<uint16_t>(mousePos.x * toDownscaledExtentScale.x),
            static_cast<uint16_t>(mousePos.y * toDownscaledExtentScale.y)
          };
        } else {
          constants.gpuPrintThreadIndex = u16vec2 {
            static_cast<uint16_t>(debugView.gpuPrint.pixelIndex().x), 
            static_cast<uint16_t>(debugView.gpuPrint.pixelIndex().y) 
          };
        }
      }
    }

    getDenoiseArgs(constants.primaryDirectNrd, constants.primaryIndirectNrd, constants.secondaryCombinedNrd);

    RayPortalManager::SceneData portalData = getSceneManager().getRayPortalManager().getRayPortalInfoSceneData();
    constants.numActiveRayPortals = portalData.numActiveRayPortals;
    constants.virtualInstancePortalIndex = getSceneManager().getInstanceManager().getVirtualInstancePortalIndex() & 0xff;

    memcpy(&constants.rayPortalHitInfos[0], &portalData.rayPortalHitInfos, sizeof(portalData.rayPortalHitInfos));
    memcpy(&constants.rayPortalHitInfos[maxRayPortalCount], &portalData.previousRayPortalHitInfos, sizeof(portalData.previousRayPortalHitInfos));

    constants.uniformRandomNumber = jenkinsHash(constants.frameIdx);
    constants.vertexColorStrength = RtxOptions::vertexColorStrength();
    constants.viewModelRayTMax = RtxOptions::ViewModel::rangeMeters() * RtxOptions::getMeterToWorldUnitScale();
    constants.roughnessDemodulationOffset = m_common->metaDemodulate().demodulateRoughnessOffset();
    
    const RtxGlobalVolumetrics& globalVolumetrics = getCommonObjects()->metaGlobalVolumetrics();
    constants.volumeArgs = globalVolumetrics.getVolumeArgs(cameraManager, getSceneManager().getFogState(), enablePortalVolumes);
    constants.startInMediumMaterialIndex = getSceneManager().getStartInMediumMaterialIndex();
    OpaqueMaterialOptions::fillShaderParams(constants.opaqueMaterialArgs);
    // pad0 carries the terrain secondary-shading mode.
    constants.opaqueMaterialArgs.pad0 = KenshiTerrainOptions::secondaryShadingMode();

    // Fold the strength in here, so the shader has one value to read and a strength of zero costs nothing.
    {
      float kenshiWetness = 0.0f;
      float kenshiWaterHeight = 0.0f;
      SceneManager::getKenshiWetness(kenshiWetness, kenshiWaterHeight);
      // A gain, capped at 4 rather than 1: the game's own wetness tops out around 0.64 in rain, where its
      // response `2*(w - 0.15a)^(4a+1)` barely changes gloss; the curve only bites near w=1. The product is
      // clamped, not the gain, so w never exceeds the game's maximum of 1.
      const float wetnessRaw = KenshiOptions::kenshiWetness();
      const float wetnessStrength =
        wetnessRaw < 0.0f ? 0.0f : (wetnessRaw > 4.0f ? 4.0f : wetnessRaw);
      constants.kenshiWetness = std::min(kenshiWetness * wetnessStrength, 1.0f);
      constants.kenshiWaterHeight = kenshiWaterHeight;
      // Characters' own gloss gain. Zero leaves roughness untouched.
      constants.kenshiCharacterGloss = KenshiOptions::kenshiCharacterGloss();

      // Kenshi's biome dust. The strength folds in here, so zero costs the shader nothing (as with wetness).
      Vector3 kenshiDustColour(0.0f, 0.0f, 0.0f);
      float kenshiDustAmountX = 0.0f;
      float kenshiDustAmountY = 0.0f;
      float kenshiDustAmountZ = 0.0f;
      SceneManager::getKenshiDust(
        kenshiDustColour, kenshiDustAmountX, kenshiDustAmountY, kenshiDustAmountZ);
      const float dustRaw = KenshiOptions::kenshiDust();
      const float dustStrength = dustRaw < 0.0f ? 0.0f : (dustRaw > 4.0f ? 4.0f : dustRaw);
      // Global dust colour: convert to linear for the channel ratios (hue and saturation), then rescale so
      // the peak channel keeps its original value. Using the sRGB constant as linear brightens and
      // desaturates it; a plain conversion darkens it to a third of what the display-space blend needs.
      auto srgbToLinear = [](float c) -> float {
        c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
        return c <= 0.04045f ? c / 12.92f
                             : std::pow((c + 0.055f) / 1.055f, 2.4f);
      };
      const float dustPeakSrgb = std::max(kenshiDustColour.x,
        std::max(kenshiDustColour.y, kenshiDustColour.z));
      Vector3 dustLinear(srgbToLinear(kenshiDustColour.x),
                         srgbToLinear(kenshiDustColour.y),
                         srgbToLinear(kenshiDustColour.z));
      const float dustPeakLinear = std::max(dustLinear.x,
        std::max(dustLinear.y, dustLinear.z));
      const float dustRenorm = dustPeakLinear > 1.0e-6f
        ? dustPeakSrgb / dustPeakLinear : 1.0f;
      constants.kenshiDustColourR = dustLinear.x * dustRenorm;
      constants.kenshiDustColourG = dustLinear.y * dustRenorm;
      constants.kenshiDustColourB = dustLinear.z * dustRenorm;
      constants.kenshiDustAmountX = kenshiDustAmountX;
      constants.kenshiDustAmountY = kenshiDustAmountY;
      constants.kenshiDustAmountZ = kenshiDustAmountZ;
      constants.kenshiDustStrength = dustStrength;

      // The water constants read off the game's own water draw; inert until one has been seen (zero tile
      // scale).
      {
        SceneManager::KenshiWaterParams water;
        SceneManager::getKenshiWater(water);
        constants.kenshiWaterTileScaleX = water.tileScaleX;
        constants.kenshiWaterTileScaleY = water.tileScaleY;
        constants.kenshiWaterTileOffsetX = water.tileOffsetX;
        constants.kenshiWaterTileOffsetY = water.tileOffsetY;
        constants.kenshiWaterMapScaleX = water.mapScaleX;
        constants.kenshiWaterMapScaleY = water.mapScaleY;
        constants.kenshiWaterMapOffsetX = water.mapOffsetX;
        constants.kenshiWaterMapOffsetY = water.mapOffsetY;
        constants.kenshiWaterSpeedX = water.speedX;
        constants.kenshiWaterSpeedY = water.speedY;
        constants.kenshiWaterDistortion = water.distortion;
        constants.kenshiWaterInvStrength = water.invStrength;
        // Accumulate the water clock here instead of publishing the game's raw gameTime. gameTime is a genuine
        // 0 for the first ~5-6 s after every load and then runs at ~0.008/s (a ~1 Hz ripple cross-fade at
        // distortion 120), so zero cannot mean "missing". Kenshi's pause and three game speeds are all
        // expressed in how fast gameTime moves:
        //   delta  > 0                    -> running at any speed: consume it
        //   delta == 0, never ran         -> load-in window:       wall fallback
        //   delta == 0, has run           -> PAUSED:               hold
        //   delta  < 0 / gameTime reset   -> new save:             re-arm fallback
        {
          // The live rate measured above, expressed as ripple cross-fade cycles
          // per second. Divided by distortion because the shader multiplies by
          // it again.
          constexpr float kWaterFallbackRippleHz = 1.0f;
          // A load or an alt-tab must not teleport the surface.
          constexpr float kWaterMaxWallStep = 0.25f;
          // gameTime advances ~1.3e-4 per frame; anything past this is a reset,
          // a rebase, or a different save, and is not a delta worth trusting.
          constexpr float kWaterMaxGameStep = 1.0f;

          static float s_waterClock = 0.0f;
          static float s_waterPrevGameTime = 0.0f;
          static std::chrono::steady_clock::time_point s_waterPrevWall;
          // Have we ever had a previous frame to measure a wall delta against?
          static bool s_waterSeenAFrame = false;
          // Has the GAME clock advanced at least once since the last load? This
          // is what separates "not started yet" from "paused" - both show a zero
          // delta, and only the first one may use the wall-clock fallback.
          static bool s_waterGameRunning = false;

          const auto waterNowWall = std::chrono::steady_clock::now();
          const float waterDistortion = std::max(water.distortion, 1.0e-3f);

          float waterWallDelta = 0.0f;
          if (s_waterSeenAFrame) {
            waterWallDelta = std::clamp(
              std::chrono::duration<float>(waterNowWall - s_waterPrevWall).count(),
              0.0f, kWaterMaxWallStep);
          }

          const float waterGameDelta = water.time - s_waterPrevGameTime;

          // A save load restarts gameTime from zero. Re-arm the fallback rather
          // than freezing for the ~5 s until the new session's clock starts.
          const bool waterClockReset = waterGameDelta < 0.0f
            || waterGameDelta >= kWaterMaxGameStep
            || (water.time <= 0.0f && s_waterPrevGameTime > 0.0f);

          if (waterClockReset) {
            s_waterGameRunning = false;
          } else if (waterGameDelta > 0.0f) {
            // The game's own delta: pause and all game speeds come for free.
            s_waterClock += waterGameDelta;
            s_waterGameRunning = true;
          } else if (!s_waterGameRunning) {
            // Never ticked since the last load (gameTime is 0 for the first ~5 s of a load): use the wall-clock
            // fallback.
            s_waterClock += waterWallDelta * (kWaterFallbackRippleHz / waterDistortion);
          }
          // else: the clock has run and has now stopped - the game is paused. Hold the accumulator, as raster
          // freezes the surface.

          s_waterPrevGameTime = water.time;
          s_waterPrevWall = waterNowWall;
          s_waterSeenAFrame = true;

          constants.kenshiWaterTime = s_waterClock;
        }
        constants.kenshiWaterRainAmount = water.rainAmount;
        constants.kenshiWaterScumScaleX = water.scumScaleX;
        constants.kenshiWaterScumScaleY = water.scumScaleY;
        constants.kenshiWaterScumDistortion = water.scumDistortion;
        // Pack the rain enable bit into the sign of Pad1, whose shader transport is proven by the
        // reflection-clearance control.
        const uint32_t waterNormalTelemetryMode =
          (kenshi_telemetry::enabled() ? std::min(KenshiOptions::kenshiWaterNormalTelemetry(), 7u) : 0u);
        // The sign carries the simulated-depth toggle without growing the shared C++/shader constant-buffer
        // layout. The translucent material reads the option directly on the CPU; only the opaque shader
        // consumes the negative form. Zero remains bit-identical to fully opaque water.
        const float waterTransparency =
          std::max(KenshiOptions::kenshiWaterTransparency(), 0.0f);
        constants.kenshiWaterTransparency =
          KenshiOptions::kenshiWaterSimulatedDepth() && waterTransparency > 0.0f
            ? -waterTransparency
            : waterTransparency;
        // The game's own value, harvested from the near shaders. It is per water body (FCS "water visibility",
        // `invOpacity = 1 / visibility`), so this global is only the fallback for water outside every biome;
        // the zone's own value travels in the biome record.
        constants.kenshiWaterInvOpacity = water.invOpacity;
        // Kenshi's own glow is a single constant, 0.0, declared in WaterFP's default_params
        // (data/materials/forward/water.material) and never overridden, so the raster term is inert in the
        // shipped game. Additive rather than an override, so a water body that does set one keeps its
        // variation. Per-biome colour survives a single global amount: the colour is waterColour (the
        // world-wide colour map at the water's own UV) and only the scalar is global, as in the raster shader.
        constants.kenshiWaterGlow =
          water.glow + std::max(KenshiOptions::kenshiWaterGlowBoost(), 0.0f);
        // Reproduces the non-physical gain Kenshi applies to its water diffuse (PI on the sun term where a
        // normalised BRDF divides by it, 4x on the irradiance probe), which is why raster water carries more
        // biome colour. Clamped at 0; 1 is physical.
        constants.kenshiWaterColourGain =
          std::max(KenshiOptions::kenshiWaterColourGain(), 0.0f);
        constants.kenshiWaterPad0 = static_cast<float>(waterNormalTelemetryMode);
        const float waterReflectionClearance = std::clamp(
          KenshiOptions::kenshiWaterReflectionClearance(), 0.05f, 1.0f);
        const bool waterRainRipples = KenshiOptions::kenshiWaterRainRipples();
        constants.kenshiWaterPad1 = waterRainRipples
          ? waterReflectionClearance : -waterReflectionClearance;
        // Pad2 carries the rain reach in world units; 0 keeps the unbounded behaviour, so a shader that never
        // sees this value behaves as before.
        constants.kenshiWaterPad2 =
          std::max(KenshiOptions::kenshiWaterRainDistance(), 0.0f);

        static uint32_t s_lastWaterNormalTelemetryMode = ~0u;
        if (kenshi_telemetry::enabled() && s_lastWaterNormalTelemetryMode != waterNormalTelemetryMode) {
          s_lastWaterNormalTelemetryMode = waterNormalTelemetryMode;
          Logger::info(str::format(
            "DX11_V612_KENSHI_WATER_RAIN_TOGGLE mode=",
            waterNormalTelemetryMode,
            " rainRipples=", waterRainRipples ? 1 : 0,
            " (0=current, 1=flat, 2=ripple-only-unbent, 3=full-unbent, "
            "4=centred-ripple-unbent, 5=centred-ripple-bent, "
            "6=ripple-only-bent, 7=centred-full-bent)"));
        }
      }

      // The submerged term is `saturate((waterHeightRel - worldY + 2) * 0.5)`: at waterHeightRel=100
      // everything below world Y 98 reads as underwater (no wet highlight, flat 0.2 darkening). Report the
      // camera height against it, once and on every meaningful move.
      if (terrain_profile::diagnosticsEnabled()) {
        static float sLastLoggedCameraY = -1.0e30f;
        const float cameraY = getSceneManager().getCamera().getPosition().y;
        if (std::abs(cameraY - sLastLoggedCameraY) > 25.0f) {
          sLastLoggedCameraY = cameraY;
          KENSHI_DIAGNOSTIC_INFO(str::format(
            "[RTX Kenshi][wetness-height] cameraWorldY=", cameraY,
            " waterHeightRel=", kenshiWaterHeight,
            " submergedAtCamera=", [&] {
              const float s = (kenshiWaterHeight - cameraY + 2.0f) * 0.5f;
              return s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
            }()));
        }
      }
    }
    // pad1 carries both gloss strengths as two unorm16s (low half terrain, high half objects), so the
    // constant-buffer layout does not change. Kept in a constant rather than the material so the sliders
    // are live.
    auto packStrength = [](float value) -> uint32_t {
      const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
      return uint32_t(clamped * 65535.0f + 0.5f);
    };
    // Low half: terrain gloss STRENGTH, 0..1. High half: object gloss
    // MULTIPLIER, 0..4 - quantised over the wider range, which at 16 bits is
    // still far finer than the slider's step.
    constants.opaqueMaterialArgs.pad1 =
        packStrength(KenshiTerrainOptions::glossStrength())
      | (packStrength(KenshiOptions::kenshiObjectGlossMultiplier() * 0.25f) << 16u);
    TranslucentMaterialOptions::fillShaderParams(constants.translucentMaterialArgs);
    ViewDistanceOptions::fillShaderParams(constants.viewDistanceArgs, RtxOptions::getMeterToWorldUnitScale());
    constants.alphaBlendSurfacePackMult = RtxOptions::getMeterToWorldUnitScale();

    // We are going to use this value to perform some animations on GPU, to mitigate precision related issues loop time
    // at the 24 bit boundary (as we use a 8 bit scalar on top of this time which we want to fit into 32 bits without issues,
    // plus we also convert this value to a floating point value at some point as well which has 23 bits of precision).
    // Bitwise and used rather than modulus as well for slightly better performance.
    constants.timeSinceStartSeconds = (static_cast<uint32_t>(GlobalTime::get().absoluteTimeMs()) & ((1U << 24U) - 1U)) / 1000.f;

    m_common->metaRtxdiRayQuery().setRaytraceArgs(rtOutput);
    getSceneManager().getLightManager().setRaytraceArgs(
      constants,
      m_common->metaRtxdiRayQuery().initialSampleCount(),
      RtxGlobalVolumetrics::initialRISSampleCount(),
      RtxOptions::risLightSampleCount());

    constants.resolveTransparencyThreshold = RtxOptions::resolveTransparencyThreshold();
    constants.resolveOpaquenessThreshold = RtxOptions::resolveOpaquenessThreshold();
    constants.resolveStochasticAlphaBlendThreshold = m_common->metaComposite().stochasticAlphaBlendOpacityThreshold();

    constants.skyBrightness = RtxOptions::skyBrightness();
    constants.skyProbeDecodeGamma = RtxOptions::skyProbeDecodeGamma();
    constants.skyProbeBlackPoint = RtxOptions::skyProbeBlackPoint();
    constants.skyProbeLightBrightness = RtxOptions::skyProbeLightBrightness();
    // Night floor for the sky's lighting contribution only.
    constants.skyProbeLightFloor = std::max(KenshiOptions::skyLightFloor(), 0.0f);
    // Per-region ambient tint from Kenshi's ambient map; the harvest writes it on the derived option layer
    // every frame.
    {
      const Vector3 ambientTint = KenshiOptions::kenshiAmbientTint();
      constants.kenshiAmbientTintR = std::max(ambientTint.x, 0.0f);
      constants.kenshiAmbientTintG = std::max(ambientTint.y, 0.0f);
      constants.kenshiAmbientTintB = std::max(ambientTint.z, 0.0f);
    }

    // DX11_V307_NO_DEGENERATE_SKY_PROBE: SkyboxRasterization makes every
    // g-buffer/indirect miss sample the SkyProbe cubemap by ray direction. That
    // cubemap cannot be rasterized correctly on the DX11 runtime - aiming each
    // of the six faces requires injecting customWorldToProjection into the
    // game's own DXBC vertex shader, which Remix does not author. All six faces
    // therefore receive the same view-projected sky, five of them fall outside
    // their face's clip volume and stay at the clear value, and the player ends
    // up sitting inside a black box.
    //
    // Fall back to the physical atmosphere, which is the only sky path here that
    // returns correct radiance for an arbitrary direction. This deliberately
    // overrides rtx.skyMode because the requested value is unimplementable on
    // this runtime; it is reported once so the override is not silent.
    SkyMode currentSkyMode = RtxOptions::skyMode();

    // Detect the condition structurally rather than waiting for the first sky draw to trip the flag: both
    // state constant buffers are null exactly when hasVertexStateCB would be false in rasterizeToSkyProbe,
    // and setConstantBuffers has no caller in this fork, so this is already true on frame 0.
    // With the b0 patch path enabled (reprojection by patching the first matrix of the sky shader's b0),
    // null state buffers no longer mean the probe cannot work; promotion is then left to
    // m_skyProbeReprojectionUnavailable, set only when the patch could not arm either.
    const bool skyProbePatchPathAvailable = RtxOptions::skyProbePatchFirstMatrix();
    const bool skyProbeUnusable = m_skyProbeReprojectionUnavailable
      || (!skyProbePatchPathAvailable
       && m_rtState.vertexCaptureCB == nullptr
       && m_rtState.vsFixedFunctionCB == nullptr);

    if (skyProbeUnusable && currentSkyMode == SkyMode::SkyboxRasterization) {
      currentSkyMode = SkyMode::PhysicalAtmosphere;
      ONCE(Logger::warn(
        "[RTX Sky] rtx.skyMode=SkyboxRasterization cannot be honoured on the DX11 runtime "
        "(the sky cubemap has no usable per-face reprojection). Using PhysicalAtmosphere "
        "instead - rasterized skybox mode would render a black box around the camera."));
    }

    constants.skyMode = static_cast<uint32_t>(currentSkyMode);

    // Detect sky mode changes and clear rasterized sky targets when switching to physical atmosphere.
    if (currentSkyMode != m_lastSkyMode) {
      if (currentSkyMode == SkyMode::PhysicalAtmosphere) {
        auto skyProbe = getResourceManager().getSkyProbe(this, m_skyColorFormat);
        auto skyMatte = getResourceManager().getSkyMatte(this, m_skyRtColorFormat);

        VkClearValue clearValue = {};
        clearValue.color.float32[0] = 0.0f;
        clearValue.color.float32[1] = 0.0f;
        clearValue.color.float32[2] = 0.0f;
        clearValue.color.float32[3] = 0.0f;

        if (skyProbe.view != nullptr) {
          DxvkContext::clearRenderTarget(skyProbe.view, VK_IMAGE_ASPECT_COLOR_BIT, clearValue);
        }

        if (skyMatte.view != nullptr) {
          DxvkContext::clearRenderTarget(skyMatte.view, VK_IMAGE_ASPECT_COLOR_BIT, clearValue);
        }
      }

      m_lastSkyMode = currentSkyMode;
    }

    // Update atmosphere parameters and LUTs in physical atmosphere mode.
    // Note: keyed off currentSkyMode, not RtxOptions::skyMode(), so the DX11
    // fallback above actually brings the atmosphere up. Reading the option here
    // would publish skyMode=PhysicalAtmosphere to the shader while leaving
    // m_atmosphere null and atmosphereArgs unwritten - the miss path would take
    // the atmosphere branch with no LUTs behind it and the sky would go black a
    // different way.
    if (currentSkyMode == SkyMode::PhysicalAtmosphere) {
      if (!m_atmosphere) {
        m_atmosphere = std::make_unique<RtxAtmosphere>(m_device.ptr());
      }

      m_atmosphere->initialize(this);
      m_atmosphere->computeLuts(this);
      constants.atmosphereArgs = m_atmosphere->getAtmosphereArgs();
    }

    // Inject / update / drop the sun distant light from the live atmosphere path. Called unconditionally:
    // the helper injects the atmosphere's sun in Numos mode and the game-driven sun in
    // skybox-rasterization mode, and drops both when neither applies. The game-driven path does not read
    // atmosphereArgs (only written inside the branch above).
    fork_hooks::syncAtmosphereDistantLights(*this, constants.atmosphereArgs);

    constants.isLastCompositeOutputValid = restirGI.isActive() && restirGI.getLastCompositeOutput().matchesWriteFrameIdx(frameIdx - 1);
    constants.isZUp = RtxOptions::zUp();
    constants.enableCullingSecondaryRays = RtxOptions::enableCullingInSecondaryRays();

    constants.domeLightArgs = getSceneManager().getLightManager().getDomeLightArgs();

    // Ray miss value handling
    constants.clearColorDepth = getSceneManager().getGlobals().clearColorDepth;
    constants.clearColorPicking = getSceneManager().getGlobals().clearColorPicking;
    constants.clearColorNormal = getSceneManager().getGlobals().clearColorNormal;

    // DLSS-RR
    constants.enableDLSSRR = useRR;
    constants.setLogValueForDisocclusionMaskForDLSSRR = DxvkRayReconstruction::enableDisocclusionMaskBlur();

    NrdArgs primaryDirectNrdArgs;
    NrdArgs primaryIndirectNrdArgs;
    NrdArgs secondaryNrdArgs;
    getDenoiseArgs(primaryDirectNrdArgs, primaryIndirectNrdArgs, secondaryNrdArgs);

    constants.primaryDirectMissLinearViewZ = primaryDirectNrdArgs.missLinearViewZ;

    constants.wboitEnergyLossCompensation = RtxOptions::wboitEnergyLossCompensation();
    constants.wboitDepthWeightTuning = RtxOptions::wboitDepthWeightTuning();
    constants.wboitEnabled = RtxOptions::wboitEnabled();
    constants.kenshiParticleLightIntensity = KenshiOptions::kenshiParticleLightIntensity();
    // Basic_Coloured_Ambient_VP applies the sun-height scalar before its LDR
    // ONE/ONE blend. In PT the same source is HDR emission before tone mapping,
    // so the rain-only calibration is deliberately applied here, live per frame.
    constants.kenshiRainEmissionScale = KenshiOptions::kenshiRainBrightness() * std::clamp(
      KenshiOptions::kenshiFogSunDir().y * 5.0f + 0.2f, 0.1f, 1.0f);

    // Publish each active interior mask shell as an oriented box. The world placement arrives already
    // resolved (see below).
    constants.kenshiInteriorClipCount = 0u;
    constants.kenshiInteriorClipDebugAll =
      (KenshiOptions::kenshiInteriorClip() && KenshiOptions::kenshiInteriorClipDebugAll()) ? 1u : 0u;
    if (KenshiOptions::kenshiInteriorClip()) {
      const std::vector<SceneManager::KenshiInteriorVolume> volumes =
        SceneManager::getKenshiInteriorVolumes();

      const float bias = KenshiOptions::kenshiInteriorClipBias();

      auto writeRow = [&](uint32_t slot, const Vector4& row) {
        for (uint32_t component = 0; component < 4u; ++component) {
          const float value = row[component];
          switch (slot * 4u + component) {
            case 0: constants.kenshiInteriorB0R0X = value; break;
            case 1: constants.kenshiInteriorB0R0Y = value; break;
            case 2: constants.kenshiInteriorB0R0Z = value; break;
            case 3: constants.kenshiInteriorB0R0W = value; break;
            case 4: constants.kenshiInteriorB0R1X = value; break;
            case 5: constants.kenshiInteriorB0R1Y = value; break;
            case 6: constants.kenshiInteriorB0R1Z = value; break;
            case 7: constants.kenshiInteriorB0R1W = value; break;
            case 8: constants.kenshiInteriorB0R2X = value; break;
            case 9: constants.kenshiInteriorB0R2Y = value; break;
            case 10: constants.kenshiInteriorB0R2Z = value; break;
            case 11: constants.kenshiInteriorB0R2W = value; break;
            case 12: constants.kenshiInteriorB1R0X = value; break;
            case 13: constants.kenshiInteriorB1R0Y = value; break;
            case 14: constants.kenshiInteriorB1R0Z = value; break;
            case 15: constants.kenshiInteriorB1R0W = value; break;
            case 16: constants.kenshiInteriorB1R1X = value; break;
            case 17: constants.kenshiInteriorB1R1Y = value; break;
            case 18: constants.kenshiInteriorB1R1Z = value; break;
            case 19: constants.kenshiInteriorB1R1W = value; break;
            case 20: constants.kenshiInteriorB1R2X = value; break;
            case 21: constants.kenshiInteriorB1R2Y = value; break;
            case 22: constants.kenshiInteriorB1R2Z = value; break;
            case 23: constants.kenshiInteriorB1R2W = value; break;
            case 24: constants.kenshiInteriorB2R0X = value; break;
            case 25: constants.kenshiInteriorB2R0Y = value; break;
            case 26: constants.kenshiInteriorB2R0Z = value; break;
            case 27: constants.kenshiInteriorB2R0W = value; break;
            case 28: constants.kenshiInteriorB2R1X = value; break;
            case 29: constants.kenshiInteriorB2R1Y = value; break;
            case 30: constants.kenshiInteriorB2R1Z = value; break;
            case 31: constants.kenshiInteriorB2R1W = value; break;
            case 32: constants.kenshiInteriorB2R2X = value; break;
            case 33: constants.kenshiInteriorB2R2Y = value; break;
            case 34: constants.kenshiInteriorB2R2Z = value; break;
            case 35: constants.kenshiInteriorB2R2W = value; break;
            case 36: constants.kenshiInteriorB3R0X = value; break;
            case 37: constants.kenshiInteriorB3R0Y = value; break;
            case 38: constants.kenshiInteriorB3R0Z = value; break;
            case 39: constants.kenshiInteriorB3R0W = value; break;
            case 40: constants.kenshiInteriorB3R1X = value; break;
            case 41: constants.kenshiInteriorB3R1Y = value; break;
            case 42: constants.kenshiInteriorB3R1Z = value; break;
            case 43: constants.kenshiInteriorB3R1W = value; break;
            case 44: constants.kenshiInteriorB3R2X = value; break;
            case 45: constants.kenshiInteriorB3R2Y = value; break;
            case 46: constants.kenshiInteriorB3R2Z = value; break;
            case 47: constants.kenshiInteriorB3R2W = value; break;
            default: break;
          }
        }
      };

      uint32_t written = 0u;
      for (const SceneManager::KenshiInteriorVolume& volume : volumes) {
        if (written >= 4u)
          break;
        if (!volume.valid)
          continue;

        // The placement arrives resolved on the D3D11 side against Kenshi's own same-frame view-projection. No
        // camera is consulted here and no staleness window applies: a static building's transform does not
        // expire, and the set is cleared when its shell stops being drawn.
        const Vector3 objCentre(
          0.5f * (volume.objMin[0] + volume.objMax[0]),
          0.5f * (volume.objMin[1] + volume.objMax[1]),
          0.5f * (volume.objMin[2] + volume.objMax[2]));

        const Matrix4 objectToWorld = volume.objectToWorld;

        // WVP = VP * world with `world` affine, so dividing VP out must return
        // something affine: bottom row (0,0,0,1). A wrong convention, a
        // mismatched camera or a projective result all break that and are
        // rejected rather than published. The failure mode is "nothing is
        // culled", never "the cut follows the camera".
        const Vector4 bottomRow(objectToWorld[0][3], objectToWorld[1][3],
                                objectToWorld[2][3], objectToWorld[3][3]);
        const float affineError = std::abs(bottomRow.x) + std::abs(bottomRow.y)
                                + std::abs(bottomRow.z) + std::abs(bottomRow.w - 1.0f);

        const Vector4 centre4 = objectToWorld * Vector4(objCentre.x, objCentre.y, objCentre.z, 1.0f);
        Vector3 centre(0.0f);
        bool centreFinite = false;
        if (std::abs(centre4.w) > 1e-6f) {
          centre = Vector3(centre4.x / centre4.w, centre4.y / centre4.w, centre4.z / centre4.w);
          centreFinite = std::isfinite(centre.x) && std::isfinite(centre.y)
                      && std::isfinite(centre.z);
        }

        constexpr float kAffineTolerance = 1e-2f;
        const bool affine = affineError < kAffineTolerance;
        const bool plausible = affine && centreFinite;
        const Matrix4 bestObjectToWorld = objectToWorld;

        if (plausible) {
          const Matrix4 worldToObject = inverse(bestObjectToWorld);

        // Fold the object-space box into the transform so the shader only has
          // to test |q| <= 1: scale each row by the half-extent and offset by the
          // centre. The bias inflates the box in object units.
          bool usable = true;
          Vector4 rows[3];
          for (uint32_t axis = 0; axis < 3u && usable; ++axis) {
            const float centreAxis = 0.5f * (volume.objMin[axis] + volume.objMax[axis]);
            const float halfExtent = 0.5f * (volume.objMax[axis] - volume.objMin[axis]) + bias;
            if (!(halfExtent > 1e-4f)) {
              usable = false;
              break;
            }
            const float rcpHalfExtent = 1.0f / halfExtent;
            rows[axis] = Vector4(
              worldToObject[0][axis] * rcpHalfExtent,
              worldToObject[1][axis] * rcpHalfExtent,
              worldToObject[2][axis] * rcpHalfExtent,
              (worldToObject[3][axis] - centreAxis) * rcpHalfExtent);
            for (uint32_t component = 0; component < 4u; ++component)
              usable = usable && std::isfinite(rows[axis][component]);
          }

          if (usable) {
            for (uint32_t axis = 0; axis < 3u; ++axis)
              writeRow(written * 3u + axis, rows[axis]);
            ++written;
          }
        }

        if ((kenshi_telemetry::enabled() && KenshiOptions::kenshiLogInteriorProbe())) {
          static uint32_t sInteriorWorldLogCount = 0u;
          if (sInteriorWorldLogCount < 16u) {
            ++sInteriorWorldLogCount;
            KENSHI_DIAGNOSTIC_INFO(str::format(
              "[RtxContext][interior] shell placed:",
              " affine=", affine ? 1u : 0u,
              " affineErr=", affineError,
              " plausible=", plausible ? 1u : 0u,
              " slot=", written,
              " verts=", volume.vertexCount,
              // Must be CONSTANT while the camera moves. It is derived from the
              // game's own view-projection now, so drift here means the capture
              // is wrong, not the camera.
              " centre=(", centre.x, ",", centre.y, ",", centre.z, ")",
              " objExtent=(", volume.objMax[0] - volume.objMin[0], ",",
                              volume.objMax[1] - volume.objMin[1], ",",
                              volume.objMax[2] - volume.objMin[2], ")"));
          }
        }
      }

      constants.kenshiInteriorClipCount = written;
    }

    constants.eyeArgs.enableEyes = RtxOptions::Eye::enable();
    constants.eyeArgs.normalBendingEyeball = RtxOptions::Eye::eyeballSphereOffset();
    constants.eyeArgs.normalBendingCornea = RtxOptions::Eye::corneaSphereOffset();
    constants.eyeArgs.whitesAlbedoScale = RtxOptions::Eye::eyeWhitesAlbedoScale();
    constants.eyeArgs.irisRadius = RtxOptions::Eye::irisRadius();
    constants.eyeArgs.irisDepth = RtxOptions::Eye::irisDepth();

    constants.shadowTerminatorSoften = RtxOptions::ShadowTerminator::soften();

    // Note: shadow terminator image are allocated/freed based on this RtxOption
    constants.shadowTerminatorEnableOffset = RtxOptions::ShadowTerminator::enableOffset();
    constants.shadowTerminatorMaxArea = std::max(0.f, RtxOptions::ShadowTerminator::maxArea() * RtxOptions::getMeterToWorldUnitScale() * RtxOptions::getMeterToWorldUnitScale());
    constants.shadowTerminatorMaxLength = std::max(0.f, RtxOptions::ShadowTerminator::maxLength() * RtxOptions::getMeterToWorldUnitScale());

    // Upload the constants to the GPU
    {
      Rc<DxvkBuffer> cb = getResourceManager().getConstantsBuffer();

      writeToBuffer(cb, 0, sizeof(constants), &constants);

      m_cmd->trackResource<DxvkAccess::Write>(cb);
    }
  }

  void RtxContext::bindCommonRayTracingResources(const Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();


    Rc<DxvkBuffer> constantsBuffer = getResourceManager().getConstantsBuffer();
    Rc<DxvkBuffer> surfaceBuffer = getSceneManager().getSurfaceBuffer();
    Rc<DxvkBuffer> kenshiBloodBuffer = getSceneManager().getKenshiBloodBuffer(this);
    Rc<DxvkBuffer> surfaceMappingBuffer = getSceneManager().getSurfaceMappingBuffer();
    Rc<DxvkBuffer> billboardsBuffer = getSceneManager().getBillboardsBuffer();
    Rc<DxvkBuffer> surfaceMaterialBuffer = getSceneManager().getSurfaceMaterialBuffer();
    Rc<DxvkBuffer> surfaceMaterialExtensionBuffer = getSceneManager().getSurfaceMaterialExtensionBuffer();
    Rc<DxvkBuffer> volumeMaterialBuffer = getSceneManager().getVolumeMaterialBuffer();
    Rc<DxvkBuffer> lightBuffer = getSceneManager().getLightManager().getLightBuffer();
    Rc<DxvkBuffer> previousLightBuffer = getSceneManager().getLightManager().getPreviousLightBuffer();
    Rc<DxvkBuffer> lightMappingBuffer = getSceneManager().getLightManager().getLightMappingBuffer();
    Rc<DxvkBuffer> gpuPrintBuffer = getResourceManager().getRaytracingOutput().m_gpuPrintBuffer;
    Rc<DxvkImageView> valueNoiseLut = getResourceManager().getValueNoiseLut(this);
    Rc<DxvkSampler> linearSampler = getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    Rc<DxvkBuffer> samplerFeedbackBuffer = getResourceManager().getRaytracingOutput().m_samplerFeedbackDevice;

    DebugView& debugView = getCommonObjects()->metaDebugView();

    bindAccelerationStructure(BINDING_ACCELERATION_STRUCTURE, getResourceManager().getTLAS(Tlas::Opaque).accelStructure);
    // A surviving TLAS handle does not retain the BLAS of a cleared scene.
    const auto& opaqueTlas = getResourceManager().getTLAS(Tlas::Opaque);
    const bool usePreviousScene = RtxOptions::enablePreviousTLAS() && getSceneManager().isPreviousFrameSceneAvailable();
    bindAccelerationStructure(BINDING_ACCELERATION_STRUCTURE_PREVIOUS,
      usePreviousScene && opaqueTlas.previousAccelStructure.ptr() ? opaqueTlas.previousAccelStructure : opaqueTlas.accelStructure);
    bindAccelerationStructure(BINDING_ACCELERATION_STRUCTURE_UNORDERED, getResourceManager().getTLAS(Tlas::Unordered).accelStructure);
    bindAccelerationStructure(BINDING_ACCELERATION_STRUCTURE_SSS, getResourceManager().getTLAS(Tlas::SSS).accelStructure);
    bindResourceBuffer(BINDING_SURFACE_DATA_BUFFER, DxvkBufferSlice(surfaceBuffer, 0, surfaceBuffer->info().size));
    bindResourceBuffer(BINDING_KENSHI_BLOOD_BUFFER, DxvkBufferSlice(kenshiBloodBuffer, 0, kenshiBloodBuffer->info().size));
    bindResourceBuffer(BINDING_SURFACE_MAPPING_BUFFER, DxvkBufferSlice(surfaceMappingBuffer, 0, surfaceMappingBuffer.ptr() ? surfaceMappingBuffer->info().size : 0));
    bindResourceBuffer(BINDING_SURFACE_MATERIAL_DATA_BUFFER, DxvkBufferSlice(surfaceMaterialBuffer, 0, surfaceMaterialBuffer->info().size));

    // Kenshi terrain parameter sets.
    {
      Rc<DxvkBuffer> kenshiTerrainBuffer = getSceneManager().getKenshiTerrainBuffer(this);
        bindResourceBuffer(BINDING_KENSHI_TERRAIN_BUFFER,
        DxvkBufferSlice(kenshiTerrainBuffer, 0, kenshiTerrainBuffer->info().size));

      // Same never-null contract as the terrain buffer above: the hit shader declares
      // kenshiTerrainBloodBuffer unconditionally.
      Rc<DxvkBuffer> kenshiTerrainBloodBuffer = getSceneManager().getKenshiTerrainBloodBuffer(this);
      bindResourceBuffer(BINDING_KENSHI_TERRAIN_BLOOD_BUFFER,
        DxvkBufferSlice(kenshiTerrainBloodBuffer, 0, kenshiTerrainBloodBuffer->info().size));

      // Same never-null contract; an empty header reads as zero volumes.
      Rc<DxvkBuffer> kenshiInteriorBuffer = getSceneManager().getKenshiInteriorBuffer(this);
      bindResourceBuffer(BINDING_KENSHI_INTERIOR_BUFFER,
        DxvkBufferSlice(kenshiInteriorBuffer, 0, kenshiInteriorBuffer->info().size));

      Rc<DxvkBuffer> kenshiWaterZoneBuffer = getSceneManager().getKenshiWaterZoneBuffer(this);
      bindResourceBuffer(BINDING_KENSHI_WATER_ZONE_BUFFER,
        DxvkBufferSlice(kenshiWaterZoneBuffer, 0, kenshiWaterZoneBuffer->info().size));
    }
    bindResourceBuffer(BINDING_SURFACE_MATERIAL_EXT_DATA_BUFFER, surfaceMaterialExtensionBuffer.ptr() ? DxvkBufferSlice(surfaceMaterialExtensionBuffer, 0, surfaceMaterialExtensionBuffer->info().size) : DxvkBufferSlice());
    bindResourceBuffer(BINDING_VOLUME_MATERIAL_DATA_BUFFER, volumeMaterialBuffer.ptr() ? DxvkBufferSlice(volumeMaterialBuffer, 0, volumeMaterialBuffer->info().size) : DxvkBufferSlice());
    bindResourceBuffer(BINDING_LIGHT_DATA_BUFFER, DxvkBufferSlice(lightBuffer, 0, lightBuffer.ptr() ? lightBuffer->info().size : 0));
    bindResourceBuffer(BINDING_PREVIOUS_LIGHT_DATA_BUFFER, DxvkBufferSlice(previousLightBuffer, 0, previousLightBuffer.ptr() ? previousLightBuffer->info().size : 0));
    bindResourceBuffer(BINDING_LIGHT_MAPPING, DxvkBufferSlice(lightMappingBuffer, 0, lightMappingBuffer.ptr() ? lightMappingBuffer->info().size : 0));
    bindResourceBuffer(BINDING_BILLBOARDS_BUFFER, DxvkBufferSlice(billboardsBuffer, 0, billboardsBuffer.ptr() ? billboardsBuffer->info().size : 0));
    bindResourceView(BINDING_BLUE_NOISE_TEXTURE, getResourceManager().getBlueNoiseTexture(this), nullptr);
    bindResourceBuffer(BINDING_CONSTANTS, DxvkBufferSlice(constantsBuffer, 0, constantsBuffer->info().size));
    bindResourceView(BINDING_DEBUG_VIEW_TEXTURE, debugView.getDebugOutput(), nullptr);
    bindResourceBuffer(BINDING_GPU_PRINT_BUFFER, DxvkBufferSlice(gpuPrintBuffer, 0, gpuPrintBuffer.ptr() ? gpuPrintBuffer->info().size : 0));
    bindResourceView(BINDING_VALUE_NOISE_SAMPLER, valueNoiseLut, nullptr);
    bindResourceSampler(BINDING_VALUE_NOISE_SAMPLER, linearSampler);
    bindResourceBuffer(BINDING_SAMPLER_READBACK_BUFFER, DxvkBufferSlice(samplerFeedbackBuffer, 0, samplerFeedbackBuffer.ptr() ? samplerFeedbackBuffer->info().size : 0));

    // Atmosphere LUTs are declared in common bindings and must always be bound.
    if (!m_atmosphere) {
      m_atmosphere = std::make_unique<RtxAtmosphere>(m_device.ptr());
    }

    m_atmosphere->initialize(this);

    auto transmittanceLut = m_atmosphere->getTransmittanceLut();
    auto multiscatteringLut = m_atmosphere->getMultiscatteringLut();
    auto skyViewLut = m_atmosphere->getSkyViewLut();

    if (transmittanceLut.isValid()) {
      bindResourceView(BINDING_ATMOSPHERE_TRANSMITTANCE_LUT, transmittanceLut.view, nullptr);
    }

    if (multiscatteringLut.isValid()) {
      bindResourceView(BINDING_ATMOSPHERE_MULTISCATTERING_LUT, multiscatteringLut.view, nullptr);
    }

    if (skyViewLut.isValid()) {
      bindResourceView(BINDING_ATMOSPHERE_SKY_VIEW_LUT, skyViewLut.view, nullptr);
    }
  }

  void RtxContext::bindResourceView(const uint32_t slot, const Rc<DxvkImageView>& imageView, const Rc<DxvkBufferView>& bufferView)
  {
    DxvkContext::bindResourceView(slot, imageView, bufferView);

#ifdef REMIX_DEVELOPMENT
    // Cache resources for aliasing
    cacheResourceAliasingImageView(imageView);
#endif
  }

  void RtxContext::checkOpacityMicromapSupport() {
    bool isOpacityMicromapSupported = OpacityMicromapManager::checkIsOpacityMicromapSupported(*m_device);

    RtxOptions::setIsOpacityMicromapSupported(isOpacityMicromapSupported);

    KENSHI_DIAGNOSTIC_INFO(str::format("[RTX info] Opacity Micromap: ", isOpacityMicromapSupported ? "supported" : "not supported"));
  }

  bool RtxContext::checkIsShaderExecutionReorderingSupported(DxvkDevice& device) {
    if (!RtxOptions::isShaderExecutionReorderingSupported()) {
      return false;
    }

    // SER Extension support check
    const bool isSERExtensionSupported = device.extensions().nvRayTracingInvocationReorder;
    const bool isSERReorderingEnabled =
      VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_NV == device.properties().nvRayTracingInvocationReorderProperties.rayTracingInvocationReorderReorderingHint;
      
    return isSERExtensionSupported && isSERReorderingEnabled;
  }

  void RtxContext::checkShaderExecutionReorderingSupport() {
    const bool isSERSupported = checkIsShaderExecutionReorderingSupported(*m_device);
    
    RtxOptions::enableShaderExecutionReordering = isSERSupported;

    const VkPhysicalDeviceProperties& props = m_device->adapter()->deviceProperties();
    const NV_GPU_ARCHITECTURE_ID archId = RtxOptions::getNvidiaArch();

    KENSHI_DIAGNOSTIC_INFO(str::format("[RTX info] Shader Execution Reordering: ", isSERSupported ? "supported" : "not supported"));

    bool isShaderExecutionReorderingEnabled = RtxOptions::isShaderExecutionReorderingInPathtracerGbufferEnabled() ||
      RtxOptions::isShaderExecutionReorderingInPathtracerIntegrateIndirectEnabled();

    KENSHI_DIAGNOSTIC_INFO(str::format("[RTX info] Shader Execution Reordering: ", isShaderExecutionReorderingEnabled ? "enabled" : "disabled"));
  }

  void RtxContext::checkNeuralRadianceCacheSupport() {
    // DX11_V319_NRC_OPT_IN_IS_AUTHORITATIVE: enforce the opt-in wherever the mode
    // came from, not just when a graphics preset picked it.
    //
    // DX11_V244 established that NRC crashes on first use on the DX11 path
    // (REMIX-4105) and made the PRESET refuse to enable it without
    // DXVK_REMIX_ENABLE_NRC=1. But that guard only covers the preset: the mode is
    // a plain RtxOption, so the in-game UI, an rtx.conf entry, or any option layer
    // could still select it, and nothing downstream re-checked. The support test
    // below does not catch it either - the hardware genuinely does support NRC.
    //
    // Field evidence (Skyrim SE, this build): no DXVK_REMIX_ENABLE_NRC anywhere in
    // the environment and no NRC entry in rtx.conf, yet "Integrate Indirect Mode:
    // Neural Radiance Cache - activated", followed by "[RTX Neural Radiance Cache]
    // EndFrame call failed", a GPU fault whose faulting address resolved to
    // write-invalid gpuVA=0x0 (a write through a null device address), and 46
    // consecutive VK_ERROR_DEVICE_LOST submissions. That is the REMIX-4105
    // signature, and it is what "the game hangs and does not act right" looks like
    // from the outside.
    //
    // Falling back here makes the opt-in mean what DX11_V244 intended: NRC is
    // reachable only when it is explicitly asked for, no matter which layer set it.
    if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache
     && env::getEnvVar("DXVK_REMIX_ENABLE_NRC") != "1") {
      Logger::warn(
        "[RTX] Neural Radiance Cache was selected without the explicit opt-in and is unstable on the "
        "DX11 path (REMIX-4105: null-address GPU write and device loss on the first NRC frame). "
        "Switching indirect illumination mode to ReSTIR GI. Set DXVK_REMIX_ENABLE_NRC=1 to force it.");
      // Same reasoning as the unsupported case below: this must take effect before
      // a frame is dispatched, so setImmediately on the Quality layer to override
      // whichever layer selected NRC.
      RtxOptions::integrateIndirectMode.setImmediately(IntegrateIndirectMode::ReSTIRGI, RtxOptionLayer::getQualityLayer());
      return;
    }

    // Update RtxOption selection if Neural Radiance Cache was selected but it's not supported
    if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache &&
        !NeuralRadianceCache::checkIsSupported(m_device.ptr())) {

      // Fallback to ReSTIRGI
      Logger::warn(str::format("[RTX] Neural Radiance Cache is not supported. Switching indirect illumination mode to ReSTIR GI."));
      // TODO[REMIX-4105] trying to use NRC for a frame when it isn't supported will cause a crash, so this needs to be setImmediately.
      // Should refactor this to use a separate global for the final state, and indicate user preference with the option.
      // Use Quality layer to ensure this overrides the Environment layer (where env vars are stored).
      RtxOptions::integrateIndirectMode.setImmediately(IntegrateIndirectMode::ReSTIRGI, RtxOptionLayer::getQualityLayer());
    }
  }

  void RtxContext::dispatchVolumetrics(const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(this, "Volumetrics");
    setFramePassStage(RtxFramePassStage::Volumetrics);

    // Volume Raytracing
    {
      m_common->metaGlobalVolumetrics().dispatch(this, rtOutput, rtOutput.m_raytraceArgs.volumeArgs.numActiveFroxelVolumes);
    }
  }

  void RtxContext::dispatchIntegrate(const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(this, "Integrate Raytracing");

    // Integrate direct
    m_common->metaPathtracerIntegrateDirect().dispatch(this, rtOutput);

    // RTXDI Gradient pass
    m_common->metaRtxdiRayQuery().dispatchGradient(this, rtOutput);

    // Integrate indirect
    {
      ScopedGpuProfileZone(this, "Integrate Indirect Raytracing");
      setFramePassStage(RtxFramePassStage::IndirectIntegration);
      
      m_common->metaPathtracerIntegrateIndirect().dispatch(this, rtOutput);
    }

    // Integrate indirect - NEE Cache pass. This dispatch has no enable gate upstream, so it would run
    // every frame even with rtx.neeCache.enable off (NeeCachePass::dispatch having returned early),
    // integrating a cache that is never updated.
    if (!KenshiOptions::kenshiGateNeeIntegration() || NeeCachePass::enable()) {
      m_common->metaPathtracerIntegrateIndirect().dispatchNEE(this, rtOutput);
    }
  }

  void RtxContext::dispatchPathTracing(const Resources::RaytracingOutput& rtOutput) {
    kenshi_fault::checkpoint(this,kenshi_fault::PathTrace,m_device->getCurrentFrameId());

    // Gbuffer Raytracing
    m_common->metaPathtracerGbuffer().dispatch(this, rtOutput);

    // RTXDI
    m_common->metaRtxdiRayQuery().dispatch(this, rtOutput);

    // NEE Cache
    dispatchNeeCache(rtOutput);

    // Integration Raytracing
    dispatchIntegrate(rtOutput);
  }
  
  void RtxContext::dispatchDemodulate(const Resources::RaytracingOutput& rtOutput) {
    kenshi_fault::checkpoint(this,kenshi_fault::Demodulate,m_device->getCurrentFrameId());
    ScopedCpuProfileZone();
    DemodulatePass& demodulate = m_common->metaDemodulate();
    demodulate.dispatch(this, rtOutput);
  }

  void RtxContext::dispatchNeeCache(const Resources::RaytracingOutput& rtOutput) {
    NeeCachePass& neeCache = m_common->metaNeeCache();
    neeCache.dispatch(this, rtOutput);
  }

  void RtxContext::dispatchDenoise(const Resources::RaytracingOutput& rtOutput) {
    kenshi_fault::checkpoint(this,kenshi_fault::Denoise,m_device->getCurrentFrameId());
    auto& rayReconstruction = getCommonObjects()->metaRayReconstruction();

    // Primary direct denoiser used for primary direct lighting when separated, otherwise a special combined direct+indirect denoiser is used when both direct and indirect signals are combined.
    DxvkDenoise& denoiser0 = RtxOptions::denoiseDirectAndIndirectLightingSeparately() ? m_common->metaPrimaryDirectLightDenoiser() : m_common->metaPrimaryCombinedLightDenoiser();
    DxvkDenoise& referenceDenoiserSecondLobe0 = m_common->metaReferenceDenoiserSecondLobe0();
    // Primary Indirect denoiser used for primary indirect lighting when separated.
    DxvkDenoise& denoiser1 = m_common->metaPrimaryIndirectLightDenoiser();
    DxvkDenoise& referenceDenoiserSecondLobe1 = m_common->metaReferenceDenoiserSecondLobe1();
    // Secondary combined denoiser always used for secondary lighting.
    DxvkDenoise& denoiser2 = m_common->metaSecondaryCombinedLightDenoiser();
    DxvkDenoise& referenceDenoiserSecondLobe2 = m_common->metaReferenceDenoiserSecondLobe2();

    bool shouldDenoise = false;
    if (useRayReconstruction()) {
      shouldDenoise = (rayReconstruction.enableNRDForTraining() && !RtxOptions::useDenoiserReferenceMode()) || rayReconstruction.preprocessSecondarySignal();
    } else {
      shouldDenoise = RtxOptions::useDenoiser() && !RtxOptions::useDenoiserReferenceMode();
    }

    if (!shouldDenoise) {
      denoiser0.releaseResources();
      denoiser1.releaseResources();
      denoiser2.releaseResources();
      referenceDenoiserSecondLobe0.releaseResources();
      referenceDenoiserSecondLobe1.releaseResources();
      referenceDenoiserSecondLobe2.releaseResources();
      return;
    }

    ScopedGpuProfileZone(this, "Denoising");
    setFramePassStage(RtxFramePassStage::NRD);

    auto runDenoising = [&](DxvkDenoise& denoiser, DxvkDenoise& secondLobeReferenceDenoiser, DxvkDenoise::Input& denoiseInput, DxvkDenoise::Output& denoiseOutput) {
      // Since NRDContext doesn't use DxvkContext abstraction
      // but its using Compute, mark its DxvkContext's Cp pipelines as dirty
      {
        this->spillRenderPass(false);
        m_flags.set(
          DxvkContextFlag::CpDirtyPipeline,
          DxvkContextFlag::CpDirtyPipelineState,
          DxvkContextFlag::CpDirtyResources,
          DxvkContextFlag::CpDirtyDescriptorBinding);
      }

      // Need to run the denoiser twice for diffuse and specular when reference denoising is enabled on non-combined inputs
      if (denoiser.isReferenceDenoiserEnabled()) {
        denoiseInput.reference = denoiseInput.diffuse_hitT;
        denoiseOutput.reference = denoiseOutput.diffuse_hitT;
        denoiser.dispatch(this, m_execBarriers, rtOutput, denoiseInput, denoiseOutput);

        // Reference denoiser accumulates internally, so the second signal has to be denoised through a separate reference denoiser
        secondLobeReferenceDenoiser.copyNrdSettingsFrom(denoiser);
        denoiseInput.reference = denoiseInput.specular_hitT;
        denoiseOutput.reference = denoiseOutput.specular_hitT;
        secondLobeReferenceDenoiser.dispatch(this, m_execBarriers, rtOutput, denoiseInput, denoiseOutput);
      } else
        denoiser.dispatch(this, m_execBarriers, rtOutput, denoiseInput, denoiseOutput);
    };

    const bool isSecondaryOnly = rayReconstruction.denoiseSecondarySignalWithExternalDenoiser();

    // Primary Direct light denoiser
    if (!isSecondaryOnly)
    {
      ScopedGpuProfileZone(this, "Primary Direct Denoising");
      
      DxvkDenoise::Input denoiseInput = {};
      denoiseInput.diffuse_hitT = &rtOutput.m_primaryDirectDiffuseRadiance.resource(Resources::AccessType::Read);
      denoiseInput.specular_hitT = &rtOutput.m_primaryDirectSpecularRadiance.resource(Resources::AccessType::Read);
      denoiseInput.normal_roughness = &rtOutput.m_primaryVirtualWorldShadingNormalPerceptualRoughnessDenoising.resource(Resources::AccessType::Read);
      denoiseInput.linearViewZ = &rtOutput.m_primaryLinearViewZ;
      denoiseInput.motionVector = &rtOutput.m_primaryVirtualMotionVector.resource(Resources::AccessType::Read);
      denoiseInput.disocclusionThresholdMix = &rtOutput.m_primaryDisocclusionThresholdMix;
      denoiseInput.reset = m_resetHistory;

      if (RtxOptions::useRTXDI() && m_common->metaRtxdiRayQuery().getEnableDenoiserConfidence(*this)) {
        denoiseInput.confidence = &rtOutput.getCurrentRtxdiConfidence().resource(Resources::AccessType::Read);
      }

      DxvkDenoise::Output denoiseOutput;
      denoiseOutput.diffuse_hitT = &rtOutput.m_primaryDirectDiffuseRadiance.resource(Resources::AccessType::Write);
      denoiseOutput.specular_hitT = &rtOutput.m_primaryDirectSpecularRadiance.resource(Resources::AccessType::Write);

      runDenoising(denoiser0, referenceDenoiserSecondLobe0, denoiseInput, denoiseOutput);
    } else {
      denoiser0.releaseResources();
      referenceDenoiserSecondLobe0.releaseResources();
    }

    // Primary Indirect light denoiser, if separate denoiser is used.
    if (RtxOptions::denoiseDirectAndIndirectLightingSeparately() && !isSecondaryOnly)
    {
      ScopedGpuProfileZone(this, "Primary Indirect Denoising");

      DxvkDenoise::Input denoiseInput = {};
      denoiseInput.diffuse_hitT = &rtOutput.m_primaryIndirectDiffuseRadiance.resource(Resources::AccessType::Read);
      denoiseInput.specular_hitT = &rtOutput.m_primaryIndirectSpecularRadiance.resource(Resources::AccessType::Read);
      denoiseInput.normal_roughness = &rtOutput.m_primaryVirtualWorldShadingNormalPerceptualRoughnessDenoising.resource(Resources::AccessType::Read);
      denoiseInput.linearViewZ = &rtOutput.m_primaryLinearViewZ;
      denoiseInput.motionVector = &rtOutput.m_primaryVirtualMotionVector.resource(Resources::AccessType::Read);
      denoiseInput.disocclusionThresholdMix = &rtOutput.m_primaryDisocclusionThresholdMix;
      denoiseInput.reset = m_resetHistory;

      DxvkDenoise::Output denoiseOutput;
      denoiseOutput.diffuse_hitT = &rtOutput.m_primaryIndirectDiffuseRadiance.resource(Resources::AccessType::Write);
      denoiseOutput.specular_hitT = &rtOutput.m_primaryIndirectSpecularRadiance.resource(Resources::AccessType::Write);

      runDenoising(denoiser1, referenceDenoiserSecondLobe1, denoiseInput, denoiseOutput);
    } else {
      denoiser1.releaseResources();
      referenceDenoiserSecondLobe1.releaseResources();
    }

    // Secondary Combined light denoiser
    {
      ScopedGpuProfileZone(this, "Secondary Combined Denoising");

      DxvkDenoise::Input denoiseInput = {};
      denoiseInput.diffuse_hitT = &rtOutput.m_secondaryCombinedDiffuseRadiance.resource(Resources::AccessType::Read);
      denoiseInput.specular_hitT = &rtOutput.m_secondaryCombinedSpecularRadiance.resource(Resources::AccessType::Read);
      denoiseInput.normal_roughness = &rtOutput.m_secondaryVirtualWorldShadingNormalPerceptualRoughnessDenoising;
      denoiseInput.linearViewZ = &rtOutput.m_secondaryLinearViewZ;
      denoiseInput.motionVector = &rtOutput.m_secondaryVirtualMotionVector.resource(Resources::AccessType::Read);
      denoiseInput.reset = m_resetHistory;

      DxvkDenoise::Output denoiseOutput;
      denoiseOutput.diffuse_hitT = &rtOutput.m_secondaryCombinedDiffuseRadiance.resource(Resources::AccessType::Write);
      denoiseOutput.specular_hitT = &rtOutput.m_secondaryCombinedSpecularRadiance.resource(Resources::AccessType::Write);

      runDenoising(denoiser2, referenceDenoiserSecondLobe2, denoiseInput, denoiseOutput);
    }
  }

  void RtxContext::dispatchDLSS(const Resources::RaytracingOutput& rtOutput) {
    DxvkDLSS& dlss = m_common->metaDLSS();
    dlss.dispatch(this, m_execBarriers, rtOutput, m_resetHistory);
  }

  void RtxContext::dispatchRayReconstruction(const Resources::RaytracingOutput& rtOutput) {
    kenshi_fault::checkpoint(this,kenshi_fault::Upscale,m_device->getCurrentFrameId());
    DxvkRayReconstruction& rayReconstruction = m_common->metaRayReconstruction();
    rayReconstruction.dispatch(this, m_execBarriers, rtOutput, m_resetHistory, GlobalTime::get().deltaTimeMs());
  }

  void RtxContext::dispatchNIS(const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(this, "NIS");
    setFramePassStage(RtxFramePassStage::NIS);
    m_common->metaNIS().dispatch(this, rtOutput);
  }

  void RtxContext::dispatchXeSS(const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(this, "XeSS");
    setFramePassStage(RtxFramePassStage::XeSS);
    DxvkXeSS& xess = m_common->metaXeSS();
    xess.dispatch(this, m_execBarriers, rtOutput, m_resetHistory);
  }

  void RtxContext::dispatchTemporalAA(const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(this, "TAA");
    setFramePassStage(RtxFramePassStage::TAA);

    DxvkTemporalAA& taa = m_common->metaTAA();
    RtCamera& mainCamera = getSceneManager().getCamera();

    if (taa.isActive() && !mainCamera.isCameraCut()) {
      float jitterOffset[2];
      mainCamera.getJittering(jitterOffset);

      taa.dispatch(this,
        getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
        mainCamera.getShaderConstants().resolution,
        jitterOffset,
        rtOutput.m_compositeOutput.resource(Resources::AccessType::Read),
        rtOutput.m_primaryScreenSpaceMotionVector,
        rtOutput.m_finalOutput.resource(Resources::AccessType::Write),
        true);
    }
  }

  void RtxContext::dispatchComposite(const Resources::RaytracingOutput& rtOutput, bool captureLighting) {
    kenshi_fault::checkpoint(this,kenshi_fault::Composite,m_device->getCurrentFrameId());
    if (getSceneManager().getSurfaceBuffer() == nullptr) {
      return;
    }

    ScopedGpuProfileZone(this, "Composite");
    setFramePassStage(RtxFramePassStage::Composition);

    bool isNRDPreCompositionDenoiserEnabled = RtxOptions::useDenoiser() && !RtxOptions::useDenoiserReferenceMode();

    CompositePass::Settings settings;
    settings.fog = getSceneManager().getFogState();
    settings.isNRDPreCompositionDenoiserEnabled = isNRDPreCompositionDenoiserEnabled;
    settings.useUpscaler = shouldUseUpscaler();
    settings.useDLSS = shouldUseDLSS();
    settings.demodulateRoughness = m_common->metaDemodulate().demodulateRoughness();
    settings.roughnessDemodulationOffset = m_common->metaDemodulate().demodulateRoughnessOffset();
    settings.captureLighting = captureLighting;
    m_common->metaComposite().dispatch(this,
      getSceneManager(),
      rtOutput, settings);
  }

  void RtxContext::dispatchToneMapping(const Resources::RaytracingOutput& rtOutput, bool performSRGBConversion) {
    ScopedCpuProfileZone();

    if (m_common->metaDebugView().debugViewIdx() == DEBUG_VIEW_PRE_TONEMAP_OUTPUT) {
      return;
    }

    // TODO: I think these are unnecessary, and/or should be automatically done within DXVK 
    this->spillRenderPass(false);
    this->unbindComputePipeline();

    DxvkAutoExposure& autoExposure = m_common->metaAutoExposure();    
    autoExposure.dispatch(this, 
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER),
      rtOutput, GlobalTime::get().deltaTimeMs(), performSRGBConversion);

    // We don't reset history for tonemapper on m_resetHistory for easier comparison when toggling raytracing modes.
    // The tone curve shouldn't be too different between raytracing modes, 
    // but the reset of denoised buffers causes wide tone curve differences
    // until it converges and thus making comparison of raytracing mode outputs more difficult
    setFramePassStage(RtxFramePassStage::ToneMapping);
    if (RtxOptions::tonemappingMode() == TonemappingMode::Global) {
      DxvkToneMapping& toneMapper = m_common->metaToneMapping();
      toneMapper.dispatch(this, 
        getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER),
        autoExposure.getExposureTexture().view,
        rtOutput, GlobalTime::get().deltaTimeMs(), performSRGBConversion, autoExposure.enabled());
    }
    DxvkLocalToneMapping& localTonemapper = m_common->metaLocalToneMapping();
    if (localTonemapper.isActive()) {
      localTonemapper.dispatch(this,
        getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
        autoExposure.getExposureTexture().view,
        rtOutput, GlobalTime::get().deltaTimeMs(), performSRGBConversion, autoExposure.enabled());
    }
  }

  void RtxContext::dispatchBloom(const Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();
    DxvkBloom& bloom = m_common->metaBloom();
    if (!bloom.isActive()) {
      return;
    }

    // TODO: just in case, because tonemapping does the same
    this->spillRenderPass(false);
    this->unbindComputePipeline();

    bloom.dispatch(this,
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      rtOutput.m_finalOutput.resource(Resources::AccessType::ReadWrite));
  }

  void RtxContext::dispatchPostFx(Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();
    DxvkPostFx& postFx = m_common->metaPostFx();
    RtCamera& mainCamera = getSceneManager().getCamera();
    if (!postFx.enable()) {
      return;
    }

    postFx.dispatch(this,
      getResourceManager().getSampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      mainCamera.getShaderConstants().resolution,
      RtxOptions::rngSeedWithFrameIndex() ? m_device->getCurrentFrameId() : 0,
      rtOutput,
      mainCamera.isCameraCut());
  }

  void RtxContext::dispatchDebugView(Rc<DxvkImage>& srcImage, const Resources::RaytracingOutput& rtOutput, bool captureScreenImage)  {
    ScopedCpuProfileZone();

    DebugView& debugView = m_common->metaDebugView();
    const uint32_t frameIdx = m_device->getCurrentFrameId();

    if (kenshi_telemetry::enabled() && debugView.gpuPrint.enable()) {
      // Read from the oldest element as it is guaranteed to be written on the GPU by now
      VkDeviceSize offset = ((frameIdx + 1) % kMaxFramesInFlight) * sizeof(GpuPrintBufferElement);
      GpuPrintBufferElement* gpuPrintElement = reinterpret_cast<GpuPrintBufferElement*>(rtOutput.m_gpuPrintBuffer->mapPtr(offset));

      if (gpuPrintElement && gpuPrintElement->isValid()) {
        static std::string previousString = "";
        const std::string newString = str::format("GPU print value [", gpuPrintElement->threadIndex.x, ", ", gpuPrintElement->threadIndex.y, "]: ", Config::generateOptionString(reinterpret_cast<Vector4&>(gpuPrintElement->writtenData)));

        // Avoid spamming the console with the same output
        if (newString != previousString) {
          previousString = newString;

          // Add additional info on which we don't want to differentiate when printing out
          const std::string fullInfoString = str::format("Frame: ", gpuPrintElement->frameIndex, " - ", newString);
          Logger::info(fullInfoString);
        }

        // Invalidate the element so that it's not reused
        gpuPrintElement->invalidate();
      }
    }

    if (!debugView.isActive()) {
      return;
    }

    debugView.dispatch(this,
      getResourceManager().getSampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      srcImage, rtOutput, *m_common);

    if (captureScreenImage) {
      // For overlayed debug views, we preserve the post tonemapping naming since the post tonemapped image is a base image.
      // The benefit is retention of most of the existing testing pipeline.
      if (debugView.getOverlayOnTopOfRenderOutput()) {
        takeScreenshot("rtxImagePostTonemapping", srcImage);
      } else {
        takeScreenshot("rtxImageDebugView", srcImage);
      }
    }
  }

  void RtxContext::dispatchReplaceCompositeWithDebugView(const Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();

    DebugView& debugView = m_common->metaDebugView();

    if (!debugView.isActive()) {
      return;
    }

    debugView.dispatchAfterCompositionPass(this,
      getResourceManager().getSampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      rtOutput, *m_common);
  }

  namespace
  {
    template<typename T>
    T mapAs(const Rc<DxvkBuffer>& buf) {
      if (buf == nullptr) {
        return nullptr;
      }
      return static_cast<T>(buf->mapPtr(0));
    }

    Vector2i rescale(const float(&scale)[2], const Vector2i& pix) {
      return Vector2i {
        static_cast<int>(static_cast<float>(pix.x) * scale[0]),
        static_cast<int>(static_cast<float>(pix.y) * scale[1]),
      };
    }

    struct PixRegion {
      Vector2i from {};
      Vector2i to {};
    };

    PixRegion rescale(const float(&scale)[2], const PixRegion& original) {
      Vector2i from = rescale(scale, original.from);
      Vector2i to = rescale(scale, original.to);
      // if was at least 1 pixel, then rescaled should also contain at least 1 pixel
      if (original.to.x - original.from.x > 0) {
        to.x = std::max(to.x, from.x + 1);
      }
      if (original.to.y - original.from.y > 0) {
        to.y = std::max(to.y, from.y + 1);
      }
      return PixRegion { from, to };
    }

    PixRegion clamp(const VkExtent3D& extent, const PixRegion& original) {
      return PixRegion {
        Vector2i{
          std::clamp<int>(original.from.x, 0, std::min<uint32_t>(INT32_MAX, extent.width)),
          std::clamp<int>(original.from.y, 0, std::min<uint32_t>(INT32_MAX, extent.height)),
        },
        Vector2i{
          std::clamp<int>(original.to.x, 0, std::min<uint32_t>(INT32_MAX, extent.width)),
          std::clamp<int>(original.to.y, 0, std::min<uint32_t>(INT32_MAX, extent.height)),
        },
      };
    }

    std::optional<PixRegion> nonzero(const PixRegion& original) {
      if (original.to.x - original.from.x > 0 &&
          original.to.y - original.from.y > 0) {
        return original;
      }
      return std::nullopt;
    }

    VkOffset3D vkoffset(const PixRegion& r) {
      return VkOffset3D { r.from.x, r.from.y, 0 };
    }

    VkExtent3D vkextent(const PixRegion& request) {
      assert(request.to.x - request.from.x > 0 && request.to.y - request.from.y > 0);
      return VkExtent3D {
        static_cast<uint32_t>(std::max(0, request.to.x - request.from.x)),
        static_cast<uint32_t>(std::max(0, request.to.y - request.from.y)),
        1 };
    }

    // In C++20
    template<typename Func >
    void erase_if(std::vector<std::future<void>>& vec, Func&& predicate) {
      auto newEnd = std::remove_if(vec.begin(), vec.end(), predicate);
      vec.erase(newEnd, vec.end());
    }
  }

  void RtxContext::dispatchObjectPicking(Resources::RaytracingOutput& rtOutput,
                                         const VkExtent3D& srcExtent,
                                         const VkExtent3D& targetExtent) {
    ScopedCpuProfileZone();
    DebugView& debugView = m_common->metaDebugView();
    SceneManager& sceneManager = m_common->getSceneManager();
    const uint32_t frameIdx = m_device->getCurrentFrameId();


    auto enoughTimeHasPassedToDestroy = [&]() {
      if (g_forceKeepObjectPickingImage) {
        return false;
      }
      // there are object picking / highlighting requests, so don't destroy
      if (debugView.ObjectPicking.containsRequests() || 
          debugView.Highlighting.active(frameIdx) || 
          !m_objectPickingReadback.asyncTasks.empty()) {
        return false;
      }
      return true;
    };

    if (rtOutput.m_primaryObjectPicking.isValid()) {
      if (enoughTimeHasPassedToDestroy()) {
        rtOutput.m_primaryObjectPicking = {};
        Logger::debug("Object picking image was destroyed");
        return;
      }
    } else {
      // if object picking image exist
      // and it should be alive
      if (!enoughTimeHasPassedToDestroy()) {
        // create and schedule picking to the next frame
        auto thisRef = Rc<DxvkContext> { this };
        rtOutput.m_primaryObjectPicking =
          Resources::createImageResource(thisRef, "primary object picking", srcExtent, VK_FORMAT_R32_UINT);
        Logger::debug("Object picking image was created");
      }
      return;
    }

    erase_if(m_objectPickingReadback.asyncTasks, [](std::future<void>& f) {
      if (!f.valid()) {
        return true;
      }
      // check status with minimal wait; safe to delete, if it has completed
      return f.wait_for(std::chrono::duration<int>{0}) == std::future_status::ready;
    });


    const Resources::Resource& objectPickingSrc = rtOutput.m_primaryObjectPicking;
    assert(srcExtent == objectPickingSrc.image->info().extent);
    const float downscale[] = {
      srcExtent.width / static_cast<float>(targetExtent.width),
      srcExtent.height / static_cast<float>(targetExtent.height)
    };
    constexpr static VkDeviceSize onePixelInBytes = sizeof(ObjectPickingValue);


    // process one request per frame, to readback in the future
    if (auto request = debugView.ObjectPicking.popRequest()) {
      if (auto pixRegion = nonzero(clamp(srcExtent, rescale(downscale,
                                                            PixRegion { request->pixelFrom, request->pixelTo })))) {
        assert(objectPickingSrc.isValid());
        assert(objectPickingSrc.image->formatInfo()->elementSize == onePixelInBytes);
        assert(getSceneManager().getGlobals().clearColorPicking <= (1ull << (8 * onePixelInBytes)) - 1);

        const VkExtent3D copyExtent = vkextent(*pixRegion);

        auto info = DxvkBufferCreateInfo {};
        {
          info.size = onePixelInBytes * copyExtent.width * copyExtent.height;
          info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
          info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT |
            VK_PIPELINE_STAGE_HOST_BIT;
          info.access = VK_ACCESS_TRANSFER_WRITE_BIT |
            VK_ACCESS_HOST_READ_BIT;
        }

        const VkMemoryPropertyFlags memType =
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

        Rc<DxvkBuffer> readbackDst = m_device->createBuffer(info, memType, DxvkMemoryStats::Category::RTXBuffer, "Picking Readback Buffer");

        auto subres = VkImageSubresourceLayers {};
        {
          subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          subres.mipLevel = 0;
          subres.baseArrayLayer = 0;
          subres.layerCount = 1;
        }
        copyImageToBuffer(
          readbackDst, 0, onePixelInBytes, onePixelInBytes,
          objectPickingSrc.image, subres, vkoffset(*pixRegion), copyExtent);


        this->emitMemoryBarrier(0,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_HOST_BIT,
          VK_ACCESS_HOST_READ_BIT);

        const uint64_t syncValue = ++m_objectPickingReadback.signalValue;
        this->signal(m_objectPickingReadback.signal, syncValue);

        m_objectPickingReadback.asyncTasks.push_back(std::async(
          std::launch::async,
          [this,
          cReadbackDst = std::move(readbackDst),
          cSyncValueToWait = syncValue,
          cCallback = std::move(request->callback)]() {
            // async wait
            this->m_objectPickingReadback.signal->wait(cSyncValueToWait);

            const uint32_t* readback = mapAs<const uint32_t*>(cReadbackDst);
            if (!readback || cReadbackDst->info().size < onePixelInBytes) {
              assert(0);
              cCallback(std::vector<ObjectPickingValue>{}, std::nullopt);
              return;
            }

            auto values = std::vector<ObjectPickingValue> {};
            auto primaryValue = ObjectPickingValue { 0 };
            {
              size_t count = cReadbackDst->info().size / onePixelInBytes;
              values.resize(count);

              memcpy(values.data(), readback, count * onePixelInBytes);
              primaryValue = values[0];

              // sort
              std::sort(values.begin(), values.end());
              // remove consecutive duplicates
              auto endNew = std::unique(values.begin(), values.end());
              values.erase(endNew, values.end());
            }

            auto legacyHashForPrimaryValue = g_allowMappingLegacyHashToObjectPickingValue ?
              m_common->getSceneManager().findLegacyTextureHashByObjectPickingValue(primaryValue) :
              std::optional<XXH64_hash_t>{};

            cCallback(std::move(values), legacyHashForPrimaryValue);
          }
        ));
      } else {
        request->callback(std::vector<ObjectPickingValue>{}, std::nullopt);
      }
    }

    if (auto pixelAndColor = debugView.Highlighting.accessPixelToHighlight(frameIdx)) {
      pixelAndColor->first = rescale(downscale, pixelAndColor->first);
      m_common->metaPostFx().dispatchHighlighting(this,
        rtOutput,
        {},
        pixelAndColor->first,
        pixelAndColor->second);
      return;
    }

    auto [objectPickingValues, color] = debugView.Highlighting.accessObjectPickingValueToHighlight(sceneManager,frameIdx);
    m_common->metaPostFx().dispatchHighlighting(
      this,
      rtOutput,
      std::move(objectPickingValues),
      {},
      color);
  }

  void RtxContext::dispatchDLFG() {
    if (!isDLFGEnabled()) {
      return;
    }

    // force vsync off if DLFG is enabled, as we don't properly support FG + vsync
    if (RtxOptions::enableVsyncState != EnableVsync::Off) {
      RtxOptions::enableVsync.setDeferred(EnableVsync::Off);
      RtxOptions::enableVsyncState = EnableVsync::Off;
    }

    Resources::RaytracingOutput& rtOutput = getResourceManager().getRaytracingOutput();

    DxvkFrameInterpolationInfo dlfgInfo = {
      m_device->getCurrentFrameId(),
      m_device->getCommon()->getSceneManager().getCamera(),
      rtOutput.m_primaryScreenSpaceMotionVector.view,
      rtOutput.m_primaryScreenSpaceMotionVector.image->info().layout,
      rtOutput.m_primaryDepth.view,
      rtOutput.m_primaryDepth.image->info().layout,
      false,
      m_common->metaDLFG().getInterpolatedFrameCount(),
    };
    m_device->setupFrameInterpolation(dlfgInfo);
  }

  void RtxContext::flushCommandList() {
    ScopedCpuProfileZone();

    // flush the residue
    tryHandleSky(nullptr, nullptr);

    m_device->submitCommandList(
      this->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE,
      m_submitContainsInjectRtx,
      m_cachedReflexFrameId);
    
    // Reset this now that we've completed the submission
    m_submitContainsInjectRtx = false;
    
    this->beginRecording(
      m_device->createCommandList());

    getCommonObjects()->metaGeometryUtils().flushCommandList();
  }

  void RtxContext::updateComputeShaderResources() {
    ScopedCpuProfileZone();
    DxvkContext::updateComputeShaderResources();

    auto&& layout = m_state.cp.pipeline->layout();
    if (layout->requiresExtraDescriptorSet()) {
      getSceneManager().getBindlessResourceManager().trackBufferResources(m_cmd.ptr());
      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, layout->pipelineLayout(), 
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Textures),
                                  BINDING_SET_BINDLESS_TEXTURE2D);

      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, layout->pipelineLayout(), 
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Buffers),
                                  BINDING_SET_BINDLESS_RAW_BUFFER);

      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, layout->pipelineLayout(),
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Samplers),
                                  BINDING_SET_BINDLESS_SAMPLER);
    }
  }

  void RtxContext::updateRaytracingShaderResources() {
    ScopedCpuProfileZone();
    DxvkContext::updateRaytracingShaderResources();

    auto&& layout = m_state.rp.pipeline->layout();
    if (layout->requiresExtraDescriptorSet()) {
      getSceneManager().getBindlessResourceManager().trackBufferResources(m_cmd.ptr());
      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout->pipelineLayout(), 
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Textures),
                                  BINDING_SET_BINDLESS_TEXTURE2D);

      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout->pipelineLayout(), 
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Buffers),
                                  BINDING_SET_BINDLESS_RAW_BUFFER);

      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout->pipelineLayout(), 
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Samplers),
                                  BINDING_SET_BINDLESS_SAMPLER);
    }
  }

  bool RtxContext::shouldUseDLSS() const {
    // Note: m_dlssSupported only checks for the presence of some basic extensions, the actual DLSS context needs to be queried to see
    // if a given platform supports DLSS (as this will depend on if it was actually initialized successfully or not). Cases where m_dlssSupported
    // is true but supportsDLSS() is not are for example when the DLSS DLL is missing.
    return RtxOptions::isDLSSEnabled() && m_dlssSupported && m_common->metaDLSS().supportsDLSS();
  }

  bool RtxContext::shouldUseRayReconstruction() const {
    return useRayReconstruction();
  }

  bool RtxContext::shouldUseNIS() const {
    return RtxOptions::isNISEnabled();
  }

  bool RtxContext::shouldUseTAA() const {
    return RtxOptions::isTAAEnabled();
  }

  bool RtxContext::shouldUseXeSS() const {
    return RtxOptions::upscalerType() == UpscalerType::XeSS;
  }

  D3D11RtxVertexCaptureData& RtxContext::allocAndMapVertexCaptureConstantBuffer() {
    DxvkBufferSliceHandle slice = m_rtState.vertexCaptureCB->allocSlice();
    invalidateBuffer(m_rtState.vertexCaptureCB, slice);

    return *static_cast<D3D11RtxVertexCaptureData*>(slice.mapPtr);
  }

  D3D11FixedFunctionVS& RtxContext::allocAndMapFixedFunctionVSConstantBuffer() {
    DxvkBufferSliceHandle slice = m_rtState.vsFixedFunctionCB->allocSlice();
    invalidateBuffer(m_rtState.vsFixedFunctionCB, slice);

    return *static_cast<D3D11FixedFunctionVS*>(slice.mapPtr);
  }
  D3D11SharedPS& RtxContext::allocAndMapPSSharedStateConstantBuffer() {
    DxvkBufferSliceHandle slice = m_rtState.psSharedStateCB->allocSlice();
    invalidateBuffer(m_rtState.psSharedStateCB, slice);

    return *static_cast<D3D11SharedPS*>(slice.mapPtr);
  }

  static bool useKenshiBlackSkyClear() {
    static const bool enabled = [] {
      std::string executable = env::getExeName();
      for (char& ch : executable)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      return executable == "kenshi_x64.exe" || executable == "kenshi_gog_x64.exe" || executable == "kenshi.exe";
    }();
    return enabled;
  }

  void RtxContext::rasterizeToSkyMatte(const DrawParameters& params, const DrawCallState& drawCallState) {
    ScopedGpuProfileZone(this, "rasterizeToSkyMatte");

    const RtCamera& camera = getSceneManager().getCamera();
    const uint32_t* renderResolution = camera.m_renderResolution;

    union UnifiedCB {
      D3D11RtxVertexCaptureData programmablePipeline;
      D3D11FixedFunctionVS fixedFunction;

      UnifiedCB() { }
    };

    UnifiedCB prevCB;

    // DX11_V296_NULL_STATE_CB_GUARD: these Remix-owned constant buffers are
    // never created in the DX11 fork (setConstantBuffers has no caller), and
    // DXBC game shaders would not read them anyway. Dereferencing them here
    // crashed the game whenever a sky draw took the raster path (skybox quads
    // or reprojection disabled). Skip the save/modify/restore and still render
    // the matte - only the clip-space jitter injection is lost, which was a
    // no-op for DXBC shaders regardless.
    const bool hasVertexStateCB = drawCallState.usesVertexShader
      ? m_rtState.vertexCaptureCB != nullptr
      : m_rtState.vsFixedFunctionCB != nullptr;

    if (hasVertexStateCB) {
      if (drawCallState.usesVertexShader) {
        prevCB.programmablePipeline = *static_cast<D3D11RtxVertexCaptureData*>(m_rtState.vertexCaptureCB->mapPtr(0));
      } else {
        prevCB.fixedFunction = *static_cast<D3D11FixedFunctionVS*>(m_rtState.vsFixedFunctionCB->mapPtr(0));
      }
    } else {
      ONCE(KENSHI_DIAGNOSTIC_INFO("[RTX Sky] Rasterizing sky matte without state constant buffers (DX11 runtime); clip-space jitter injection skipped."));
    }

    auto skyMatteView = getResourceManager().getSkyMatte(this, m_skyColorFormat).view;
    const auto skyMatteExt = skyMatteView->mipLevelExtent(0);

    // Update spec constants
    int prevClipSpaceJitterEnabled = -1;
    {
      if (drawCallState.usesVertexShader) {
        prevClipSpaceJitterEnabled = getSpecConstantsInfo(VK_PIPELINE_BIND_POINT_GRAPHICS)
          .specConstants[D3D11SpecConstantId::ClipSpaceJitterEnabled]
            ? 1
            : 0;
        // Enable, to use clipSpaceJitter, see notes below
        setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, D3D11SpecConstantId::ClipSpaceJitterEnabled, true);
      }
    }

    {
      VkViewport viewport { 0.5f, static_cast<float>(skyMatteExt.height) + 0.5f,
       static_cast<float>(skyMatteExt.width),
       -static_cast<float>(skyMatteExt.height),
       drawCallState.minZ,
       drawCallState.maxZ
      };
      VkRect2D scissor {
        { 0, 0 },
        { skyMatteExt.width, skyMatteExt.height }
      };
      setViewports(1, &viewport, &scissor);
    }

    if (hasVertexStateCB) {
      if (drawCallState.usesVertexShader) {
        D3D11RtxVertexCaptureData modified = prevCB.programmablePipeline;
        {
          // Jittered clip space for DLSS
          // Note: we can't jitter the projection matrix, as a game might calculate
          // its gl_Position by different methods (e.g. without projection matrix at all);
          // so apply jitter directly on gl_Position
          float ratioX = Sign(drawCallState.getTransformData().viewToProjection[2][3]);
          float ratioY = -Sign(drawCallState.getTransformData().viewToProjection[2][3]);
          Vector2 clipSpaceJitter = camera.calcClipSpaceJitter(camera.calcPixelJitter(m_device->getCurrentFrameId()), ratioX, ratioY);
          modified.jitterX = clipSpaceJitter.x;
          modified.jitterY = clipSpaceJitter.y;
        }

        // Ensure that memcpy can be used for fewer memory interactions
        static_assert(std::is_trivially_copyable_v<D3D11RtxVertexCaptureData>);
        allocAndMapVertexCaptureConstantBuffer() = modified;
      } else {
        D3D11FixedFunctionVS modified = prevCB.fixedFunction;
        {
          // Jittered projection for DLSS
          camera.applyJitterTo(modified.Projection,
                               m_device->getCurrentFrameId());
        }
        // Ensure that memcpy can be used for fewer memory interactions
        static_assert(std::is_trivially_copyable_v<D3D11FixedFunctionVS>);
        allocAndMapFixedFunctionVSConstantBuffer() = modified;
      }
    }

    DxvkRenderTargets skyRt;
    skyRt.color[0].view = getResourceManager().getCompatibleViewForView(skyMatteView, m_skyRtColorFormat);
    skyRt.color[0].layout = VK_IMAGE_LAYOUT_GENERAL;
    bindRenderTargets(skyRt);

    if (m_skyClearDirty) {
      DxvkContext::clearRenderTarget(skyMatteView, VK_IMAGE_ASPECT_COLOR_BIT, m_skyClearValue);
    }

    if (params.indexCount == 0) {
      DxvkContext::draw(params.vertexCount, params.instanceCount, params.vertexOffset, 0);
    } else {
      DxvkContext::drawIndexed(params.indexCount, params.instanceCount, params.firstIndex, params.vertexOffset, 0);
    }

    // Restore state
    if (prevClipSpaceJitterEnabled >= 0) {
      assert(prevClipSpaceJitterEnabled == 0 || prevClipSpaceJitterEnabled == 1);
      setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, D3D11SpecConstantId::ClipSpaceJitterEnabled, prevClipSpaceJitterEnabled);
    }
    if (hasVertexStateCB) {
      if (drawCallState.usesVertexShader) {
        allocAndMapVertexCaptureConstantBuffer() = prevCB.programmablePipeline;
      } else {
        allocAndMapFixedFunctionVSConstantBuffer() = prevCB.fixedFunction;
      }
    }
  }

  void RtxContext::initSkyProbe() {
    auto skyProbeImage = getResourceManager().getSkyProbe(this, m_skyColorFormat).image;

    if (m_skyProbeImage == skyProbeImage)
      return;

    m_skyProbeImage = skyProbeImage;

    // A newly allocated cube needs all six faces cleared on its first capture, even if no game clear
    // preceded allocation/recreation.
    if (useKenshiBlackSkyClear()) {
      m_skyClearValue = {};
      m_skyClearDirty = true;
    }

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_skyRtColorFormat;
    viewInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel = 0;
    viewInfo.numLevels = 1;
    viewInfo.minLayer = 0;
    viewInfo.numLayers = 1;

    for (uint32_t n = 0; n < 6; n++) {
      viewInfo.minLayer = n;
      m_skyProbeCubePlanes[n] = m_device->createImageView(m_skyProbeImage, viewInfo);
    }
  }

  void RtxContext::rasterizeToSkyProbe(const DrawParameters& params, const DrawCallState& drawCallState) {
    ScopedGpuProfileZone(this, "rasterizeToSkyProbe");

    // Bounded entry trace: whether the game's sky draws are classified as sky and reach this function at
    // all. Reports the draw's size, so the skydome (tens of thousands of indices) can be told from the
    // small horizon-cloud banks.
    if (terrain_profile::diagnosticsEnabled()) {
      // Periodic, not first-N: the probe must be refilled every frame, and a first-N budget cannot tell a
      // healthy stream from one that stopped.
      static uint32_t sSkyProbeEntryLogCount = 0;
      ++sSkyProbeEntryLogCount;
      if (sSkyProbeEntryLogCount <= 4u || (sSkyProbeEntryLogCount % 240u) == 0u) {
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[RTX Sky][probe-entry] sky draw reached the probe: n=", sSkyProbeEntryLogCount,
          " frame=", m_device->getCurrentFrameId(),
          " indexCount=", params.indexCount, " vertexCount=", params.vertexCount,
          " usesVertexShader=", drawCallState.usesVertexShader ? 1 : 0));
      }
    }

    // Lazy init
    initSkyProbe();

    // Grab transforms
    
    union UnifiedCB {
      D3D11RtxVertexCaptureData programmablePipeline;
      D3D11FixedFunctionVS fixedFunction;

      UnifiedCB() { }
    };

    UnifiedCB prevCB;

    // DX11_V296_NULL_STATE_CB_GUARD: same as rasterizeToSkyMatte - these
    // buffers do not exist in the DX11 fork and dereferencing them crashed any
    // sky draw that reached the probe path. Without them the per-cube-face
    // reprojection (customWorldToProjection) cannot be injected, so the six
    // faces render with the game's own transform: a degenerate but stable
    // probe instead of a crash.
    const bool hasVertexStateCB = drawCallState.usesVertexShader
      ? m_rtState.vertexCaptureCB != nullptr
      : m_rtState.vsFixedFunctionCB != nullptr;

    // Reproject by patching the sky shader's input rather than the shader: engines whose sky vertex shader
    // takes its world-view-projection as the first constant of b0 (OGRE/SkyX: `uWorldViewProj` /
    // `worldViewProjMatrix` at offset 0) can be re-rendered per cube face by binding a patched copy of b0
    // and re-issuing the game's own draw - no shader authoring, no vertex capture. The world transform is
    // baked into that matrix on the skydome, so it is recovered as
    //   world = inverse(viewToProj * worldToView) * originalWVP
    // and the per-face matrix rebuilt as cubeProj * cubeView * world.
    uint32_t patchVsCbSlot = 0u;
    const void* patchOriginalCb = nullptr;
    VkDeviceSize patchOriginalCbSize = 0u;
    DxvkBufferSlice patchPrevSlice;
    Rc<DxvkBuffer> patchScratch;
    bool patchFirstMatrix = false;

    if (!hasVertexStateCB
     && drawCallState.usesVertexShader
     && RtxOptions::skyProbePatchFirstMatrix()) {
      patchVsCbSlot = computeConstantBufferBinding(DxbcProgramType::VertexShader, 0u);
      patchPrevSlice = getShaderResourceSlot(patchVsCbSlot).bufferSlice;
      if (patchPrevSlice.defined() && patchPrevSlice.length() >= sizeof(Matrix4)) {
        // OGRE's $Params blocks are host-visible, which is what lets the D3D11
        // side read them by name; if a game's is device-local there is nothing
        // to copy and the old degenerate-probe bail-out still applies.
        patchOriginalCb = patchPrevSlice.mapPtr(0);
        patchOriginalCbSize = patchPrevSlice.length();
        patchFirstMatrix = patchOriginalCb != nullptr;
      }
    }

    if (patchFirstMatrix) {
      DxvkBufferCreateInfo info = {};
      info.size = patchOriginalCbSize;
      info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
      info.access = VK_ACCESS_UNIFORM_READ_BIT;
      patchScratch = m_device->createBuffer(info,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        DxvkMemoryStats::Category::AppBuffer, "sky probe b0 patch");
      patchFirstMatrix = patchScratch != nullptr;
    }

    if (hasVertexStateCB) {
      if (drawCallState.usesVertexShader) {
        prevCB.programmablePipeline = *static_cast<D3D11RtxVertexCaptureData*>(m_rtState.vertexCaptureCB->mapPtr(0));
      } else {
        prevCB.fixedFunction = *static_cast<D3D11FixedFunctionVS*>(m_rtState.vsFixedFunctionCB->mapPtr(0));
      }
    } else {
      // DX11_V307_NO_DEGENERATE_SKY_PROBE: do not rasterize the cube at all.
      //
      // The loop below binds each of the 6 cube faces in turn and re-draws the
      // sky geometry, relying on customWorldToProjection to aim the draw at that
      // face. That injection needs m_rtState.vertexCaptureCB, which does not
      // exist in this fork (RtxContext::setConstantBuffers has no caller), and
      // it cannot be made to exist for DXBC titles: the reprojection has to
      // happen inside the game's own compiled vertex shader, which Remix does
      // not author. D3D11SpecConstantId::CustomVertexTransformEnabled is set
      // below but appears nowhere in src/d3d11 or the shaders, so it is inert.
      //
      // Running the loop anyway wrote the SAME view-projected sky into all six
      // faces. Five of the six directions fall outside their face's clip volume
      // and keep the clear value, so the resulting cubemap is a mostly-black box
      // centred on the camera - and because SkyProbe is sampled by direction on
      // every g-buffer/indirect miss, the player sits inside that black box.
      // It also cost six extra full sky-dome draws per sky draw per frame for a
      // result that was never usable.
      //
      // Skip it and let the sky come from the physical atmosphere instead (see
      // updateAtmosphereConstants, which forces PhysicalAtmosphere once this
      // flag is set). Leave the probe cleared rather than filled with garbage.
      // DX11_V446: only give up if the b0 patch path could not arm either.
      if (!patchFirstMatrix) {
        m_skyProbeReprojectionUnavailable = true;
        ONCE(Logger::warn(
          "[RTX Sky] Sky probe per-face reprojection is unavailable on the DX11 runtime "
          "(no vertex-capture constant buffer, and this sky shader's b0 could not be patched). "
          "Skipping skybox-cubemap rasterization and switching the sky to the physical "
          "atmosphere model; rasterized skybox mode would render a black box around the camera."));
        return;
      }
      ONCE(KENSHI_DIAGNOSTIC_INFO(
        "[RTX Sky] Sky probe per-face reprojection using the b0 first-matrix patch path "
        "(DX11_V446); the game's own sky shaders are re-issued per cube face with a "
        "rebuilt world-view-projection."));
    }

    const bool useDrawCallTransforms = drawCallState.usesVertexShader || !hasVertexStateCB;
    const Matrix4& worldToView = useDrawCallTransforms ? drawCallState.getTransformData().worldToView : prevCB.fixedFunction.View;
    const Matrix4& viewToProj  = useDrawCallTransforms ? drawCallState.getTransformData().viewToProjection : prevCB.fixedFunction.Projection;

    // Figure out camera position
    const Vector3 camPos = inverse(worldToView).data[3].xyz();

    // Decompose with the main camera's matrices, not this draw's: a skydome VS exposing only
    // uWorldViewProj leaves the bridge nothing to factor, so the draw's own worldToView is identity and
    // viewToProj a fallback - which would apply the game's view twice and centre the cube at the origin.
    // Kenshi draws its sky with the world camera. Falls back to the draw's transforms if the main camera is
    // not valid yet this frame.
    Matrix4 patchWorldToView = worldToView;
    Matrix4 patchViewToProj = viewToProj;
    Vector3 patchCamPos = camPos;

    if (patchFirstMatrix) {
      const RtCamera& mainCamera =
        getSceneManager().getCameraManager().getCamera(CameraType::Main);
      if (mainCamera.isValid(m_device->getCurrentFrameId())) {
        patchWorldToView = mainCamera.getWorldToView();
        patchViewToProj = mainCamera.getViewToProjection();
        patchCamPos = mainCamera.getPosition();
      } else {
        ONCE(Logger::warn(
          "[RTX Sky] Main camera is not valid yet; falling back to the sky draw's own "
          "transforms for the per-face rebuild, which are identity for a WVP-only sky "
          "shader and will render a degenerate probe face."));
      }
    }

    const DxvkRsInfo &ri = m_state.gp.state.rs;

    DxvkRasterizerState prevRasterizerState {};
    {
      DxvkRasterizerState newRs;
      {
        newRs.depthClipEnable = ri.depthClipEnable();
        newRs.depthBiasEnable = ri.depthBiasEnable();
        newRs.polygonMode = ri.polygonMode();
        newRs.cullMode = ri.cullMode();
        newRs.frontFace = ri.frontFace();
        newRs.sampleCount = ri.sampleCount();
        newRs.conservativeMode = ri.conservativeMode();
      }
      prevRasterizerState = newRs;

      // Set cull mode to none
      newRs.cullMode = VK_CULL_MODE_NONE;
      setRasterizerState(newRs);
    }


    // Update spec constants
    int prevCustomVertexTransformEnabled = -1;
    {
      if (drawCallState.usesVertexShader) {
        prevCustomVertexTransformEnabled = getSpecConstantsInfo(VK_PIPELINE_BIND_POINT_GRAPHICS)
          .specConstants[D3D11SpecConstantId::CustomVertexTransformEnabled]
            ? 1
            : 0;
        setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, D3D11SpecConstantId::CustomVertexTransformEnabled, true);
      }
    }

    const auto& skyProbeExt = m_skyProbeImage->info().extent;

    VkViewport viewport { 0, static_cast<float>(skyProbeExt.height),
      static_cast<float>(skyProbeExt.width),
      -static_cast<float>(skyProbeExt.height),
      0.f, 1.f
    };

    VkRect2D scissor {
      { 0, 0 },
      { skyProbeExt.width, skyProbeExt.height }
    };

    setViewports(1, &viewport, &scissor);

    // Go over sky probe views and rasterize to each plane.
    // NOTE: Ideally sky probe should be rendered in single pass using
    // multiple views, however this would require multiview support
    // plumbing to dxvk side.
    // TODO: add multiview rendering in future.
    for (uint32_t plane = 0; plane < 6; plane++) {
      Rc<DxvkImageView>* skyRenderTarget = &m_skyProbeCubePlanes[plane];

      if (hasVertexStateCB) {
        if (drawCallState.usesVertexShader) {
          D3D11RtxVertexCaptureData& newState = allocAndMapVertexCaptureConstantBuffer();
          newState = prevCB.programmablePipeline;

          // Create cube plane projection
          Matrix4 proj = viewToProj;
          proj[0][0] = 1.f;
          proj[1][1] = 1.f;
          proj[2][2] = 1.f;
          proj[2][3] = 1.f;

          newState.customWorldToProjection = proj * makeViewMatrixForCubePlane(plane, camPos);
        } else {
          // Push new state to the fixed function constants
          D3D11FixedFunctionVS& newState = allocAndMapFixedFunctionVSConstantBuffer();
          newState = prevCB.fixedFunction;
          const Matrix4 view = makeViewMatrixForCubePlane(plane, camPos);

          // Set to identity, as we use custom matrices that transform from world to cube side projection
          newState.View      = view;
          newState.WorldView = view * prevCB.fixedFunction.World;
          // And cube plane projection
          newState.Projection[0][0] = 1.f;
          newState.Projection[1][1] = 1.f;
          newState.Projection[2][2] = 1.f;
          newState.Projection[2][3] = 1.f;
        }
      } else if (patchFirstMatrix) {
        // Rebuild the game's world-view-projection for this cube face and bind a patched copy of b0.
        // Everything else in the block (sun direction, Rayleigh/Mie constants, cloud and starfield params) is
        // copied through, so the face renders the game's real sky.
        Matrix4 cubeProj = patchViewToProj;
        cubeProj[0][0] = 1.f;
        cubeProj[1][1] = 1.f;
        cubeProj[2][2] = 1.f;
        cubeProj[2][3] = 1.f;

        Matrix4 originalWvp;
        std::memcpy(&originalWvp, patchOriginalCb, sizeof(Matrix4));
        const Matrix4 world = inverse(patchViewToProj * patchWorldToView) * originalWvp;
        const Matrix4 faceWvp =
          cubeProj * makeViewMatrixForCubePlane(plane, patchCamPos) * world;

        // Report the rebuilt transform once per family. A sane `world` for a camera-centred skydome is close to
        // a pure scale/translate.
        if (plane == 0u) {
          static uint32_t s_skyPatchMatrixLogCount = 0;
          if (s_skyPatchMatrixLogCount < 4u) {
            ++s_skyPatchMatrixLogCount;
            KENSHI_DIAGNOSTIC_INFO(str::format(
              "[RTX Sky][patch] face0 rebuild: camPos=[", patchCamPos.x, ",",
              patchCamPos.y, ",", patchCamPos.z, "] worldT=[",
              world.data[3].x, ",", world.data[3].y, ",", world.data[3].z,
              "] worldDiag=[", world.data[0].x, ",", world.data[1].y, ",",
              world.data[2].z, "]"));
          }
        }

        DxvkBufferSliceHandle patchSlice = patchScratch->allocSlice();
        invalidateBuffer(patchScratch, patchSlice);
        std::memcpy(patchSlice.mapPtr, patchOriginalCb, size_t(patchOriginalCbSize));
        std::memcpy(patchSlice.mapPtr, &faceWvp, sizeof(Matrix4));

        bindResourceBuffer(patchVsCbSlot,
          DxvkBufferSlice(patchScratch, 0, patchOriginalCbSize));
      }

      DxvkRenderTargets skyRt;
      skyRt.color[0].view   = *skyRenderTarget;
      skyRt.color[0].layout = VK_IMAGE_LAYOUT_GENERAL;

      bindRenderTargets(skyRt);

      if (m_skyClearDirty) {
        DxvkContext::clearRenderTarget(*skyRenderTarget, VK_IMAGE_ASPECT_COLOR_BIT, m_skyClearValue);
      }

      if (params.indexCount > 0) {
        DxvkContext::drawIndexed(params.indexCount, params.instanceCount, params.firstIndex, params.vertexOffset, 0);
      } else {
        DxvkContext::draw(params.vertexCount, params.instanceCount, params.vertexOffset, 0);
      }
    }

    // Restore state
    setRasterizerState(prevRasterizerState);
    if (prevCustomVertexTransformEnabled >= 0) {
      assert(prevCustomVertexTransformEnabled == 0 || prevCustomVertexTransformEnabled == 1);
      setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, D3D11SpecConstantId::CustomVertexTransformEnabled, prevCustomVertexTransformEnabled);
    }
    if (hasVertexStateCB) {
      if (drawCallState.usesVertexShader) {
        allocAndMapVertexCaptureConstantBuffer() = prevCB.programmablePipeline;
      } else {
        allocAndMapFixedFunctionVSConstantBuffer() = prevCB.fixedFunction;
      }
    }
    // Hand b0 back to the game's own buffer; otherwise every following draw this frame gets the last cube
    // face's matrix.
    if (patchFirstMatrix) {
      bindResourceBuffer(patchVsCbSlot, patchPrevSlice);
    }
  }

  void RtxContext::bakeTerrain(const DrawParameters& params, DrawCallState& drawCallState, const MaterialData** outOverrideMaterialData) {
    if (!getSceneManager().getTerrainBaker().enableBaking() ||
        !drawCallState.testCategoryFlags(InstanceCategories::Terrain)) {
      return;
    }

    DrawCallTransforms& transformData = drawCallState.transformData;

    // Terrain Baker (may) update bound color textures, so preserve the views
    Rc<DxvkImageView> previousColorView;
    Rc<DxvkImageView> previousSecondaryColorView;

    OpaqueMaterialData* opaqueReplacementMaterial = nullptr;
    TerrainBaker& terrainBaker = getSceneManager().getTerrainBaker();

    if (!TerrainBaker::debugDisableBaking()) {

      // Retrieve the replacement material
      MaterialData* replacementMaterial = getSceneManager().getAssetReplacer()->getReplacementMaterial(drawCallState.getMaterialData().getHash());

      if (replacementMaterial) {
        if (replacementMaterial->getType() == MaterialDataType::Opaque) {
          opaqueReplacementMaterial = &replacementMaterial->getOpaqueMaterialData();

          // Original 0th colour texture slot
          const uint32_t colorTextureSlot = drawCallState.materialData.colorTextureSlot[0];

          // Save current color texture first
          if (colorTextureSlot < m_rc.size() && m_rc[colorTextureSlot].imageView != nullptr) {
            previousColorView = m_rc[colorTextureSlot].imageView;
          }          
          
        } else {
          ONCE(Logger::warn(str::format("[RTX Texture Baker] Only opaque replacement materials are supported for terrain baking. Texture hash ",
                                        drawCallState.getMaterialData().getHash(),
                                        " has a non-opaque replacement material set. Baking the texture with legacy material instead.")));
        }
      }
    }

    // Bake the material
    const bool isBaked = terrainBaker.bakeDrawCall(this, m_state, m_rtState, params, drawCallState, opaqueReplacementMaterial, transformData.textureTransform);

    if (isBaked) {
      // Bind the baked terrain texture to the mesh
      if (!TerrainBaker::debugDisableBinding()) {

        // Set the terrain's baked material data
        *outOverrideMaterialData = terrainBaker.getMaterialData();

        // Generate texcoords in the RT shader
        transformData.texgenMode = TexGenMode::CascadedViewPositions;

        // Update the legacy material data with legacy value defaults as well as set the color textur since some of its data 
        // is still used through the Rt pipeline even though overrideMaterialData is specifide. 
        // Also SceneManager uses sampler associated with the color texture to patch samplers for the textures in the opaque material.
        LegacyMaterialData overrideMaterial;
        overrideMaterial.colorTextures[0] = (*outOverrideMaterialData)->getOpaqueMaterialData().getAlbedoOpacityTexture();
        overrideMaterial.samplers[0] = terrainBaker.getTerrainSampler();
        overrideMaterial.updateCachedHash();
        drawCallState.materialData = overrideMaterial;
      }

      // Restore state modified during baking
      if (!TerrainBaker::debugDisableBaking()) {

        // Restore bound color texture views
        if (previousColorView != nullptr) {
          bindResourceView(drawCallState.materialData.colorTextureSlot[0], previousColorView, nullptr);
        }
      }
    }
  }

  void RtxContext::rasterizeSky(const DrawParameters& params, const DrawCallState& drawCallState) {
    // Reaching here is distinct from reaching the probe, since this early-out sits between them.
    {
      static uint32_t s_rasterizeSkyLogCount = 0;
      if (s_rasterizeSkyLogCount < 16u) {
        ++s_rasterizeSkyLogCount;
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[RTX Sky][gate] rasterizeSky entered: skyMode=", uint32_t(RtxOptions::skyMode()),
          " willEarlyOut=",
          RtxOptions::skyMode() == SkyMode::PhysicalAtmosphere ? 1 : 0,
          " indexCount=", params.indexCount));
      }
    }

    // Skip rasterized sky rendering when physical atmosphere mode is active.
    if (RtxOptions::skyMode() == SkyMode::PhysicalAtmosphere) {
      return;
    }

    // Grab and apply replacement texture if any
    // NOTE: only the original color texture will be replaced with albedo-opacity texture
    MaterialData* replacementMaterial = getSceneManager().getAssetReplacer()->getReplacementMaterial(drawCallState.getMaterialData().getHash());
    bool replacemenIsLDR = false;
    Rc<DxvkImageView> replacementTexture = {};
    uint32_t replacementTextureSlot = UINT32_MAX;

    if (replacementMaterial && replacementMaterial->getType() == MaterialDataType::Opaque) {
      // Must pull a ref because we will modify it for loading purposes below.
      TextureRef& albedoOpacity = replacementMaterial->getOpaqueMaterialData().getAlbedoOpacityTexture();

      if (albedoOpacity.isValid()) {
        uint32_t textureIndex;
        getSceneManager().trackTexture(albedoOpacity, textureIndex, true, false);

        if (!albedoOpacity.isImageEmpty()) {
          replacementTextureSlot = drawCallState.materialData.colorTextureSlot[0];
          replacementTexture = albedoOpacity.getImageView();
          replacemenIsLDR = TextureUtils::isLDR(albedoOpacity.getImageView()->info().format);
        } else {
          ONCE(Logger::warn("A replacement texture for sky was specified, but it could not be loaded."));
        }
      }
    }
    
    Rc<DxvkImageView> curColorView = {};
    if (replacementTextureSlot < m_rc.size())
    {
      if (m_rc[replacementTextureSlot].imageView != nullptr && replacementTexture != nullptr) {
        // Save currently bound texture to restore later
        curColorView = m_rc[replacementTextureSlot].imageView;
        // Bind a replacement texture
        bindResourceView(replacementTextureSlot, replacementTexture, nullptr);
      }
    }

    // Save current RTs
    DxvkRenderTargets curRts = m_state.om.renderTargets;

    if (!TextureUtils::isLDR(m_skyRtColorFormat) && (!replacementMaterial || replacemenIsLDR)) {
      ONCE(Logger::warn("Sky may not appear correct: sky intermediate format has been forced to HDR "
                        "while the original sky is LDR and no HDR sky replacement has been found!"));
    }

    // Save viewports
    const uint32_t curViewportCount = m_state.gp.state.rs.viewportCount();
    const DxvkViewportState curVp = m_state.vp;

    // Kenshi's G-buffer clears to (0, 0.5, 0), an encoded deferred background, not sky radiance, and the
    // generic clear interception would capture that colour. Use black for our sky targets only, so
    // uncovered dome regions cannot light the scene green.
    if (useKenshiBlackSkyClear()) {
      m_skyClearValue = {};
      ONCE(KENSHI_DIAGNOSTIC_INFO("[RTX Sky V789] Capture background is black for sky matte and all six probe faces; game clears unchanged."));
    }

    rasterizeToSkyMatte(params, drawCallState);
    rasterizeToSkyProbe(params, drawCallState);

    m_skyClearDirty = false;

    // Restore VPs
    setViewports(curViewportCount, curVp.viewports.data(), curVp.scissorRects.data());

    // Restore RTs
    bindRenderTargets(curRts);

    // Restore color texture
    if (curColorView != nullptr) {
      bindResourceView(drawCallState.materialData.colorTextureSlot[0], curColorView, nullptr);
    }
  }

  void RtxContext::clearRenderTarget(const Rc<DxvkImageView>& imageView,
                                     VkImageAspectFlags clearAspects, VkClearValue clearValue) {
    // Capture color for skybox clear
    if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
      m_skyClearValue = clearValue;

      // Set dirty flag so that next skyprobe rasterize will clear the views.
      // We assume that skybox drawcalls will immediately follow the clear. The logic would
      // need to be revisited if this is not true for some game.
      m_skyClearDirty = true;
    }

    DxvkContext::clearRenderTarget(imageView, clearAspects, clearValue);
  }

  void RtxContext::clearImageView(const Rc<DxvkImageView>& imageView, VkOffset3D offset,
                                  VkExtent3D extent, VkImageAspectFlags aspect, VkClearValue value) {
    // Capture color for skybox clear
    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
      m_skyClearValue = value;

      // Set dirty flag so that next skyprobe rasterize will clear the views.
      // We assume that skybox drawcalls will immediately follow the clear. The logic would
      // need to be revisited if this is not true for some game.
      m_skyClearDirty = true;
    }

    DxvkContext::clearImageView(imageView, offset, extent, aspect, value);
  }

  void RtxContext::reportCpuSimdSupport() {
    switch (fast::getSimdSupportLevel()) {
    case fast::AVX512:
      dxvk::Logger::info("CPU supports SIMD: AVX512");
      break;
    case fast::AVX2:
      dxvk::Logger::info("CPU supports SIMD: AVX2");
      break;
    case fast::SSE4_1:
      dxvk::Logger::info("CPU supports SIMD: SSE 4.1");
      break;
    case fast::SSE3:
      dxvk::Logger::info("CPU supports SIMD: SSE 3");
      break;
    case fast::SSE2:
      dxvk::Logger::info("CPU supports SIMD: SSE 2");
      break;
    case fast::None:
      dxvk::Logger::info("CPU doesn't support SIMD");
      break;
    default:
      Logger::err("Invalid SIMD state");
      break;
    }
  }

  const DxvkScInfo& RtxContext::getSpecConstantsInfo(VkPipelineBindPoint pipeline) const {
    return
      pipeline == VK_PIPELINE_BIND_POINT_GRAPHICS
      ? m_state.gp.state.sc
      : pipeline == VK_PIPELINE_BIND_POINT_COMPUTE
      ? m_state.cp.state.sc
      : m_state.rp.state.sc;
  }

  void RtxContext::setSpecConstantsInfo(
    VkPipelineBindPoint pipeline,
    const DxvkScInfo& newSpecConstantInfo) {
    DxvkScInfo& specConstantInfo =
      pipeline == VK_PIPELINE_BIND_POINT_GRAPHICS
      ? m_state.gp.state.sc
      : pipeline == VK_PIPELINE_BIND_POINT_COMPUTE
      ? m_state.cp.state.sc
      : m_state.rp.state.sc;

    if (specConstantInfo != newSpecConstantInfo) {
      specConstantInfo = newSpecConstantInfo;

      m_flags.set(
        pipeline == VK_PIPELINE_BIND_POINT_GRAPHICS
        ? DxvkContextFlag::GpDirtyPipelineState
        : pipeline == VK_PIPELINE_BIND_POINT_COMPUTE
          ? DxvkContextFlag::CpDirtyPipelineState
          : DxvkContextFlag::RpDirtyPipelineState);
    }
  }

#ifdef REMIX_DEVELOPMENT
  void RtxContext::cacheResourceAliasingImageView(const Rc<DxvkImageView>& imageView) {
    if (imageView.ptr()) {
      // Determine the format compatibility category for the image view
      const auto formatCategory = Resources::getFormatCompatibilityCategory(imageView->info().format);
      const auto categoryIndex = static_cast<uint32_t>(formatCategory);
      const auto& underlyingImage = imageView->image();

      // Proceed only if the category is valid and the image view is tracked in the resource view map
      if (formatCategory != RtxTextureFormatCompatibilityCategory::InvalidFormatCompatibilityCategory &&
          Resources::s_resourcesViewMap.find(imageView.ptr()) != Resources::s_resourcesViewMap.end()) {
        bool aliasingMatchFound = false;
        // Search the cache for an existing aliased resource with the same underlying image
        for (auto& compatibleResource : m_resourceCacheTable[categoryIndex]) {
          if (compatibleResource.view->image() == underlyingImage) {
            // Match found: update the begin and end pass stages to expand their range
            compatibleResource.beginPassStage = std::min(compatibleResource.beginPassStage, m_currentPassStage);
            compatibleResource.endPassStage = std::max(compatibleResource.endPassStage, m_currentPassStage);

            // Add the current resource name to the set of names for this aliased group
            compatibleResource.names.insert(Resources::s_resourcesViewMap[imageView.ptr()]);
            aliasingMatchFound = true;
            break;
          }
        }

        if (!aliasingMatchFound) {
          // No match found: cache this as a new aliased resource entry
          m_resourceCacheTable[categoryIndex].push_back({ imageView, m_currentPassStage, m_currentPassStage, { Resources::s_resourcesViewMap[imageView.ptr()] } });
        }
      }
    }
  }

  void RtxContext::queryAvailableResourceAliasing() {
    // Check if aliasing query is enabled through user settings
    if (!Resources::s_queryAliasing) {
      return;
    }

    // Set the aliasing resource dimensions based on user options
    const VkExtent3D extent = { RtxOptions::Aliasing::width(), RtxOptions::Aliasing::height(), RtxOptions::Aliasing::depth() };
    // Get the start and end frame pass stages from user settings
    const RtxFramePassStage beginPass = RtxOptions::Aliasing::beginPass();
    const RtxFramePassStage endPass = RtxOptions::Aliasing::endPass();

    std::string newResourceAliasingQueryResult;
    // Check if the begin pass is before the end pass
    if (beginPass > endPass) {
      // Set an error message if the begin pass is invalid
      Resources::s_resourceAliasingQueryText = "Begin Pass must be before the End Pass";
      return;
    }

    // Lambda function to check if a resource matches the aliasing criteria
    auto isResourceMatches = [&](const Rc<DxvkImageView>& view) {
      const auto& imageInfo = view->image()->info();
      const auto& viewInfo = view->info();
      uint32_t aliasingWidth = RtxOptions::Aliasing::width();
      uint32_t aliasingHeight = RtxOptions::Aliasing::height();

      // Adjust dimensions if the aliasing extent type is DownScaledExtent or TargetExtent
      if (RtxOptions::Aliasing::extentType() == RtxTextureExtentType::DownScaledExtent) {
        aliasingWidth = getResourceManager().getDownscaleDimensions().width;
        aliasingHeight = getResourceManager().getDownscaleDimensions().height;
      } else if (RtxOptions::Aliasing::extentType() == RtxTextureExtentType::TargetExtent) {
        aliasingWidth = getResourceManager().getTargetDimensions().width;
        aliasingHeight = getResourceManager().getTargetDimensions().height;
      }

      // Check if the resource's dimensions and format match the aliasing query settings
      return imageInfo.extent.width == aliasingWidth &&
              imageInfo.extent.height == aliasingHeight &&
              imageInfo.extent.depth == RtxOptions::Aliasing::depth() &&
              imageInfo.numLayers == RtxOptions::Aliasing::layer() &&
              imageInfo.type == RtxOptions::Aliasing::imageType() &&
              viewInfo.type == RtxOptions::Aliasing::imageViewType();
    };

    std::string manualSolveResources;
    uint32_t matchedIndex = 0;

    bool aliasingMatchFound = false;
    const auto category = RtxOptions::Aliasing::formatCategory();
    if (category == RtxTextureFormatCompatibilityCategory::InvalidFormatCompatibilityCategory) {
      // If the format category is invalid, no aliasing can be done
      Resources::s_resourceAliasingQueryText = "Please select aliasing compatible texture format.";
      return;
    }

    // Map category to index for cache lookup
    const uint32_t index = static_cast<uint32_t>(category);

    // Loop through the resource cache table for the corresponding format category
    for (auto& compatibleResource : m_resourceCacheTable[index]) {
      // Check if the resource is compatible with the aliasing query (based on pass stages and matching criteria)
      if ((endPass < compatibleResource.beginPassStage || beginPass > compatibleResource.endPassStage ||
            (beginPass != endPass && compatibleResource.beginPassStage != compatibleResource.endPassStage &&
            (endPass == compatibleResource.beginPassStage || beginPass == compatibleResource.endPassStage))) &&
          (isResourceMatches(compatibleResource.view))) {

        // Loop through names of matching resources and prepare result string
        for (const auto& name : compatibleResource.names) {
          if (Resources::s_dynamicAliasingResourcesSet.find(compatibleResource.view.ptr()) == Resources::s_dynamicAliasingResourcesSet.end()) {
            ++matchedIndex;
            newResourceAliasingQueryResult += std::to_string(matchedIndex) + ". " + name + "\n";
            if (matchedIndex > 10) {
              break; // Limit to 10 results
            }
          } else {
            if (manualSolveResources.empty()) {
              // Give notification for users who want to do aliasing for dynamic resources
              manualSolveResources = "[WARNING] Use caution when aliasing dynamic resources. Ensure aliasing is handled every frame in Resources::onFrameBegin.\n";
            }
            manualSolveResources += name + "\n";
          }
        }
        aliasingMatchFound = true;
      }
    }

    // Set the result of the aliasing query, either showing available resources or a no-match message
    Resources::s_resourceAliasingQueryText =
      aliasingMatchFound ? (newResourceAliasingQueryResult + manualSolveResources) : "No available resources that can be aliased, please create a new resource.";
  }

  void RtxContext::clearResourceAliasingCache() {
    // Clean up caches
    for (auto& resourceCaches : m_resourceCacheTable) {
      resourceCaches.clear();
    }
    m_currentPassStage = RtxFramePassStage::FrameEnd;
  }

  void RtxContext::analyzeResourceAliasing() {
    // Early exit if the aliasing analyzer option is not enabled
    if (!Resources::s_startAliasingAnalyzer) {
      return;
    }

    // Lambda to check if two image views are compatible for aliasing
    auto isResourceCompatible = [](const Rc<DxvkImageView>& view, const Rc<DxvkImageView>& matchedView) {
      const auto& imageInfo = view->image()->info();
      const auto& matchedImageInfo = matchedView->image()->info();
      const auto& viewInfo = view->info();
      return imageInfo.extent == matchedImageInfo.extent &&
             imageInfo.numLayers == matchedImageInfo.numLayers &&
             imageInfo.type == matchedImageInfo.type;
    };

    std::string availableAliasingText;

    // Iterate over all format compatibility categories
    for (uint32_t index = 0; index < static_cast<uint32_t>(RtxTextureFormatCompatibilityCategory::Count); ++index) {
      const std::vector<ResourceCache>& cacheList = m_resourceCacheTable[index];

      // Compare each pair of resources within the same format category
      for (size_t i = 0; i < cacheList.size(); ++i) {
        for (size_t j = i + 1; j < cacheList.size(); ++j) {
          // Check for non-overlapping lifetimes (safe for aliasing)
          if ((cacheList[i].endPassStage < cacheList[j].beginPassStage || cacheList[i].beginPassStage > cacheList[j].endPassStage ||
               (cacheList[i].beginPassStage != cacheList[i].endPassStage && cacheList[j].beginPassStage != cacheList[j].endPassStage &&
                (cacheList[i].endPassStage == cacheList[j].beginPassStage || cacheList[i].beginPassStage == cacheList[j].endPassStage))) &&
              isResourceCompatible(cacheList[i].view, cacheList[j].view)) {
            // Add the resource names to the output text
            availableAliasingText += *cacheList[i].names.begin() + " <-> " + *cacheList[j].names.begin() + "\n";
          }
        }
      }
    }

    // Output the results to the GUI field
    Resources::s_aliasingAnalyzerResultText = !availableAliasingText.empty() ? availableAliasingText : "Can't find any resources that can be aliased.\n";
  }
#endif
} // namespace dxvk
