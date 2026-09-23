/*
 * SPDX-FileCopyrightText: Copyright (c) 2018-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */
#pragma once

#if USE_DX11 || USE_DX12
#include <DXGI.h>
#endif

#if USE_DX11
#include <d3d11.h>
#endif

#if USE_DX12
#include <d3d12.h>
#endif

#if USE_VK
#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>
#include <nvrhi/vulkan.h>
#endif

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif // _WIN32
#include <GLFW/glfw3native.h>
#include <nvrhi/nvrhi.h>

#include <list>
#include <array>

#include <donut/core/log.h>

namespace donut::app
{
    struct DefaultMessageCallback : public nvrhi::IMessageCallback
    {
        static DefaultMessageCallback& GetInstance();

        virtual void message(nvrhi::MessageSeverity severity, const char* messageText, const char* file = nullptr, int line = 0) override;
    };

    struct DeviceCreationParameters
    {
        bool startMaximized = false;
        bool startFullscreen = false;
        bool allowModeSwitch = true;
        int windowPosX = -1;            // -1 means use default placement
        int windowPosY = -1;
        uint32_t backBufferWidth = 1280;
        uint32_t backBufferHeight = 720;
        uint32_t refreshRate = 0;
        uint32_t swapChainBufferCount = 3;
        nvrhi::Format swapChainFormat = nvrhi::Format::SRGBA8_UNORM;
        uint32_t swapChainSampleCount = 1;
        uint32_t swapChainSampleQuality = 0;
        bool enableDebugRuntime = false;
        bool enableNvrhiValidationLayer = false;
        bool vsyncEnabled = false;

#if USE_DX11 || USE_DX12
        // Adapter to create the device on. Setting this to non-null overrides adapterNameSubstring.
        // If device creation fails on the specified adapter, it will *not* try any other adapters.
        IDXGIAdapter* adapter = nullptr;
#endif

        // For use in the case of multiple adapters; only effective if 'adapter' is null. If this is non-null, device creation will try to match
        // the given string against an adapter name.  If the specified string exists as a sub-string of the
        // adapter name, the device and window will be created on that adapter.  Case sensitive.
        std::wstring adapterNameSubstring = L"";

        // set to true to enable DPI scale factors to be computed per monitor
        // this will keep the on-screen window size in pixels constant
        //
        // if set to false, the DPI scale factors will be constant but the system
        // may scale the contents of the window based on DPI
        //
        // note that the backbuffer size is never updated automatically; if the app
        // wishes to scale up rendering based on DPI, then it must set this to true
        // and respond to DPI scale factor changes by resizing the backbuffer explicitly
        bool enablePerMonitorDPI = false;

#if USE_DX11 || USE_DX12
        DXGI_USAGE swapChainUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
#endif
    };

    class IRenderPass;

    class DeviceManager
    {
    public:
        static DeviceManager* Create(nvrhi::GraphicsAPI api);

        bool CreateWindowDeviceAndSwapChain(const DeviceCreationParameters& params, const char *windowTitle);

        void AddRenderPassToFront(IRenderPass *pController);
        void AddRenderPassToBack(IRenderPass *pController);
        void RemoveRenderPass(IRenderPass *pController);

        void RunMessageLoop();

        // returns the size of the window in screen coordinates
        void GetWindowDimensions(int& width, int& height);
        // returns the screen coordinate to pixel coordinate scale factor
        void GetDPIScaleInfo(float& x, float& y)
        {
            x = m_DPIScaleFactorX;
            y = m_DPIScaleFactorY;
        }

    protected:
        bool m_windowVisible = false;

        DeviceCreationParameters m_DeviceParams;
        GLFWwindow *m_Window = nullptr;
        // set to true if running on NV GPU
        bool m_IsNvidia = false;
        std::list<IRenderPass *> m_vRenderPasses;
        // timestamp in seconds for the previous frame
        double m_PreviousFrameTimestamp = 0.0;
        // current DPI scale info (updated when window moves)
        float m_DPIScaleFactorX = 1.f;
        float m_DPIScaleFactorY = 1.f;

        double m_AverageFrameTime = 0.0;
        double m_AverageTimeUpdateInterval = 0.5;
        double m_FrameTimeSum = 0.0;
        int m_NumberOfAccumulatedFrames = 0;

        std::vector<nvrhi::FramebufferHandle> m_SwapChainFramebuffers;

        DeviceManager() = default;

        void UpdateWindowSize();

        void BackBufferResizing();
        void BackBufferResized();

        void Animate(double elapsedTime);
        void Render();
        void UpdateAverageFrameTime(double elapsedTime);

        // device-specific methods
        virtual bool CreateDeviceAndSwapChain() = 0;
        virtual void DestroyDeviceAndSwapChain() = 0;
        virtual void ResizeSwapChain() = 0;
        virtual void BeginFrame() = 0;
        virtual void Present() = 0;

    public:
        virtual nvrhi::IDevice *GetDevice() = 0;
        virtual const char *GetRendererString() = 0;
        virtual nvrhi::GraphicsAPI GetGraphicsAPI() const = 0;

        const DeviceCreationParameters& GetDeviceParams();
        double GetAverageFrameTimeSeconds() { return m_AverageFrameTime; }
        void SetFrameTimeUpdateInterval(double seconds) { m_AverageTimeUpdateInterval = seconds; }
        bool IsVsyncEnabled() { return m_DeviceParams.vsyncEnabled; }
        virtual void SetVsyncEnabled(bool enabled) { m_DeviceParams.vsyncEnabled = enabled; }
        virtual void ReportLiveObjects() {}

        // these are public in order to be called from the GLFW callback functions
        void WindowCloseCallback() { };
        void WindowIconifyCallback(int iconified) { }
        void WindowFocusCallback(int focused) { }
        void WindowRefreshCallback() { }
        void WindowPosCallback(int xpos, int ypos);

        void KeyboardUpdate(int key, int scancode, int action, int mods);
        void KeyboardCharInput(unsigned int unicode, int mods);
        void MousePosUpdate(double xpos, double ypos);
        void MouseButtonUpdate(int button, int action, int mods);
        void MouseScrollUpdate(double xoffset, double yoffset);
        void DropFileUpdate(int path_count, const char* paths[]);

        GLFWwindow* GetWindow() const { return m_Window; }

        virtual nvrhi::ITexture* GetCurrentBackBuffer() = 0;
        virtual nvrhi::ITexture* GetBackBuffer(uint32_t index) = 0;
        virtual uint32_t GetCurrentBackBufferIndex() = 0;
        virtual uint32_t GetBackBufferCount() = 0;
        nvrhi::IFramebuffer* GetCurrentFramebuffer();
        nvrhi::IFramebuffer* GetFramebuffer(uint32_t index);

        void Shutdown();
        virtual ~DeviceManager() { }

    private:
        static DeviceManager* CreateD3D11();
        static DeviceManager* CreateD3D12();
        static DeviceManager* CreateVK();
    };

    class IRenderPass
    {
    private:
        DeviceManager* m_DeviceManager;

    public:
        IRenderPass(DeviceManager* deviceManager)
            : m_DeviceManager(deviceManager)
        { }

        virtual void Render(nvrhi::IFramebuffer* framebuffer) { }
        virtual void Animate(float fElapsedTimeSeconds) { }
        virtual void BackBufferResizing() { }
        virtual void BackBufferResized(const uint32_t width, const uint32_t height, const uint32_t sampleCount) { }

        // all of these pass in GLFW constants as arguments
        // see http://www.glfw.org/docs/latest/input.html
        // return value is true if the event was consumed by this render pass, false if it should be passed on
        virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) { return false; }
        virtual bool KeyboardCharInput(unsigned int unicode, int mods) { return false; }
        virtual bool MousePosUpdate(double xpos, double ypos) { return false; }
        virtual bool MouseScrollUpdate(double xoffset, double yoffset) { return false; }
        virtual bool MouseButtonUpdate(int button, int action, int mods) { return false; }
        virtual bool DropFileUpdate(int path_count, const char* paths[]) { return false; }
        virtual bool JoystickButtonUpdate(int button, bool pressed) { return false; }
        virtual bool JoystickAxisUpdate(int axis, float value) { return false; }

        inline DeviceManager* GetDeviceManager() { return m_DeviceManager; }
        inline nvrhi::IDevice* GetDevice() { return m_DeviceManager->GetDevice(); }
    };

#if USE_DX11 || USE_DX12
    #include <nvapi.h>
    
    using nvrhi::RefCountPtr;
    // Find an adapter whose name contains the given string.
    static RefCountPtr<IDXGIAdapter> FindAdapter(const std::wstring& targetName)
    {
        RefCountPtr<IDXGIAdapter> targetAdapter;
        RefCountPtr<IDXGIFactory1> DXGIFactory;
        HRESULT hres = CreateDXGIFactory1(IID_PPV_ARGS(&DXGIFactory));
        if (hres != S_OK)
        {
            donut::log::error("ERROR in CreateDXGIFactory.\n"
                "For more info, get log from debug D3D runtime: (1) Install DX SDK, and enable Debug D3D from DX Control Panel Utility. (2) Install and start DbgView. (3) Try running the program again.\n");
            return targetAdapter;
        }

        unsigned int adapterNo = 0;
        while (SUCCEEDED(hres))
        {
            RefCountPtr<IDXGIAdapter> pAdapter;
            hres = DXGIFactory->EnumAdapters(adapterNo, &pAdapter);

            if (SUCCEEDED(hres))
            {
                DXGI_ADAPTER_DESC aDesc;
                pAdapter->GetDesc(&aDesc);
                std::wstring aName = aDesc.Description;
                donut::log::info("Adapter[%d] %ls VenId=%d DevId=%d SubSysId=%d Rev=%d 0x%x memory:{Vid=%d Sys=%d Shared=%d}", adapterNo, aName.c_str(), aDesc.VendorId, aDesc.DeviceId, aDesc.SubSysId, aDesc.Revision, aDesc.AdapterLuid, aDesc.DedicatedVideoMemory, aDesc.DedicatedSystemMemory, aDesc.SharedSystemMemory);
            }

            adapterNo++;
        }
        std::wstring desiredName = targetName;
        adapterNo = 0;
        hres = S_OK;
        while (SUCCEEDED(hres))
        {
            RefCountPtr<IDXGIAdapter> pAdapter;
            hres = DXGIFactory->EnumAdapters(adapterNo, &pAdapter);

            if (SUCCEEDED(hres))
            {
                DXGI_ADAPTER_DESC aDesc;
                pAdapter->GetDesc(&aDesc);
                auto InAdapterLUID = aDesc.AdapterLuid;

                NvLogicalGpuHandle logicalGpuHandles[NVAPI_MAX_LOGICAL_GPUS];
                NvU32 logicalGpuCount = 0;
                auto ret = NvAPI_EnumLogicalGPUs(logicalGpuHandles, &logicalGpuCount);
                NvPhysicalGpuHandle physicalGpuHandles[NVAPI_MAX_PHYSICAL_GPUS];
                NvU32 physicalGpuCount = 0;
                ret = NvAPI_EnumPhysicalGPUs(physicalGpuHandles, &physicalGpuCount);
                bool adapterFound = false;
                if (ret == NVAPI_OK)
                {
                    for (NvU32 i = 0; i < logicalGpuCount; i++)
                    {
                        LUID currAdapterLUID = { 0 };
                        NV_LOGICAL_GPU_DATA_V1 currLogicalGpuData = { 0 };
                        currLogicalGpuData.version = NV_LOGICAL_GPU_DATA_VER1;
                        currLogicalGpuData.pOSAdapterId = &currAdapterLUID;
                        ret = NvAPI_GPU_GetLogicalGpuInfo(logicalGpuHandles[i], &currLogicalGpuData);
                        if (ret != NVAPI_OK)
                        {
                            continue;
                        }
                        if (currAdapterLUID.HighPart == InAdapterLUID.HighPart && currAdapterLUID.LowPart == InAdapterLUID.LowPart)
                        {
                            donut::log::info("Found matching adapter with NVAPI physical GPU handle: 0x%0x and LUID: { 0x%x, 0x%x }", logicalGpuHandles[i], currAdapterLUID.HighPart, currAdapterLUID.LowPart);
                            adapterFound = true;
                            break;
                        }
                    }
                }
                if (!adapterFound)
                {
                    // If the first adapter doesn't have an LUID assume it's some virtual device that seems to be created for Remote Desktop sessions.
                    // They seem to have the same name as the "real" adapter, so set it as the target/desired adapter as if the app/user had 
                    // explicitly requested it.
                    if (desiredName.empty())
                    {
                        donut::log::info("Requesting adapter without corresponding NvApi LUID: 0x%x %ls", aDesc.AdapterLuid, aDesc.Description);
                        desiredName = aDesc.Description;
                    }
                    else
                    {
                        donut::log::info("Skipping adapter without corresponding NvApi LUID: 0x%x %ls", aDesc.AdapterLuid, aDesc.Description);
                    }
                    adapterNo++;
                    continue;
                }

                // If no name is specified, return the first adapater.  This is the same behaviour as the
                // default specified for D3D11CreateDevice when no adapter is specified.
                if (desiredName.length() == 0)
                {
                    targetAdapter = pAdapter;
                    break;
                }

                std::wstring aName = aDesc.Description;

                if (aName.find(desiredName) != std::string::npos)
                {
                    targetAdapter = pAdapter;
                    break;
                }
            }

            adapterNo++;
        }

        return targetAdapter;
    }
#endif
}
