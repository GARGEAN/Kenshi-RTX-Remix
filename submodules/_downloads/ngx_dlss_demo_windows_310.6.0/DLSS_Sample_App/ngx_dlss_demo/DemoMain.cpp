//----------------------------------------------------------------------------------
// File:        DemoMain.cpp
// SDK Version: 1.1
// Email:       DLSS_support@nvidia.com
// Site:        http://developer.nvidia.com/
//
// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------------

#include <string>
#include <vector>
#include <memory>
#include <sstream>
#include <iomanip>
#include <random>

#if !__linux__
#include <nvapi.h>
#endif

#if __linux__
#define ExitProcess exit
#endif

#include <donut/core/vfs/VFS.h>
#include <donut/core/log.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/render/BloomPass.h>
#include <donut/render/CascadedShadowMap.h>
#include <donut/render/DeferredLightingPass.h>
#include <donut/render/DepthPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/EnvironmentMapPass.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/render/GBufferFillPass.h>
#include <donut/render/LightProbeProcessingPass.h>
#include <donut/render/PixelReadbackPass.h>
#include <donut/render/SkyPass.h>
#include <donut/render/SsaoPass.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/render/ToneMappingPasses.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/app/DeviceManager.h>
#include <nvrhi/utils.h>
#ifdef DONUT_WITH_HBAO_PLUS
#include <donut/render/plugins/HbaoPlusPass.h>
#endif

using namespace donut;
using namespace donut::math;
using namespace donut::app;
using namespace donut::vfs;
using namespace donut::engine;
using namespace donut::render;

#ifndef DONUT_WITH_NGX
#error This sample is to demonstrate NGX DLSS.  Please configure donut via Cmake to use NGX by setting DONUT_WITH_NGX option to ON
#endif

#include <NGXWrapper.h>

#if USE_VK
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvrhi/vulkan.h>
#include <nvsdk_ngx_helpers_vk.h>
#endif


// All render target resources are created in here.  Check here for formats.
#include "glue/RenderTargets.hpp"

constexpr float OPTIMAL_RATIO  = -1.0f; // Use the scaling ratio that the NGX Snippet recommends
constexpr float VARIABLE_RATIO = -2.0f; // Use a random variable scaling ratio (between MIN and MAX that the snippet recommends)

constexpr size_t NUM_OFFSET_SEQUENCES = 64; // Use a large number of Halton sequence offsets to accomodate large scaling ratios.

struct PERF_QUALITY_ITEM
{
    NVSDK_NGX_PerfQuality_Value PerfQuality;
    const char *                PerfQualityText;
    bool                        PerfQualityAllowed;
    bool                        PerfQualityDynamicAllowed;
};

struct UIData
{
    enum class                            AntiAliasingMode {NONE,TEMPORAL,DLSS,};
    static constexpr float                ScaleRatios[] = {OPTIMAL_RATIO, VARIABLE_RATIO, 1.f/3.f, 1.f/2.f, 1.75f/3.f, 2.f/3.f, 1.f};
    bool                                  ShowUI = true;
    bool                                  EnableSsao = true;
    struct SsaoParameters                 SsaoParameters;
    struct ToneMappingParameters          ToneMappingParams;
    struct TemporalAntiAliasingParameters TemporalAntiAliasingParams;
    enum AntiAliasingMode                 AntiAliasingMode = AntiAliasingMode::DLSS;
    float                                 ScaleRatio = ScaleRatios[0];
    bool                                  PreTonemapping = true;
    bool                                  OverrideLodBias = false;
    float                                 DefaultLodBias = 0.0f;
    float                                 ForcedLodBias = 0.0f;
    bool                                  ForceReset     = false;
    bool                                  EnableVsync = false;
    bool                                  ShaderReloadRequested = false;
    bool                                  EnableProceduralSky = true;
    bool                                  EnableBloom = true;
    float                                 BloomSigma = 32.f;
    bool                                  EnableTranslucency = true;
    bool                                  EnableMaterialEvents = false;
    bool                                  EnableShadows = true;
    float                                 AmbientIntensity = 0.2f;
    float                                 LightProbeDiffuseScale = 1.f;
    float                                 LightProbeSpecularScale = 1.f;
    float                                 CsmExponent = 4.f;
    bool                                  EnableAutoExposure = false;    
    NVSDK_NGX_DLSS_Hint_Render_Preset     RenderPresetSelected = NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    bool                                  RenderPresetSelectionHasChanged = false;
    std::map<NVSDK_NGX_DLSS_Hint_Render_Preset , const char*> RENDER_PRESET_MAP =
        {
            {NVSDK_NGX_DLSS_Hint_Render_Preset_Default, "Default"},
            {NVSDK_NGX_DLSS_Hint_Render_Preset_E,       "Render Preset E"},
            {NVSDK_NGX_DLSS_Hint_Render_Preset_F,       "Render Preset F"},
            {NVSDK_NGX_DLSS_Hint_Render_Preset_J,       "Render Preset J"},
            {NVSDK_NGX_DLSS_Hint_Render_Preset_K,       "Render Preset K"},
            {NVSDK_NGX_DLSS_Hint_Render_Preset_L,       "Render Preset L"},
            {NVSDK_NGX_DLSS_Hint_Render_Preset_M,       "Render Preset M"}
    };

    int                                   PerfModeListIdx = 0;
    std::vector<PERF_QUALITY_ITEM>        PERF_QUALITY_LIST =
        {   
            {NVSDK_NGX_PerfQuality_Value_MaxPerf,          "Performance", false, false},
            {NVSDK_NGX_PerfQuality_Value_Balanced,         "Balanced"   , false, false},
            {NVSDK_NGX_PerfQuality_Value_MaxQuality,       "Quality"    , false, false},
            {NVSDK_NGX_PerfQuality_Value_UltraPerformance, "UltraPerf"  , false, false},
            {NVSDK_NGX_PerfQuality_Value_DLAA,             "DLAA"       , false, false},
        };

    std::string                           ScreenshotFileName = "";
};

class FeatureDemo : public ApplicationBase
{
private:
    typedef ApplicationBase Super;

    std::unique_ptr<MediaFolder>        m_MediaFolder;
    std::string                         m_CurrentSceneName;
    std::shared_ptr<Scene>              m_Scene;
    std::shared_ptr<ShaderFactory>      m_ShaderFactory;
    std::shared_ptr<DirectionalLight>   m_SunLight;
    std::shared_ptr<CascadedShadowMap>  m_ShadowMap;
    std::shared_ptr<FramebufferFactory> m_ShadowFramebuffer;
    std::shared_ptr<DepthPass>          m_ShadowDepthPass;
    std::shared_ptr<InstancedOpaqueDrawStrategy> m_OpaqueDrawStrategy;
    std::shared_ptr<TransparentDrawStrategy> m_TransparentDrawStrategy;
    std::unique_ptr<RenderTargets>      m_RenderTargets;
    std::shared_ptr<ForwardShadingPass> m_ForwardPass;
    std::unique_ptr<GBufferFillPass>    m_GBufferPass;
    std::unique_ptr<DeferredLightingPass> m_DeferredLightingPass;
    std::unique_ptr<SkyPass>            m_SkyPass;
    std::unique_ptr<EnvironmentMapPass> m_EnvironmentMapPass;
    std::unique_ptr<TemporalAntiAliasingPass> m_TemporalAntiAliasingPass;
    std::unique_ptr<BloomPass>          m_BloomPass;
    std::unique_ptr<ToneMappingPass>    m_ToneMappingPass;
    std::unique_ptr<SsaoPass>           m_SsaoPass;
    std::shared_ptr<LightProbeProcessingPass> m_LightProbePass;
#ifdef DONUT_WITH_HBAO_PLUS
    std::unique_ptr<HbaoPlusPass>       m_HbaoPlusPass;
#endif

    std::unique_ptr<NGXWrapper>         m_NGXWrapper;
    std::shared_ptr<IView>              m_View;
    std::shared_ptr<IView>              m_ViewPrevious;
    std::shared_ptr<IView>              m_TonemappingView;

    std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_GBufferBindingSets;

    nvrhi::CommandListHandle            m_CommandList;
    bool                                m_PreviousViewsValid = false;
    FPSCamera                           m_Camera;
    dm::affine3                         m_CameraPreviousMatrix;
    degrees_f                           m_CameraVerticalFov = 60.f;
    float3                              m_AmbientTop = 0.f;
    float3                              m_AmbientBottom = 0.f;
    bool                                m_SharpeningOn = false;
    bool                                m_AutoExposureOn = false;
    int                                 m_FrameIndex = 0;
    float                               m_PreviousLodBias = 0.f;
    dm::int2                            m_PreviousCreationTimeSize = {~0, ~0, };

    std::shared_ptr<LoadedTexture>      m_EnvironmentMap;

    UIData&                             m_ui;

    std::map<NVSDK_NGX_PerfQuality_Value, DlssRecommendedSettings> m_RecommendedSettingsMap;
    dm::int2                            m_recommendedSettingsLastSize = {~0, ~0, }; // in case we need to refresh the recommended settings map

    bool m_bUseNgxSdkExtApi;
    std::default_random_engine          m_Generator;

public:

    FeatureDemo(DeviceManager* deviceManager, UIData& ui, bool useNgxSdkExtApi)
        : Super(deviceManager)
        , m_ui(ui)
        , m_bUseNgxSdkExtApi(useNgxSdkExtApi)
    {
        std::shared_ptr<NativeFileSystem> nativeFS = std::make_shared<NativeFileSystem>();

        std::filesystem::path mediaPath = FindMediaFolder("media/sponza.json");
        std::filesystem::path frameworkShaderPath = FindDirectoryWithShaderBin(GetDevice()->getGraphicsAPI(), *nativeFS, GetDirectoryWithExecutable(), "donut/shaders", "blit_ps");

        std::shared_ptr<RootFileSystem> rootFS = std::make_shared<RootFileSystem>();
        rootFS->mount("/media", mediaPath);
        rootFS->mount("/shaders/framework", frameworkShaderPath);

        m_MediaFolder = std::make_unique<MediaFolder>(rootFS, "/media");
        if (m_MediaFolder->GetAvailableScenes().empty())
            ExitProcess(1);

        m_TextureCache = std::make_shared<TextureCache>(GetDevice(), rootFS);

        m_ShaderFactory = std::make_shared<ShaderFactory>(GetDevice(), rootFS, "/shaders/framework", "");
        m_CommonPasses = std::make_shared<CommonRenderPasses>(GetDevice(), m_ShaderFactory);

        m_ShadowMap = std::make_shared<CascadedShadowMap>(GetDevice(), 2048, 4, 0, nvrhi::Format::D24S8);

        m_ShadowFramebuffer = std::make_shared<FramebufferFactory>(GetDevice());
        m_ShadowFramebuffer->DepthTarget = m_ShadowMap->GetTexture();

        DepthPass::CreateParameters shadowDepthParams;
        shadowDepthParams.rasterState.slopeScaledDepthBias = 4.f;
        shadowDepthParams.rasterState.depthBias = 100;
        m_ShadowDepthPass = std::make_shared<DepthPass>(GetDevice(), m_CommonPasses);
        m_ShadowDepthPass->Init(*m_ShaderFactory, *m_ShadowFramebuffer, m_ShadowMap->GetView(), shadowDepthParams);

#ifdef DONUT_WITH_HBAO_PLUS
        m_HbaoPlusPass = std::make_unique<HbaoPlusPass>(GetDevice());
#endif

        // Create and initialize NGX library
        m_NGXWrapper.reset();
        m_NGXWrapper = std::make_unique<NGXWrapper>(GetDevice());


        m_CommandList = GetDevice()->createCommandList();

        m_Camera.SetMoveSpeed(3.0f);

        SetAsynchronousLoadingEnabled(true);
        SetCurrentSceneName("sponza.json");

        // Load the environment map
        m_EnvironmentMap = m_TextureCache->LoadTextureFromFileDeferred(m_MediaFolder->GetPath() / "EnvironmentMap.dds", true);

    }

    std::string GetCurrentSceneName() const
    {
        return m_CurrentSceneName;
    }

    void SetCurrentSceneName(const std::string& sceneName)
    {
        if (m_CurrentSceneName == sceneName)
            return;

        m_CurrentSceneName = sceneName;

        BeginLoadingScene(m_MediaFolder->GetFileSystem(), m_MediaFolder->GetPath() / m_CurrentSceneName);
    }

    MediaFolder& GetMediaFolder() const
    {
        return *m_MediaFolder;
    }

    virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) override
    {
        if (key == GLFW_KEY_GRAVE_ACCENT && action == GLFW_PRESS)
        {
            m_ui.ShowUI = !m_ui.ShowUI;
            return true;
        }

        m_Camera.KeyboardUpdate(key, scancode, action, mods);
        return true;
    }

    virtual bool MousePosUpdate(double xpos, double ypos) override
    {
        m_Camera.MousePosUpdate(xpos, ypos);
        return true;
    }

    virtual bool MouseButtonUpdate(int button, int action, int mods) override
    {
        m_Camera.MouseButtonUpdate(button, action, mods);
        return true;
    }

    virtual void Animate(float fElapsedTimeSeconds) override
    {
        m_Camera.Animate(fElapsedTimeSeconds);
        if(m_ToneMappingPass)
            m_ToneMappingPass->AdvanceFrame(fElapsedTimeSeconds);
    }


    virtual void SceneUnloading() override
    {
        if (m_ForwardPass) m_ForwardPass->ResetBindingCache();
        if (m_DeferredLightingPass) m_DeferredLightingPass->ResetBindingCache();
        if (m_GBufferPass) m_GBufferPass->ResetBindingCache();
        if (m_LightProbePass) m_LightProbePass->ResetCaches();
        if (m_ShadowDepthPass) m_ShadowDepthPass->ResetBindingCache();
        m_SunLight.reset();
    }

    virtual bool LoadScene(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& fileName) override
    {
        Scene* scene = new Scene(fs);

        if (scene->Load(fileName, VertexAttribute::ALL, *m_TextureCache))
        {
            m_Scene = std::unique_ptr<Scene>(scene);
            return true;
        }

        return false;
    }

    virtual void SceneLoaded() override
    {
        Super::SceneLoaded();

        m_Scene->CreateRenderingResources(GetDevice());

        m_OpaqueDrawStrategy = std::make_shared<InstancedOpaqueDrawStrategy>(*m_Scene);
        m_TransparentDrawStrategy = std::make_shared<TransparentDrawStrategy>(*m_Scene);
        m_PreviousViewsValid = false;

        for (auto light : m_Scene->Lights)
        {
            if (light->GetLightType() == LightType::DIRECTIONAL)
            {
                m_SunLight = std::static_pointer_cast<DirectionalLight>(light);
                break;
            }
        }

        if (!m_SunLight)
        {
            m_SunLight = std::make_shared<DirectionalLight>();
            m_SunLight->direction = normalize(float3(-0.05, -1, 0.1));
            m_SunLight->angularSize = 0.53f;
            m_SunLight->irradiance = 1.f;
            m_SunLight->name = "Sun";
            m_Scene->Lights.push_back(m_SunLight);
        }

        if (m_Scene->DefaultCamera)
        {
            m_Camera.LookAt(m_Scene->DefaultCamera->position, m_Scene->DefaultCamera->lookAt, m_Scene->DefaultCamera->up);
            m_CameraVerticalFov = m_Scene->DefaultCamera->verticalFov;
        }
        else
        {
            m_Camera.LookAt(float3(0.f, 1.8f, 0.f),float3(1.f, 1.8f, 0.f));
            m_CameraVerticalFov = 60.f;
        }
    }

    std::shared_ptr<TextureCache> GetTextureCache()
    {
        return m_TextureCache;
    }

    std::shared_ptr<Scene> GetScene()
    {
        return m_Scene;
    }

    bool SetupView(donut::math::int2 renderSize, donut::math::int2 renderOffset, bool preTonemapping)
    {
        const float zNear = 0.1f;
        const float zFar = 200.f;
        bool topologyChanged = false;

        float2 displaySize = float2(m_RenderTargets->m_DisplaySize);

        // Always use the display aspect ratio
        // because we may render to a different rendering resolution than the display resolution
        // we still want geometry to appear similar once it is scaled up to the whole window/screen
        float aspectRatio = displaySize.x / displaySize.y;

        donut::math::float2 renderTargetSize = m_RenderTargets->m_MaximumRenderSize;
        float2 pixelOffset = (m_ui.AntiAliasingMode == UIData::AntiAliasingMode::TEMPORAL ||
                              m_ui.AntiAliasingMode == UIData::AntiAliasingMode::DLSS) &&
                              !m_ui.ForceReset
            ? GetCurrentPixelOffset()
            : float2(0.f);

        nvrhi::Viewport renderViewport = nvrhi::Viewport(float(renderOffset.x), float(renderOffset.x + renderSize.x), float(renderOffset.y), float(renderOffset.y + renderSize.y), 0.0f, 1.0f);;

        {
            std::shared_ptr<PlanarView> renderPlanarView = std::dynamic_pointer_cast<PlanarView, IView>(m_View);

            if (!renderPlanarView)
            {
                m_View = renderPlanarView = std::make_shared<PlanarView>();
                m_ViewPrevious = std::make_shared<PlanarView>();
                topologyChanged = true;
            }

            std::shared_ptr<PlanarView> renderPlanarViewPrevious = std::dynamic_pointer_cast<PlanarView, IView>(m_ViewPrevious);

            float4x4 projection = perspProjD3DStyle(dm::radians(m_CameraVerticalFov), aspectRatio, zNear, zFar);

            renderPlanarView->SetViewport(renderViewport);
            renderPlanarView->SetPixelOffset(pixelOffset);

            // Also correct the ViewPrevious so that the motion vector rendering does not
            // account for the change in viewport and pixel offsets !
            renderPlanarViewPrevious->SetViewport(renderViewport);
            renderPlanarViewPrevious->SetPixelOffset(pixelOffset);
            renderPlanarViewPrevious->SetMatrices(m_CameraPreviousMatrix, projection);

            renderPlanarView->SetMatrices(m_Camera.GetWorldToViewMatrix(), projection);
        }
        {
            std::shared_ptr<PlanarView> tonemappingPlanarView = std::dynamic_pointer_cast<PlanarView, IView>(m_TonemappingView);

            if (!tonemappingPlanarView)
            {
                m_TonemappingView = tonemappingPlanarView = std::make_shared<PlanarView>();
                topologyChanged = true;
            }

            float4x4 projection = perspProjD3DStyle(dm::radians(m_CameraVerticalFov), aspectRatio, zNear, zFar);
            float2 vpSize = preTonemapping ? displaySize : renderTargetSize;

            tonemappingPlanarView->SetViewport(nvrhi::Viewport(vpSize.x, vpSize.y));
            tonemappingPlanarView->SetMatrices(m_Camera.GetWorldToViewMatrix(), projection);
        }

        return topologyChanged;
    }

    void CreateRenderPasses(donut::math::int2 creationTimeRenderSize, float lodBias, bool& exposureResetRequired)
    {
        uint32_t motionVectorStencilMask = 0x01;

        nvrhi::SamplerDesc samplerdescPoint      = m_CommonPasses->m_PointClampSampler->GetDesc();
        nvrhi::SamplerDesc samplerdescLinear     = m_CommonPasses->m_LinearClampSampler->GetDesc();
        nvrhi::SamplerDesc samplerdescLinearWrap = m_CommonPasses->m_LinearWrapSampler->GetDesc();
        nvrhi::SamplerDesc samplerdescAniso      = m_CommonPasses->m_AnisotropicWrapSampler->GetDesc();

        samplerdescPoint.mipBias      = lodBias;
        samplerdescLinear.mipBias     = lodBias;
        samplerdescLinearWrap.mipBias = lodBias;
        samplerdescAniso.mipBias      = lodBias;

        m_CommonPasses->m_PointClampSampler      = GetDevice()->createSampler(samplerdescPoint);
        m_CommonPasses->m_LinearClampSampler     = GetDevice()->createSampler(samplerdescLinear);
        m_CommonPasses->m_LinearWrapSampler      = GetDevice()->createSampler(samplerdescLinearWrap);
        m_CommonPasses->m_AnisotropicWrapSampler = GetDevice()->createSampler(samplerdescAniso);

        m_ForwardPass = std::make_unique<ForwardShadingPass>(GetDevice(), m_CommonPasses);
        m_ForwardPass->Init(*m_ShaderFactory, *m_RenderTargets->m_ForwardFramebuffer, *m_View, ForwardShadingPass::CreateParameters());

        GBufferFillPass::CreateParameters GBufferParams;
        GBufferParams.enableMotionVectors = true;
        GBufferParams.stencilWriteMask = motionVectorStencilMask;
        m_GBufferPass = std::make_unique<GBufferFillPass>(GetDevice(), m_CommonPasses);
        m_GBufferPass->Init(*m_ShaderFactory, *m_RenderTargets->m_GBufferFramebuffer, *m_View, GBufferParams);
        m_GBufferBindingSets.clear();

        m_DeferredLightingPass = std::make_unique<DeferredLightingPass>(GetDevice(), m_CommonPasses);
        m_DeferredLightingPass->Init(*m_ShaderFactory, *m_RenderTargets->m_HdrFramebuffer, *m_View);

        m_SkyPass = std::make_unique<SkyPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->m_ForwardFramebuffer, *m_View);

        if (m_EnvironmentMap->texture)
        {
            m_EnvironmentMapPass = std::make_unique<EnvironmentMapPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->m_ForwardFramebuffer, *m_View, m_EnvironmentMap->texture);
        }

        m_TemporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, *m_View,
            m_RenderTargets->m_Depth,
            m_RenderTargets->m_MotionVectors,
            m_RenderTargets->m_HdrColor,
            m_RenderTargets->m_ResolvedColor,
            m_RenderTargets->m_TemporalFeedback1,
            m_RenderTargets->m_TemporalFeedback2,
            motionVectorStencilMask,
            true);

        // NGX DLSS Initialization code
        // Don't need to reinit NGX each render target change
        assert(m_NGXWrapper);
        if (m_NGXWrapper && m_NGXWrapper->IsNGXInitialized())
        {
            if (m_NGXWrapper->IsDLSSInitialized())
            {
                m_NGXWrapper->ReleaseDLSSFeatures();
            }

            bool isContentHDR = m_ui.PreTonemapping;
            bool depthInverted = m_View->IsReverseDepth();
            float depthScale = 1.0f;

            m_NGXWrapper->InitializeDLSSFeatures(creationTimeRenderSize,
                                                 m_RenderTargets->m_DisplaySize,
                                                 isContentHDR, depthInverted,
                                                 depthScale, m_SharpeningOn,
                                                 m_AutoExposureOn, m_ui.PERF_QUALITY_LIST[m_ui.PerfModeListIdx].PerfQuality,
                                                 m_ui.RenderPresetSelected);
        }

        m_SsaoPass = std::make_unique<SsaoPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->m_HdrFramebuffer, m_RenderTargets->m_Depth, m_RenderTargets->m_GBufferNormals, *m_View, true);

        m_LightProbePass = std::make_shared<LightProbeProcessingPass>(GetDevice(), m_ShaderFactory, m_CommonPasses);

        nvrhi::TextureHandle exposureTexture = nullptr;
        if (m_ToneMappingPass)
            exposureTexture = m_ToneMappingPass->GetExposureTexture();
        else
            exposureResetRequired = true;

        m_ToneMappingPass = std::make_unique<ToneMappingPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->m_LdrFramebuffer, *m_TonemappingView, 256, false, exposureTexture);

        m_BloomPass = std::make_unique<BloomPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->m_HdrFramebuffer, *m_View);

        m_PreviousViewsValid = false;
    }

    virtual void RenderSplashScreen(nvrhi::IFramebuffer* framebuffer) override
    {
        nvrhi::ITexture* framebufferTexture = framebuffer->GetDesc().colorAttachments[0].texture;
        m_CommandList->open();
        m_CommandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));
        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);
        GetDeviceManager()->SetVsyncEnabled(true);
    }

    nvrhi::IBindingSet* GetGBufferBindingSet(nvrhi::ITexture* indirectDiffuse)
    {
        nvrhi::BindingSetHandle& bindingSet = m_GBufferBindingSets[indirectDiffuse];

        if (!bindingSet)
        {
            bindingSet = m_DeferredLightingPass->CreateGBufferBindingSet(
                nvrhi::AllSubresources,
                m_RenderTargets->m_Depth,
                m_RenderTargets->m_GBufferDiffuse,
                m_RenderTargets->m_GBufferSpecular,
                m_RenderTargets->m_GBufferNormals,
                indirectDiffuse
            );
        }

        return bindingSet;
    }

    void FillRecommendedSettings(int2 inDisplaySize)
    {
        if (all(m_recommendedSettingsLastSize == inDisplaySize))
        {
            return; // no need to refresh recommended Settings as the display resolution hasn't changed
        }
        for (PERF_QUALITY_ITEM &item : m_ui.PERF_QUALITY_LIST)
        {
            m_NGXWrapper->QueryOptimalSettings(inDisplaySize, item.PerfQuality, &m_RecommendedSettingsMap[item.PerfQuality]);
            if (m_RecommendedSettingsMap[item.PerfQuality].m_ngxRecommendedOptimalRenderSize.x == 0 ||
                m_RecommendedSettingsMap[item.PerfQuality].m_ngxRecommendedOptimalRenderSize.y == 0)
            {
                item.PerfQualityAllowed = false;
            }
            else
            {
                item.PerfQualityAllowed = true;
            }
            if (all(m_RecommendedSettingsMap[item.PerfQuality].m_ngxDynamicMaximumRenderSize ==
                    m_RecommendedSettingsMap[item.PerfQuality].m_ngxDynamicMinimumRenderSize))
            {
                item.PerfQualityDynamicAllowed = false;
            }
            else
            {
                item.PerfQualityDynamicAllowed = true;
            }
        }
        m_recommendedSettingsLastSize = inDisplaySize;
    }

    void AdvanceFrame()
    {
        m_FrameIndex = (m_FrameIndex + 1) % NUM_OFFSET_SEQUENCES;

        m_CameraPreviousMatrix = m_Camera.GetWorldToViewMatrix();
    }

    dm::float2 GetCurrentPixelOffset()
    {
        // Halton jitter
        float2 Result(0.0f, 0.0f);

        constexpr int BaseX = 2;
        int Index = m_FrameIndex + 1;
        float InvBase = 1.0f / BaseX;
        float Fraction = InvBase;
        while (Index > 0)
        {
            Result.x += (Index % BaseX) * Fraction;
            Index /= BaseX;
            Fraction *= InvBase;
        }

        constexpr int BaseY = 3;
        Index = m_FrameIndex + 1;
        InvBase = 1.0f / BaseY;
        Fraction = InvBase;
        while (Index > 0)
        {
            Result.y += (Index % BaseY) * Fraction;
            Index /= BaseY;
            Fraction *= InvBase;
        }

        Result.x -= 0.5f;
        Result.y -= 0.5f;
        return Result;
    }

    virtual void RenderScene(nvrhi::IFramebuffer* framebuffer) override
    {
        int windowWidth, windowHeight;
        GetDeviceManager()->GetWindowDimensions(windowWidth, windowHeight);
        nvrhi::Viewport windowViewport = nvrhi::Viewport(float(windowWidth), float(windowHeight));

        bool exposureResetRequired = true;

        bool needNewPasses = false;
        int2 displaySize = int2(windowWidth, windowHeight);
        int2 requiredRendertargetSize = displaySize;
        int2 renderSize               = displaySize;
        int2 dlssCreationTimeRenderSize = displaySize;
        int2 renderOffset             = {0, 0};
        auto PerfQualityMode          = m_ui.PERF_QUALITY_LIST[m_ui.PerfModeListIdx].PerfQuality;
        float lodBias                 = 0.f;
        bool preTonemapping           = true;

        // DLSS INTEGRATION POINT
        // Pick an appropriate rendering resolution.  For DLSS inference, ask NGX based on display
        if (m_ui.AntiAliasingMode == UIData::AntiAliasingMode::DLSS)
        {
            FillRecommendedSettings(displaySize);

            requiredRendertargetSize = m_RecommendedSettingsMap[PerfQualityMode].m_ngxDynamicMaximumRenderSize;

            float texLodXDimension;

            if (m_ui.ScaleRatio == OPTIMAL_RATIO)
            {
                dlssCreationTimeRenderSize = m_RecommendedSettingsMap[PerfQualityMode].m_ngxRecommendedOptimalRenderSize;
                renderSize = dlssCreationTimeRenderSize;
                texLodXDimension = renderSize.x;
            }
            else if (m_ui.ScaleRatio == VARIABLE_RATIO)
            {
                dlssCreationTimeRenderSize = m_RecommendedSettingsMap[PerfQualityMode].m_ngxRecommendedOptimalRenderSize;
                // in variable ratio mode, pick a random ratio between min and max rendering resolution
                int2 maxSize = m_RecommendedSettingsMap[PerfQualityMode].m_ngxDynamicMaximumRenderSize;
                int2 minSize = m_RecommendedSettingsMap[PerfQualityMode].m_ngxDynamicMinimumRenderSize;
                std::uniform_int_distribution<int> distributionWidth(minSize.x, maxSize.x);
                std::uniform_int_distribution<int> distributionHeight(minSize.y, maxSize.y);
                renderSize = { distributionWidth(m_Generator), distributionHeight(m_Generator) };
                assert(all(renderSize <= requiredRendertargetSize));
                // if maxSize is the same as minSize then dynamic resolution is not supported. Also subrectangle support may be off with an older DLSS dll.
                if (all(maxSize == minSize))
                {
                    // Offset should be between 0 and resource size
                    std::uniform_int_distribution<int> distributionX(0, requiredRendertargetSize.x - renderSize.x);
                    std::uniform_int_distribution<int> distributionY(0, requiredRendertargetSize.y - renderSize.y);
                    renderOffset = { distributionX(m_Generator), distributionY(m_Generator) };
                }
                // For Variable ratio, we want to choose the minimum rendering size
                // to select a Texture LOD that will preserve its sharpness over a large range of rendering resolution.
                // Ideally, the texture LOD would be allowed to be variable as well based on the dynamic scale
                // but we don't support that here yet.
                texLodXDimension = minSize.x;
            }
            else
            {
                dlssCreationTimeRenderSize = int2((int)(displaySize.x * m_ui.ScaleRatio), (int)(displaySize.y * m_ui.ScaleRatio));
                // Make sure the rendertarget is large enough to accommodate a forced ratio
                requiredRendertargetSize = int2(max(requiredRendertargetSize.x, dlssCreationTimeRenderSize.x), max(requiredRendertargetSize.y, dlssCreationTimeRenderSize.y));
                renderSize = dlssCreationTimeRenderSize;
                texLodXDimension = renderSize.x;
            }

            // Use the formula of the DLSS programming guide for the Texture LOD Bias...
            lodBias = std::log2f(texLodXDimension / displaySize.x) - 1.0f;
            m_ui.DefaultLodBias = lodBias;
            // ... but leave the opportunity to override it in the UI
            if (m_ui.OverrideLodBias)
            {
                lodBias = m_ui.ForcedLodBias;
            }

            preTonemapping = m_ui.PreTonemapping;
        }

        if (!m_RenderTargets || m_RenderTargets->IsUpdateRequired(requiredRendertargetSize, displaySize, preTonemapping))
        {
            // Whenever we reallocate rendertargets, start with rendersize and offset at their default values.
            renderOffset             = {0, 0};
            if (m_ui.AntiAliasingMode == UIData::AntiAliasingMode::DLSS)
            {
                renderSize = dlssCreationTimeRenderSize;
            }
            else
            {
                renderSize = displaySize;
            }
            m_RenderTargets = nullptr;
            m_CommonPasses->ResetBindingCache();
            m_RenderTargets = std::make_unique<RenderTargets>(GetDevice(), requiredRendertargetSize, displaySize, preTonemapping);

            needNewPasses = true;
        }

        // Render scene, change bias
        if (m_PreviousLodBias != lodBias)
        {
            needNewPasses = true;
            m_PreviousLodBias = lodBias;
        }

        if (SetupView(renderSize, renderOffset, preTonemapping))
        {
            needNewPasses = true;
        }

        if (m_ui.ShaderReloadRequested)
        {
            m_ShaderFactory->ClearCache();
            needNewPasses = true;
        }

        if (m_ui.EnableAutoExposure != m_AutoExposureOn)
        {
            m_AutoExposureOn = m_ui.EnableAutoExposure;
            needNewPasses = true;
        }

        if (any(m_PreviousCreationTimeSize != dlssCreationTimeRenderSize))
        {
            m_PreviousCreationTimeSize = dlssCreationTimeRenderSize;
            needNewPasses = true;
        }

        if (m_ui.RenderPresetSelectionHasChanged)
        {
            m_ui.RenderPresetSelectionHasChanged = false;
            needNewPasses = true;
        }

        if(needNewPasses)
        {
            CreateRenderPasses(dlssCreationTimeRenderSize, lodBias, exposureResetRequired);
        }

        m_ui.ShaderReloadRequested = false;

        m_CommandList->open();
        m_ToneMappingPass->BeginTrackingState(m_CommandList);

        nvrhi::ITexture* framebufferTexture = framebuffer->GetDesc().colorAttachments[0].texture;
        m_CommandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));

        DaylightInformation daylight = CalculateDaylightInformation(-m_SunLight->direction);
        m_AmbientTop = daylight.AmbientColor * m_ui.AmbientIntensity * m_SunLight->irradiance;
        m_AmbientBottom = m_AmbientTop * float3(0.3f, 0.4f, 0.3f);
        m_SunLight->color = daylight.SunColor;
        if (m_ui.EnableShadows)
        {
            m_SunLight->shadowMap = m_ShadowMap;
            box3 sceneBounds = m_Scene->GetSceneBounds();

            frustum projectionFrustum = m_View->GetProjectionFrustum();
            projectionFrustum = projectionFrustum.grow(1.f); // to prevent volumetric light leaking
            const float maxShadowDistance = 100.f;

            dm::affine3 viewMatrixInv = m_View->GetChildView(ViewType::PLANAR, 0)->GetInverseViewMatrix();

            float zRange = length(sceneBounds.diagonal()) * 0.5f;
            m_ShadowMap->SetupForPlanarViewStable(*m_SunLight, projectionFrustum, viewMatrixInv, maxShadowDistance, zRange, zRange, m_ui.CsmExponent);

            m_ShadowMap->Clear(m_CommandList);

            RenderCompositeView(m_CommandList, 
                &m_ShadowMap->GetView(), nullptr, 
                *m_ShadowFramebuffer, 
                *m_OpaqueDrawStrategy, 
                *m_ShadowDepthPass, 
                "ShadowMap",
                m_ui.EnableMaterialEvents);
        }
        else
        {
            m_SunLight->shadowMap = nullptr;
        }

        std::vector<std::shared_ptr<LightProbe>> lightProbes; // no probes
        m_RenderTargets->Clear(m_CommandList);

        if (exposureResetRequired)
            m_ToneMappingPass->ResetExposure(m_CommandList, 8.f);

        if (m_ui.EnableTranslucency)
        {
            m_ForwardPass->PrepareLights(m_CommandList, m_Scene->Lights, m_AmbientTop, m_AmbientBottom, lightProbes);
        }

        RenderCompositeView(m_CommandList, 
            m_View.get(), m_ViewPrevious.get(), 
            *m_RenderTargets->m_GBufferFramebuffer, 
            *m_OpaqueDrawStrategy, 
            *m_GBufferPass, 
            "GBufferFill",
            m_ui.EnableMaterialEvents);

        m_DeferredLightingPass->Render(m_CommandList, *m_RenderTargets->m_HdrFramebuffer, *m_View, m_Scene->Lights,
            GetGBufferBindingSet(nullptr), m_AmbientTop, m_AmbientBottom, lightProbes);

        if (m_EnvironmentMapPass && !m_ui.EnableProceduralSky)
            m_EnvironmentMapPass->Render(m_CommandList, *m_View);
        else
            m_SkyPass->Render(m_CommandList, *m_View, *m_SunLight);

        if (m_ui.EnableSsao)
        {
#ifdef DONUT_WITH_HBAO_PLUS
            if (m_HbaoPlusPass->CanRender())
            {
                m_HbaoPlusPass->Render(m_CommandList, m_ui.SsaoParameters, *m_View, m_RenderTargets->m_Depth, m_RenderTargets->m_GBufferNormals, m_RenderTargets->m_HdrColor);
            }
            else
#endif
            {
                float2 randomOffset = float2(float(rand()), float(rand()));
                m_SsaoPass->Render(m_CommandList, m_ui.SsaoParameters, *m_View, randomOffset);
            }
        }

        if (m_ui.EnableTranslucency)
        {
            RenderCompositeView(m_CommandList,
                m_View.get(), m_ViewPrevious.get(),
                *m_RenderTargets->m_ForwardFramebuffer,
                *m_TransparentDrawStrategy,
                *m_ForwardPass,
                "ForwardTransparent",
                m_ui.EnableMaterialEvents);
        }

        nvrhi::ITexture* renderColor = m_RenderTargets->m_HdrColor;
        nvrhi::ITexture* preResolveColor;
        nvrhi::ITexture* postResolveColor;
        nvrhi::ITexture* finalTonemappedColor;

        if (m_ui.EnableBloom)
        {
            float effectiveBloomSigma = m_ui.BloomSigma * (((float)renderSize.x) / m_RenderTargets->m_DisplaySize.x);
            m_BloomPass->Render(m_CommandList, m_RenderTargets->m_HdrFramebuffer, *m_View, renderColor, effectiveBloomSigma);
        }


        m_ui.ToneMappingParams.minAdaptedLuminance = 0.1f;
        m_ui.ToneMappingParams.maxAdaptedLuminance = m_ui.ToneMappingParams.minAdaptedLuminance;
        m_ui.ToneMappingParams.eyeAdaptationSpeedDown = 0.0f;
        m_ui.ToneMappingParams.eyeAdaptationSpeedUp = m_ui.ToneMappingParams.eyeAdaptationSpeedDown;
        if (!preTonemapping)
        {
            m_ToneMappingPass->SimpleRender(m_CommandList, m_ui.ToneMappingParams, *m_TonemappingView, renderColor);
            preResolveColor = m_RenderTargets->m_LdrColor;
        }
        else
        {
            // will tonemap after resolve, simply pass through
            preResolveColor = renderColor;
        }

        if ((m_ui.AntiAliasingMode == UIData::AntiAliasingMode::DLSS) &&
            !m_NGXWrapper->IsDLSSAvailable())
        {
            log::warning("DLSS is not available.  NGX wasn't initialized, or scale factor recommended DLSS to be off as perf too high.");
            m_ui.AntiAliasingMode = UIData::AntiAliasingMode::TEMPORAL;    // switch back
        }

        if ((m_ui.AntiAliasingMode == UIData::AntiAliasingMode::TEMPORAL) ||
            (m_ui.AntiAliasingMode == UIData::AntiAliasingMode::DLSS))
        {
            if (m_PreviousViewsValid)
            {
                m_TemporalAntiAliasingPass->RenderMotionVectors(m_CommandList, *m_View, *m_ViewPrevious);
            }

             // NGX DLSS SuperSampling Feature Evaluation
            if (m_ui.AntiAliasingMode == UIData::AntiAliasingMode::DLSS)
            {
                bool ResetScene = !m_PreviousViewsValid || m_ui.ForceReset;
                //static unsigned int frameCount = 0;
                //frameCount++;
                //ResetScene = ((frameCount % 20) == 0);
                std::shared_ptr<PlanarView> renderPlanarView = std::dynamic_pointer_cast<PlanarView, IView>(m_View);
                float2 jitterOffset = renderPlanarView->GetPixelOffset();

                m_NGXWrapper->EvaluateSuperSampling(m_CommandList, preResolveColor, m_RenderTargets->m_ResolvedColor,
                    m_RenderTargets->m_MotionVectors, m_RenderTargets->m_Depth, m_ToneMappingPass->GetExposureTexture(), *m_View, ResetScene, true, jitterOffset);
            }
            else
            {
                m_TemporalAntiAliasingPass->TemporalResolve(m_CommandList, m_ui.TemporalAntiAliasingParams, m_PreviousViewsValid, *m_View, m_PreviousViewsValid ? *m_ViewPrevious : *m_View);
            }

            postResolveColor = m_RenderTargets->m_ResolvedColor;

            m_PreviousViewsValid = true;
        }
        else
        {
            // Resolve is a simple pass through
            postResolveColor = preResolveColor;

            m_PreviousViewsValid = false;
        }

        if (preTonemapping)
        {
            m_ToneMappingPass->SimpleRender(m_CommandList, m_ui.ToneMappingParams, *m_TonemappingView, postResolveColor);
            finalTonemappedColor = m_RenderTargets->m_LdrColor;
        }
        else
        {
            // Already tonemapped, just pass through
            finalTonemappedColor = postResolveColor;
        }

        m_CommonPasses->BlitTexture(m_CommandList, framebuffer, windowViewport, box2(0.f, 1.f), finalTonemappedColor, box2(0.f, 1.f));

        m_ToneMappingPass->SaveCurrentState(m_CommandList);

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);

        if (!m_ui.ScreenshotFileName.empty())
        {
            SaveTextureToFile(GetDevice(), m_CommonPasses.get(), framebufferTexture, nvrhi::ResourceStates::RENDER_TARGET, m_ui.ScreenshotFileName.c_str());
            m_ui.ScreenshotFileName = "";
        }

        m_TemporalAntiAliasingPass->AdvanceFrame();

        AdvanceFrame();

        GetDeviceManager()->SetVsyncEnabled(m_ui.EnableVsync);
    }

    std::shared_ptr<ShaderFactory> GetShaderFactory()
    {
        return m_ShaderFactory;
    }

    bool IsFeatureSupported(NVSDK_NGX_FeatureDiscoveryInfo* dis)
    {
        return m_NGXWrapper->IsFeatureSupported(dis);
    }

};

// Just some UX glue code.  Go here to add a button or muck with the UX controls.
#include "glue/UIRenderer.hpp"

#ifdef _WIN32
bool ProcessCommandLine(DeviceCreationParameters& deviceParams, nvrhi::GraphicsAPI api, bool& useNgxSdkExtApi)
{
    LPWSTR *szArglist;
    int nArgs;
    int i;

    szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (!szArglist)
    {
        return false;
    }
    else
    {
        for (i = 0; i < nArgs; i++)
        {
            if (!lstrcmpW(szArglist[i], L"-width"))
            {
                deviceParams.backBufferWidth = std::stoi(szArglist[++i]);
            }
            else if (!lstrcmpW(szArglist[i], L"-height"))
            {
                deviceParams.backBufferHeight = std::stoi(szArglist[++i]);
            }
            else if (!lstrcmpW(szArglist[i], L"-fullscreen"))
            {
                if (api != nvrhi::GraphicsAPI::VULKAN)
                {
                    deviceParams.startFullscreen = true;
                }
            }
            else if (!lstrcmpW(szArglist[i], L"-useNgxSdkExtApi"))
            {
                useNgxSdkExtApi = true;
            }
        }
    }

    return true;
}
#else
#include <getopt.h>
bool ProcessCommandLine(DeviceCreationParameters& deviceParams, UIData& uiData, nvrhi::GraphicsAPI api, bool& useNgxSdkExtApi, int argc, char *argv[])
{
    static const char *USAGE_STR =
        "Usage: ngx_dlss_demo [--width|-w <width>] [--height|-h <height>]\n"
        "       ngx_dlss_demo [--fullscreen|-f]\n"
        "       ngx_dlss_demo [--useNgxSdkExtApi]\n";
        "       ngx_dlss_demo [--aamode <value>]\n"
        "\n"
        "  --aamode\n"
        "       Set the starting antialiasing mode. Can be one of:\n"
        "              0: NONE\n"
        "              1: TEMPORAL\n"
        "              2: DLSS\n";

    static const struct option long_options[] = {
        {"width",      required_argument, 0, 'w' },
        {"height",     required_argument, 0, 'h' },
        {"fullscreen", no_argument,       0, 'f' },
        {"useNgxSdkExtApi",  no_argument,       0, 'n' },
        {"aamode",     required_argument, 0, 'a' },
        {"help",       no_argument,       0, 'z' },
        {0,            0,                 0,  0  }
    };

    while (true)
    {
        int option_index = 0;
        int c = getopt_long(argc, argv, "w:h:f", long_options, &option_index);
        if (c == -1) break;

        switch (c)
        {
        case 'w':
            deviceParams.backBufferWidth = atoi(optarg);
            break;
        case 'h':
            deviceParams.backBufferHeight = atoi(optarg);
            break;
        case 'f':
            if (api != nvrhi::GraphicsAPI::VULKAN)
            {
                deviceParams.startFullscreen = true;
            }
            break;
        case 'n':
            useNgxSdkExtApi = true;
            break;
        case 'a':
            {
                int aamode = atoi(optarg);
                switch (aamode)
                {
                case 0:
                    uiData.AntiAliasingMode = UIData::AntiAliasingMode::NONE;
                    break;
                case 1:
                    uiData.AntiAliasingMode = UIData::AntiAliasingMode::TEMPORAL;
                    break;
                case 2:
                    uiData.AntiAliasingMode = UIData::AntiAliasingMode::DLSS;
                    break;
                default:
                    printf("%s: Invalid --aamode value '%d'\n"
                           "Try '%s --help' for more information.\n",
                           argv[0], aamode, argv[0]);
                    exit(1);
                    break;
                }
            }
            break;
        case 'z':
            printf("%s\n", USAGE_STR);
            exit(0);
            break;
        case '?':
        default:
            printf("Try '%s --help' for more information.\n", argv[0]);
            exit(1);
            break;
        }
    }

    return true;
}
#endif


#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);

    // disable steam overlay. Could do this vk conditionally, but it shouldnt hurt anything.
    // This is to fix potential issues caused by the steam overlay, at worst preventing vulkan initialization due to steam issues.
    _putenv("DISABLE_VK_LAYER_VALVE_steam_overlay_1=1");
#else //  _WIN32
int main(int argc, char *argv[])
{
    nvrhi::GraphicsAPI api = nvrhi::GraphicsAPI::VULKAN;
#endif //  _WIN32
    DeviceCreationParameters deviceParams;
    UIData uiData;


#if !__linux__
    NvAPI_Initialize();
#endif

    deviceParams.backBufferWidth = 2560;
    deviceParams.backBufferHeight = 1440;
    deviceParams.swapChainSampleCount = 1;
    deviceParams.swapChainBufferCount = 3;
    deviceParams.startFullscreen = false;
    deviceParams.enablePerMonitorDPI = true;
#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true;
    deviceParams.enableNvrhiValidationLayer = true;
#endif
    deviceParams.vsyncEnabled = true;

    bool useNgxSdkExtApi = false;
#ifdef _WIN32
    if (!ProcessCommandLine(deviceParams, api, useNgxSdkExtApi))
#else
    if (!ProcessCommandLine(deviceParams, uiData, api, useNgxSdkExtApi, argc, argv))
#endif
    {
        log::error("Failed to process the command line.");
        return 1;
    }

    DeviceManager* deviceManager = DeviceManager::Create(api);
    const char* apiString = nvrhi::utils::GraphicsAPIToString(deviceManager->GetGraphicsAPI());

    std::string windowTitle = "NVIDIA NGX DLSS Sample (" + std::string(apiString) + ")";

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, windowTitle.c_str()))
    {
        log::error("Cannot initialize a %s graphics device with the requested parameters", apiString);
        return 1;
    }

    {
        std::shared_ptr<FeatureDemo> demo = std::make_shared<FeatureDemo>(deviceManager, uiData, useNgxSdkExtApi);
        std::shared_ptr<UIRenderer> gui = std::make_shared<UIRenderer>(deviceManager, demo, uiData);

        NVSDK_NGX_FeatureDiscoveryInfo dis;        
        memset(&dis, 0, sizeof(NVSDK_NGX_FeatureDiscoveryInfo));

        std::wstring ApplicationDataPath = L".";
        dis.SDKVersion                   = NVSDK_NGX_Version_API;
        dis.FeatureID                    = NVSDK_NGX_Feature_SuperSampling;
        dis.Identifier.IdentifierType    = NVSDK_NGX_Application_Identifier_Type_Application_Id;        
        dis.ApplicationDataPath          = ApplicationDataPath.data();
        dis.Identifier.v.ApplicationId   = 0x0023;
        
        if (!demo->IsFeatureSupported(&dis))
        {
            log::error("Requested NGX DLSS Feature not supported");
            return 1;
        }        

        gui->LoadFont(*demo->GetMediaFolder().GetFileSystem(), demo->GetMediaFolder().GetPath() / "OpenSansFont/OpenSans-Regular.ttf", 17.f);
        gui->Init(demo->GetShaderFactory());

        deviceManager->AddRenderPassToBack(demo.get());
        deviceManager->AddRenderPassToBack(gui.get());

        deviceManager->RunMessageLoop();
    }

    deviceManager->Shutdown();
#ifdef _DEBUG
    deviceManager->ReportLiveObjects();
#endif
    delete deviceManager;
    
    return 0;
}
