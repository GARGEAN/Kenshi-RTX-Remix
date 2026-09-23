#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>
#include <memory>

#include <nvsdk_ngx.h>

#define APP_ID 231313132

// maximum HDR value in the scene
#define NGX_HDRMax 1000.0f

// when flickering is detected bigger value will use more history to suppress it
#define NGX_FlickerFactor 0.5f

// threshold to determine low vs high contrast. low contrast areas get more current frame than history
#define NGX_ContrastFactor 0.05f

// higher value includes more of the current frame in motion (which can result in sharper images but also flickering)
#define NGX_MotionFactor 0.1f

// bigger the value more current frame will be used vs history (sharper vs blurrier image)
#define NGX_HistoryBlendFactor 0.15f

// higher the value more sharpening applies (and more halo around high contrast areas)
#define NGX_SharpenThreshold 0.3f

// bigger the value more history gets rejected when motion delta is high (current vs previous frame) to avoid ghosting
#define NGX_RejectFactor 0.02f

namespace donut::engine
{
    class ShaderFactory;
    class ShadowMap;
    class CommonRenderPasses;
    class FramebufferFactory;
    class IDrawStrategy;
    class ICompositeView;
}

// Forward decls from NGX library
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace donut::render
{
    struct DlssRecommendedSettings
    {
        float     m_ngxRecommendedSharpness         = 0.01f; // in ngx sdk 3.1, dlss sharpening is deprecated
        dm::uint2 m_ngxRecommendedOptimalRenderSize = {~(0u), ~(0u)};
        dm::uint2 m_ngxDynamicMaximumRenderSize     = {~(0u), ~(0u)};
        dm::uint2 m_ngxDynamicMinimumRenderSize     = {~(0u), ~(0u)};
    };

    // This is a wrapper around the NGX functionality for DLSS.  It is seperated to provide focus to the calls specific to NGX for code sample purposes.
    class NGXWrapper
    {
    private:
        nvrhi::DeviceHandle m_Device;
#ifdef _WIN32
        IDXGIAdapter*       m_Adapter {};
        bool FindAdapter(const std::wstring& targetName);
#endif


        bool m_ngxInitialized   = false;
        bool m_bDlssAvailable = false;

        NVSDK_NGX_Parameter* m_ngxParameters = nullptr;
        NVSDK_NGX_Handle* m_dlssFeature = nullptr;

        void InitializeNGX(nvrhi::IDevice* device, const wchar_t* rwLogsFolder);
        void ShutdownNGX();

    public:
        NGXWrapper(nvrhi::IDevice* device, const wchar_t* rwLogsFolder=L".")
            : m_Device(device)
        {
            InitializeNGX(device, rwLogsFolder);
        }
        ~NGXWrapper()
        {
            if (IsNGXInitialized())
            {
                ReleaseDLSSFeatures();
            }
            ShutdownNGX();
        }

        bool InitializeDLSSFeatures(dm::int2 optimalRenderSize, dm::int2 displayOutSize, int isContentHDR, bool depthInverted, float depthScale = 1.0f, bool enableSharpening = false, bool enableAutoExposure = false, NVSDK_NGX_PerfQuality_Value qualValue = NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_DLSS_Hint_Render_Preset renderPreset = NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
        void ReleaseDLSSFeatures();

        bool QueryOptimalSettings(dm::uint2 inDisplaySize, NVSDK_NGX_PerfQuality_Value inQualValue, DlssRecommendedSettings *outRecommendedSettings);

        bool IsDLSSAvailable() const { return m_bDlssAvailable; }

        bool IsDLSSInitialized() const { return IsNGXInitialized() && m_dlssFeature != nullptr; }
        bool IsNGXInitialized() const { return m_ngxInitialized; }

        bool IsFeatureSupported(NVSDK_NGX_FeatureDiscoveryInfo* dis);

        void EvaluateSuperSampling(
            nvrhi::ICommandList* commandList,
            nvrhi::ITexture* unresolvedColor,
            nvrhi::ITexture* resolvedColor,
            nvrhi::ITexture* motionVectors,
            nvrhi::ITexture* depth,
            nvrhi::ITexture* exposure,
            const engine::ICompositeView& compositeView,
            bool bResetAccumulation = false,
            bool bUseNgxSdkExtApi = false,
            donut::math::float2 jitterOffset = { 0.0f, 0.0f },
            donut::math::float2 mVScale = { 1.0f, 1.0f });
    };
}
