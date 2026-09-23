//----------------------------------------------------------------------------------
// File:        NGXWrapper.cpp
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

#ifndef DONUT_WITH_NGX
#error This sample is to demonstrate NGX DLSS.  Please configure Donut via Cmake to use NGX by setting DONUT_WITH_NGX option to ON
#endif

#include <NGXWrapper.h>
#include <donut/engine/View.h>
#include <donut/core/log.h>
#include <assert.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#if USE_DX11
#include <d3d11.h>
#include <nvrhi/d3d11/d3d11.h>
#endif
#if USE_DX12
#include <d3d12.h>
#include <nvrhi/d3d12/d3d12.h>
#endif
#if USE_VK
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvrhi/vulkan.h>
#include <nvsdk_ngx_helpers_vk.h>
#endif

using namespace donut::math;
using namespace donut::engine;
using namespace donut::render;

void NGXWrapper::InitializeNGX(nvrhi::IDevice* device, const wchar_t* rwLogsFolder)
{
    NVSDK_NGX_Result Result = NVSDK_NGX_Result_Fail;
#if USE_DX11
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
    {
        ID3D11Device* d3ddevice = m_Device->getNativeObject(nvrhi::ObjectTypes::D3D11_Device);
        Result = NVSDK_NGX_D3D11_Init(APP_ID, rwLogsFolder, d3ddevice);
    }
#endif

#if USE_DX12
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        ID3D12Device* d3ddevice = m_Device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
        Result = NVSDK_NGX_D3D12_Init(APP_ID, rwLogsFolder, d3ddevice);
    }
#endif

#if USE_VK
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        VkDevice vkdevice = m_Device->getNativeObject(nvrhi::ObjectTypes::VK_Device);
        VkPhysicalDevice vkphysicaldevice = m_Device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice);
        VkInstance vkinstance = m_Device->getNativeObject(nvrhi::ObjectTypes::VK_Instance);
         Result = NVSDK_NGX_VULKAN_Init(APP_ID, rwLogsFolder, vkinstance, vkphysicaldevice, vkdevice);
    }
#endif

    m_ngxInitialized = !NVSDK_NGX_FAILED(Result);
    if (!IsNGXInitialized())
    {
        if (Result == NVSDK_NGX_Result_FAIL_FeatureNotSupported || Result == NVSDK_NGX_Result_FAIL_PlatformError)
            log::info("NVIDIA NGX not available on this hardware/platform., code = 0x%08x, info: %ls", Result, GetNGXResultAsString(Result));
        else
            log::error("Failed to initialize NGX, error code = 0x%08x, info: %ls", Result, GetNGXResultAsString(Result));

        return;
    }

#if USE_DX11
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
    {
        Result = NVSDK_NGX_D3D11_GetCapabilityParameters(&m_ngxParameters);
    }
#endif

#if USE_DX12
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        Result = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_ngxParameters);
    }
#endif

#if USE_VK
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        Result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&m_ngxParameters);
    }
#endif

    if (NVSDK_NGX_FAILED(Result))
    {
        log::error("NVSDK_NGX_GetCapabilityParameters failed, code = 0x%08x, info: %ls", Result, GetNGXResultAsString(Result));
        ShutdownNGX();
        return;
    }

// Currently, the SDK and this sample are not in sync.  The sample is a bit forward looking,
// in this case.  This will likely be resolved very shortly, and therefore, the code below
// should be thought of as needed for a smooth user experience.
#if defined(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver)        \
    && defined (NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor) \
    && defined (NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor)

    // If NGX Successfully initialized then it should set those flags in return
    int needsUpdatedDriver = 0;
    unsigned int minDriverVersionMajor = 0;
    unsigned int minDriverVersionMinor = 0;
    NVSDK_NGX_Result ResultUpdatedDriver         = m_ngxParameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,    &needsUpdatedDriver);
    NVSDK_NGX_Result ResultMinDriverVersionMajor = m_ngxParameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minDriverVersionMajor);
    NVSDK_NGX_Result ResultMinDriverVersionMinor = m_ngxParameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minDriverVersionMinor);
    if (ResultUpdatedDriver == NVSDK_NGX_Result_Success &&
        ResultMinDriverVersionMajor == NVSDK_NGX_Result_Success &&
        ResultMinDriverVersionMinor == NVSDK_NGX_Result_Success)
    {
        if (needsUpdatedDriver)
        {
            log::error("NVIDIA DLSS cannot be loaded due to outdated driver. Minimum Driver Version required : %u.%u", minDriverVersionMajor, minDriverVersionMinor);

            ShutdownNGX();
            return;
        }
        else
        {
            log::info("NVIDIA DLSS Minimum driver version was reported as : %u.%u", minDriverVersionMajor, minDriverVersionMinor);
        }
    }
    else
    {
        log::info("NVIDIA DLSS Minimum driver version was not reported.");
    }

#endif

    int dlssAvailable  = 0;
    NVSDK_NGX_Result ResultDLSS = m_ngxParameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlssAvailable);
    if (ResultDLSS != NVSDK_NGX_Result_Success || !dlssAvailable)
    {
        // More details about what failed (per feature init result)
        NVSDK_NGX_Result FeatureInitResult = NVSDK_NGX_Result_Fail;
        NVSDK_NGX_Parameter_GetI(m_ngxParameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, (int*)&FeatureInitResult);
        log::error("NVIDIA DLSS not available on this hardward/platform., FeatureInitResult = 0x%08x, info: %ls", FeatureInitResult, GetNGXResultAsString(FeatureInitResult));
        ShutdownNGX();
        return;
    }

    // Setup some debug filtering
#if USE_DX11 
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
    {
        ID3D11Debug* d3dDebug = nullptr;
        const auto pD3D11Device = static_cast<ID3D11Device*>(device->getNativeObject(nvrhi::ObjectTypes::D3D11_Device));
        if (SUCCEEDED(pD3D11Device->QueryInterface(__uuidof(ID3D11Debug), (void**)& d3dDebug)))
        {
            ID3D11InfoQueue* d3dInfoQueue = nullptr;
            if (SUCCEEDED(d3dDebug->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)& d3dInfoQueue)))
            {
                d3dInfoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, true);
                d3dInfoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, true);

                D3D11_MESSAGE_ID hide[] =
                {
                D3D11_MESSAGE_ID_DEVICE_OMSETRENDERTARGETS_HAZARD,
                D3D11_MESSAGE_ID_DEVICE_CSSETSHADERRESOURCES_HAZARD,
                // Add more message IDs here as needed
                };

                D3D11_INFO_QUEUE_FILTER filter;
                memset(&filter, 0, sizeof(filter));
                filter.DenyList.NumIDs = _countof(hide);
                filter.DenyList.pIDList = hide;
                d3dInfoQueue->AddStorageFilterEntries(&filter);

                d3dInfoQueue->Release();
            }
            d3dDebug->Release();
        }
    }
#endif

}

void NGXWrapper::ShutdownNGX()
{
    if (IsNGXInitialized())
    {
        m_Device->waitForIdle();

        if (m_dlssFeature != nullptr)
        {
            log::warning("Attempt to release NGX library before features have been released!  Releasing now but should check your code.");
            ReleaseDLSSFeatures();
        }

#if USE_DX11
        if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
        {
            NVSDK_NGX_D3D11_DestroyParameters(m_ngxParameters);
            NVSDK_NGX_D3D11_Shutdown1(nullptr);
        }
#endif
#if USE_DX12
        if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
        {
            NVSDK_NGX_D3D12_DestroyParameters(m_ngxParameters);
            NVSDK_NGX_D3D12_Shutdown1(nullptr);
        }
#endif
#if USE_VK
        if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        {
            NVSDK_NGX_VULKAN_DestroyParameters(m_ngxParameters);
            NVSDK_NGX_VULKAN_Shutdown1(nullptr);
        }
#endif

        m_ngxInitialized = false;
    }
}

#ifdef _WIN32
bool NGXWrapper::FindAdapter(const std::wstring& targetName)
{
    if (m_Adapter != nullptr)
        return true;

    IDXGIFactory1*            DXGIFactory;
    HRESULT hres = CreateDXGIFactory1(IID_PPV_ARGS(&DXGIFactory));
    if (hres != S_OK)
    {
        donut::log::error("ERROR in CreateDXGIFactory.\n"
            "For more info, get log from debug D3D runtime: (1) Install DX SDK, and enable Debug D3D from DX Control Panel Utility. (2) Install and start DbgView. (3) Try running the program again.\n");
        return false;
    }

    unsigned int adapterNo = 0;
    while (SUCCEEDED(hres))
    {
        IDXGIAdapter* pAdapter;
        hres = DXGIFactory->EnumAdapters(adapterNo, &pAdapter);

        if (SUCCEEDED(hres))
        {
            DXGI_ADAPTER_DESC aDesc;
            pAdapter->GetDesc(&aDesc);

            // If no name is specified, return the first adapater.  This is the same behaviour as the
            // default specified for D3D11CreateDevice when no adapter is specified.
            if (targetName.length() == 0)
            {
                m_Adapter = pAdapter;
                return true;
            }

            std::wstring aName = aDesc.Description;

            if (aName.find(targetName) != std::string::npos)
            {
                m_Adapter = pAdapter;
                return true;
            }
        }

        adapterNo++;
    }

    return false;

}
#endif


bool NGXWrapper::InitializeDLSSFeatures(dm::int2 optimalRenderSize, dm::int2 displayOutSize, int isContentHDR, bool depthInverted, float depthScale, bool enableSharpening, bool enableAutoExposure, NVSDK_NGX_PerfQuality_Value qualValue, NVSDK_NGX_DLSS_Hint_Render_Preset renderPreset)
{
    if (!IsNGXInitialized())
    {
        log::error("Attempt to InitializeDLSSFeature without NGX being initialized.");
        return false;
    }

    unsigned int CreationNodeMask = 1;
    unsigned int VisibilityNodeMask = 1;
    NVSDK_NGX_Result ResultDLSS = NVSDK_NGX_Result_Fail;

    int MotionVectorResolutionLow = 1; // we let the Snippet do the upsampling of the motion vector
    // Next create features	
    int DlssCreateFeatureFlags = NVSDK_NGX_DLSS_Feature_Flags_None;
    DlssCreateFeatureFlags |= MotionVectorResolutionLow ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0;
    DlssCreateFeatureFlags |= isContentHDR ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0;
    DlssCreateFeatureFlags |= depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0;
    DlssCreateFeatureFlags |= enableSharpening ? NVSDK_NGX_DLSS_Feature_Flags_DoSharpening : 0;
    DlssCreateFeatureFlags |= enableAutoExposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0;

    NVSDK_NGX_DLSS_Create_Params DlssCreateParams;

    memset(&DlssCreateParams, 0, sizeof(DlssCreateParams));

    DlssCreateParams.Feature.InWidth = optimalRenderSize.x;
    DlssCreateParams.Feature.InHeight = optimalRenderSize.y;
    DlssCreateParams.Feature.InTargetWidth = displayOutSize.x;
    DlssCreateParams.Feature.InTargetHeight = displayOutSize.y;
    DlssCreateParams.Feature.InPerfQualityValue = qualValue;
    DlssCreateParams.InFeatureCreateFlags = DlssCreateFeatureFlags;

    // Select render preset (DL weights)
    NVSDK_NGX_Parameter_SetUI(m_ngxParameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, renderPreset);              // will remain the chosen weights after OTA
    NVSDK_NGX_Parameter_SetUI(m_ngxParameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, renderPreset);           // ^
    NVSDK_NGX_Parameter_SetUI(m_ngxParameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, renderPreset);          // ^
    NVSDK_NGX_Parameter_SetUI(m_ngxParameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, renderPreset);       // ^
    NVSDK_NGX_Parameter_SetUI(m_ngxParameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, renderPreset);  // ^

#if USE_DX11
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
    {
        ID3D11DeviceContext* deviceContext = m_Device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);

        // Actually create the features
        ResultDLSS = NGX_D3D11_CREATE_DLSS_EXT(deviceContext, &m_dlssFeature, m_ngxParameters, &DlssCreateParams);

        if (NVSDK_NGX_FAILED(ResultDLSS))
        {
            log::error("Failed to create DLSS Features = 0x%08x, info: %ls", ResultDLSS, GetNGXResultAsString(ResultDLSS));
            return false;
        }
    }
#endif
#if USE_DX12
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        nvrhi::CommandListHandle commandList = m_Device->createCommandList();
        commandList->open();

        ID3D12GraphicsCommandList* d3d12CommandList = commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);

        ResultDLSS = NGX_D3D12_CREATE_DLSS_EXT(d3d12CommandList, CreationNodeMask, VisibilityNodeMask, &m_dlssFeature, m_ngxParameters, &DlssCreateParams);

        commandList->close();

        if (NVSDK_NGX_FAILED(ResultDLSS))
        {
            log::error("Failed to create DLSS Features = 0x%08x, info: %ls", ResultDLSS, GetNGXResultAsString(ResultDLSS));
            return false;
        }

        m_Device->executeCommandList(commandList);
    }
#endif
#if USE_VK
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        nvrhi::CommandListHandle commandList = m_Device->createCommandList();
        commandList->open();

        VkCommandBuffer vkCommandBuffer = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);

        ResultDLSS = NGX_VULKAN_CREATE_DLSS_EXT(vkCommandBuffer, CreationNodeMask, VisibilityNodeMask, &m_dlssFeature, m_ngxParameters, &DlssCreateParams);

        commandList->close();

        if (NVSDK_NGX_FAILED(ResultDLSS))
        {
            log::error("Failed to create DLSS Features = 0x%08x, info: %ls", ResultDLSS, GetNGXResultAsString(ResultDLSS));
            return false;
        }

        m_Device->executeCommandList(commandList);
    }
#endif

    m_bDlssAvailable = true;

    return m_bDlssAvailable;
}

void NGXWrapper::ReleaseDLSSFeatures()
{
    if (!IsNGXInitialized())
    {
        log::error("Attempt to ReleaseDLSSFeatures without NGX being initialized.");
        return;
    }

    m_Device->waitForIdle();

#if USE_DX11
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
    {
        NVSDK_NGX_Result ResultDLSS = (m_dlssFeature != nullptr) ? NVSDK_NGX_D3D11_ReleaseFeature(m_dlssFeature) : NVSDK_NGX_Result_Success;
        if (NVSDK_NGX_FAILED(ResultDLSS))
        {
                log::error("Failed to NVSDK_NGX_D3D11_ReleaseFeature, code = 0x%08x, info: %ls", ResultDLSS, GetNGXResultAsString(ResultDLSS));
        }
    }
#endif

#if USE_DX12
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        NVSDK_NGX_Result ResultDLSS = (m_dlssFeature != nullptr) ? NVSDK_NGX_D3D12_ReleaseFeature(m_dlssFeature) : NVSDK_NGX_Result_Success;
        if (NVSDK_NGX_FAILED(ResultDLSS))
        {
            log::error("Failed to NVSDK_NGX_D3D12_ReleaseFeature, code = 0x%08x, info: %ls", ResultDLSS, GetNGXResultAsString(ResultDLSS));
        }

    }
#endif

#if USE_VK
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        NVSDK_NGX_Result ResultDLSS = (m_dlssFeature != nullptr) ? NVSDK_NGX_VULKAN_ReleaseFeature(m_dlssFeature) : NVSDK_NGX_Result_Success;
        if (NVSDK_NGX_FAILED(ResultDLSS))
        {
            log::error("Failed to NVSDK_NGX_D3D12_ReleaseFeature, code = 0x%08x, info: %ls", ResultDLSS, GetNGXResultAsString(ResultDLSS));
        }
    }
#endif

    m_dlssFeature = nullptr;    
}

// Not everything is returned from GET_OPTIMAL_SETTINGS as it is unneeded for this sample.  However, everything is 
// saved off for future use, since this sample should be updated as needed.
bool NGXWrapper::QueryOptimalSettings(uint2 inDisplaySize, NVSDK_NGX_PerfQuality_Value inQualValue, DlssRecommendedSettings *outRecommendedSettings)
{
    if (!IsNGXInitialized())
    {
        outRecommendedSettings->m_ngxRecommendedOptimalRenderSize = inDisplaySize;
        outRecommendedSettings->m_ngxDynamicMaximumRenderSize     = inDisplaySize;
        outRecommendedSettings->m_ngxDynamicMinimumRenderSize     = inDisplaySize;

        log::info("NGX was not initialized when querying Optimal Settings");
        return false;
    }

    NVSDK_NGX_Result Result = NGX_DLSS_GET_OPTIMAL_SETTINGS(m_ngxParameters,
        inDisplaySize.x, inDisplaySize.y, inQualValue,
        &outRecommendedSettings->m_ngxRecommendedOptimalRenderSize.x, &outRecommendedSettings->m_ngxRecommendedOptimalRenderSize.y,
        &outRecommendedSettings->m_ngxDynamicMaximumRenderSize.x, &outRecommendedSettings->m_ngxDynamicMaximumRenderSize.y,
        &outRecommendedSettings->m_ngxDynamicMinimumRenderSize.x, &outRecommendedSettings->m_ngxDynamicMinimumRenderSize.y,
        &outRecommendedSettings->m_ngxRecommendedSharpness);

    if (NVSDK_NGX_FAILED(Result))
    {
        outRecommendedSettings->m_ngxRecommendedOptimalRenderSize   = inDisplaySize;
        outRecommendedSettings->m_ngxDynamicMaximumRenderSize       = inDisplaySize;
        outRecommendedSettings->m_ngxDynamicMinimumRenderSize       = inDisplaySize;
        outRecommendedSettings->m_ngxRecommendedSharpness           = 0.0f;

        log::warning("Querying Optimal Settings failed! code = 0x%08x, info: %ls", Result, GetNGXResultAsString(Result));
        return false;
    }

    return true;
}

bool NGXWrapper::IsFeatureSupported(NVSDK_NGX_FeatureDiscoveryInfo* dis)
{
    NVSDK_NGX_Result             res;
    NVSDK_NGX_FeatureRequirement req;

#ifdef _WIN32
    if (!FindAdapter(L""))
    {
        return false;
    }

    res = NVSDK_NGX_UpdateFeature(&dis->Identifier, dis->FeatureID);
    // expect to fail for now. Don't check the return value until it's implemented.
    log::info("NVSDK_NGX_UpdateFeature returned 0x%08x info: %ls", res, GetNGXResultAsString(res));

    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        res = NVSDK_NGX_D3D12_GetFeatureRequirements(m_Adapter, dis, &req);
    }
    else if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
    {
        res = NVSDK_NGX_D3D11_GetFeatureRequirements(m_Adapter, dis, &req);
    }
    else
#endif
#if USE_VK
    {        
        VkDevice vkdevice = m_Device->getNativeObject(nvrhi::ObjectTypes::VK_Device);
        VkPhysicalDevice vkphysicaldevice = m_Device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice);
        VkInstance vkinstance = m_Device->getNativeObject(nvrhi::ObjectTypes::VK_Instance);

        uint32_t nDeviceExtensions;
        VkExtensionProperties* deviceExtensions;
        res = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(vkinstance, vkphysicaldevice, dis, &nDeviceExtensions, &deviceExtensions);
        if (res != NVSDK_NGX_Result_Success)
        {
            log::error("GetFeatureRequirements error: NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements returned 0x%08x info: %ls\n", res, GetNGXResultAsString(res));
            return false;
        }
        else
        {
            log::info("NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements returned %d device extension requirements: ", nDeviceExtensions);
            for (uint32_t i = 0; i < nDeviceExtensions; i++)
            {
                log::info("%s, ", deviceExtensions[i].extensionName);
            }
            log::info("\n");
        }

        uint32_t nInstanceExtensions;
        VkExtensionProperties* instanceExtensions;
        res = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(dis, &nInstanceExtensions, &instanceExtensions);
        if (res != NVSDK_NGX_Result_Success)
        {
            log::info("GetFeatureRequirements error: NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements returned 0x%08x info: %ls\n", res, GetNGXResultAsString(res));
            return 1;
        }
        else
        {
            log::info("NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements returned %d instance extension requirements: ", nInstanceExtensions);
            for (uint32_t i = 0; i < nInstanceExtensions; i++)
            {
                log::info("%s, ", instanceExtensions[i].extensionName);
            }
            log::info("\n");
        }

        res = NVSDK_NGX_VULKAN_GetFeatureRequirements(vkinstance, vkphysicaldevice, dis, &req);
    }
#endif
    if (res != NVSDK_NGX_Result_Success && res != NVSDK_NGX_Result_FAIL_NotImplemented)
    {
        log::error("GetFeatureRequirements error: 0x%08x info: %ls\n", res, GetNGXResultAsString(res));
        return false;
    }
    
    log::info("GetFeatureRequirements returned 0x%08x: Min GPU Arch: 0x%08x MinOS: %s\n", req.FeatureSupported, req.MinHWArchitecture, req.MinOSVersion);
    
    return true;
}

#if USE_VK
NVSDK_NGX_Resource_VK TextureToResourceVK(nvrhi::ITexture * tex, nvrhi::TextureSubresourceSet subresources)
{
    nvrhi::TextureDesc desc = tex->GetDesc();
    NVSDK_NGX_Resource_VK resourceVK = {};


    bool isDepthTexture = (desc.format == nvrhi::Format::D16 ||
                           desc.format == nvrhi::Format::D24S8 ||
                           desc.format == nvrhi::Format::D32 ||
                           desc.format == nvrhi::Format::D32S8);

    // DLSS reads the depth texture as a shader resource so it has to be a read only view
    VkImageView imageView = tex->getNativeView(nvrhi::ObjectTypes::VK_ImageView, nvrhi::Format::UNKNOWN, subresources, isDepthTexture);
    VkFormat format = (VkFormat)nvrhi::vulkan::convertFormat(desc.format);
    VkImage image = tex->getNativeObject(nvrhi::ObjectTypes::VK_Image);

    // We need to pass the correct aspect mask to the SDK
    uint32_t aspectMask = isDepthTexture ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageSubresourceRange subresourceRange = { aspectMask, subresources.baseMipLevel, subresources.numMipLevels, subresources.baseArraySlice, subresources.numArraySlices };

    return NVSDK_NGX_Create_ImageView_Resource_VK(imageView, image, subresourceRange, format, desc.width, desc.height, desc.isUAV);
}
#endif

void NGXWrapper::EvaluateSuperSampling(nvrhi::ICommandList* commandList,
            nvrhi::ITexture* unresolvedColor,
            nvrhi::ITexture* resolvedColor,
            nvrhi::ITexture* motionVectors,
            nvrhi::ITexture* depth,
            nvrhi::ITexture* exposure,
            const ICompositeView& compositeView,
            bool bResetAccumulation,
            bool bUseNgxSdkExtApi,
            donut::math::float2 jitterOffset,
            donut::math::float2 mVScale)
{
    if (!IsDLSSAvailable())
    {
        log::error("DLSS is not available - could not evaluate SuperSampling");
        return;
    }

    assert(compositeView.GetNumChildViews(ViewType::PLANAR) == 1); // DLSS is simple?

    commandList->beginMarker("DLSS::DLSS");

    for (uint viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

        nvrhi::ViewportState viewportState = view->GetViewportState();

        for (uint viewportIndex = 0; viewportIndex < viewportState.viewports.size(); viewportIndex++)
        {
            int Reset = bResetAccumulation ? 1 : 0;

            NVSDK_NGX_Coordinates renderingOffset = {(unsigned int)viewportState.viewports[viewportIndex].minX,
                                                     (unsigned int)viewportState.viewports[viewportIndex].minY};
            NVSDK_NGX_Dimensions  renderingSize   = {(unsigned int)(viewportState.viewports[viewportIndex].maxX - viewportState.viewports[viewportIndex].minX),
                                                     (unsigned int)(viewportState.viewports[viewportIndex].maxY - viewportState.viewports[viewportIndex].minY)};

#if USE_DX11
            if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11)
            {
                NVSDK_NGX_Result Result;

                ID3D11DeviceContext* d3dcontext = m_Device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
                ID3D11Resource* unresolvedColorResource = unresolvedColor->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);
                ID3D11Resource* motionVectorsResource = motionVectors->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);
                ID3D11Resource* resolvedColorResource = resolvedColor->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);
                ID3D11Resource* depthResource = depth->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);
                ID3D11Resource* exposureResource = exposure->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);

                NVSDK_NGX_D3D11_DLSS_Eval_Params D3D11DlssEvalParams;

                memset(&D3D11DlssEvalParams, 0, sizeof(D3D11DlssEvalParams));

                D3D11DlssEvalParams.Feature.pInColor = unresolvedColorResource;
                D3D11DlssEvalParams.Feature.pInOutput = resolvedColorResource;
                D3D11DlssEvalParams.pInDepth = depthResource;
                D3D11DlssEvalParams.pInMotionVectors = motionVectorsResource;
                D3D11DlssEvalParams.pInExposureTexture = exposureResource;
                D3D11DlssEvalParams.InJitterOffsetX = jitterOffset.x;
                D3D11DlssEvalParams.InJitterOffsetY = jitterOffset.y;
                D3D11DlssEvalParams.InReset = Reset;
                D3D11DlssEvalParams.InMVScaleX = mVScale.x;
                D3D11DlssEvalParams.InMVScaleY = mVScale.y;
                D3D11DlssEvalParams.InColorSubrectBase        = renderingOffset;
                D3D11DlssEvalParams.InDepthSubrectBase        = renderingOffset;
                D3D11DlssEvalParams.InTranslucencySubrectBase = renderingOffset;
                D3D11DlssEvalParams.InMVSubrectBase           = renderingOffset;
                D3D11DlssEvalParams.InRenderSubrectDimensions = renderingSize;

                Result = NGX_D3D11_EVALUATE_DLSS_EXT(d3dcontext, m_dlssFeature, m_ngxParameters, &D3D11DlssEvalParams);

                if (NVSDK_NGX_FAILED(Result) )
                {
                    log::error("Failed to NVSDK_NGX_D3D11_EvaluateFeature for DLSS, code = 0x%08x, info: %ls", Result, GetNGXResultAsString(Result));
                }
            }
#endif
#if USE_DX12
            if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
            {
                commandList->endTrackingTextureState(unresolvedColor, nvrhi::AllSubresources, nvrhi::ResourceStates::SHADER_RESOURCE);
                commandList->endTrackingTextureState(resolvedColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UNORDERED_ACCESS);
                commandList->endTrackingTextureState(motionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::SHADER_RESOURCE);

                NVSDK_NGX_Result Result;
                
                ID3D12GraphicsCommandList* d3dcommandList = commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);
                ID3D12Resource* unresolvedColorResource = unresolvedColor->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
                ID3D12Resource* motionVectorsResource = motionVectors->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
                ID3D12Resource* resolvedColorResource = resolvedColor->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
                ID3D12Resource* depthResource = depth->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
                ID3D12Resource* exposureResource = exposure->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);

                NVSDK_NGX_D3D12_DLSS_Eval_Params D3D12DlssEvalParams;

                memset(&D3D12DlssEvalParams, 0, sizeof(D3D12DlssEvalParams));

                D3D12DlssEvalParams.Feature.pInColor = unresolvedColorResource;
                D3D12DlssEvalParams.Feature.pInOutput = resolvedColorResource;
                D3D12DlssEvalParams.pInDepth = depthResource;
                D3D12DlssEvalParams.pInMotionVectors = motionVectorsResource;
                D3D12DlssEvalParams.pInExposureTexture = exposureResource;
                D3D12DlssEvalParams.InJitterOffsetX = jitterOffset.x;
                D3D12DlssEvalParams.InJitterOffsetY = jitterOffset.y;
                D3D12DlssEvalParams.InReset = Reset;
                D3D12DlssEvalParams.InMVScaleX = mVScale.x;
                D3D12DlssEvalParams.InMVScaleY = mVScale.y;
                D3D12DlssEvalParams.InColorSubrectBase        = renderingOffset;
                D3D12DlssEvalParams.InDepthSubrectBase        = renderingOffset;
                D3D12DlssEvalParams.InTranslucencySubrectBase = renderingOffset;
                D3D12DlssEvalParams.InMVSubrectBase           = renderingOffset;
                D3D12DlssEvalParams.InRenderSubrectDimensions = renderingSize;

                Result = NGX_D3D12_EVALUATE_DLSS_EXT(d3dcommandList, m_dlssFeature, m_ngxParameters, &D3D12DlssEvalParams);

                if (NVSDK_NGX_FAILED(Result))
                {
                    log::error("Failed to NVSDK_NGX_D3D12_EvaluateFeature for DLSS, code = 0x%08x, info: %ls", Result, GetNGXResultAsString(Result));
                }

                commandList->clearState();
            }
#endif
#if USE_VK
            if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
            {
                commandList->endTrackingTextureState(unresolvedColor, nvrhi::AllSubresources, nvrhi::ResourceStates::SHADER_RESOURCE);
                commandList->endTrackingTextureState(resolvedColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UNORDERED_ACCESS);
                commandList->endTrackingTextureState(motionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::SHADER_RESOURCE);
                commandList->endTrackingTextureState(depth, nvrhi::AllSubresources, nvrhi::ResourceStates::SHADER_RESOURCE);

                VkCommandBuffer vkcommandbuffer = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
                nvrhi::TextureSubresourceSet subresources = view->GetSubresources();

                NVSDK_NGX_Resource_VK unresolvedColorResource = TextureToResourceVK(unresolvedColor, subresources);
                NVSDK_NGX_Resource_VK resolvedColorResource = TextureToResourceVK(resolvedColor, subresources);
                NVSDK_NGX_Resource_VK motionVectorsResource = TextureToResourceVK(motionVectors, subresources);
                NVSDK_NGX_Resource_VK depthResource = TextureToResourceVK(depth, subresources);
                NVSDK_NGX_Resource_VK exposureResource = TextureToResourceVK(exposure, subresources);

                NVSDK_NGX_Result Result;

                NVSDK_NGX_VK_DLSS_Eval_Params VkDlssEvalParams;

                memset(&VkDlssEvalParams, 0, sizeof(VkDlssEvalParams));

                VkDlssEvalParams.Feature.pInColor = &unresolvedColorResource;
                VkDlssEvalParams.Feature.pInOutput = &resolvedColorResource;
                VkDlssEvalParams.pInDepth = &depthResource;
                VkDlssEvalParams.pInMotionVectors = &motionVectorsResource;
                VkDlssEvalParams.pInExposureTexture = &exposureResource;
                VkDlssEvalParams.InJitterOffsetX = jitterOffset.x;
                VkDlssEvalParams.InJitterOffsetY = jitterOffset.y;
                VkDlssEvalParams.InReset = Reset;
                VkDlssEvalParams.InMVScaleX = mVScale.x;
                VkDlssEvalParams.InMVScaleY = mVScale.y;
                VkDlssEvalParams.InColorSubrectBase        = renderingOffset;
                VkDlssEvalParams.InDepthSubrectBase        = renderingOffset;
                VkDlssEvalParams.InTranslucencySubrectBase = renderingOffset;
                VkDlssEvalParams.InMVSubrectBase           = renderingOffset;
                VkDlssEvalParams.InRenderSubrectDimensions = renderingSize;

                Result = NGX_VULKAN_EVALUATE_DLSS_EXT(vkcommandbuffer, m_dlssFeature, m_ngxParameters, &VkDlssEvalParams);

                if (NVSDK_NGX_FAILED(Result))
                {
                    log::error("Failed to NVSDK_NGX_VULKAN_EvaluateFeature for DLSS, code = 0x%08x, info: %ls", Result, GetNGXResultAsString(Result));
                }

                commandList->clearState();
            }
#endif
        }
    }

    commandList->endMarker();
}

