#pragma once

#include "d3d11_texture.h"
#include "../dxvk/rtx_render/rtx_dlfg.h"

#include "../dxvk/hud/dxvk_hud.h"

#include "../dxvk/dxvk_swapchain_blitter.h"

#include "../util/sync/sync_signal.h"

namespace dxvk {
  
  class D3D11Device;
  class D3D11DXGIDevice;

  class D3D11SwapChain : public ComObject<IDXGIVkSwapChain> {
    constexpr static uint32_t DefaultFrameLatency = 1;
  public:

    D3D11SwapChain(
            D3D11DXGIDevice*          pContainer,
            D3D11Device*              pDevice,
            HWND                      hWnd,
      const DXGI_SWAP_CHAIN_DESC1*    pDesc);
    
    ~D3D11SwapChain();

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                    riid,
            void**                    ppvObject);

    HRESULT STDMETHODCALLTYPE GetDesc(
            DXGI_SWAP_CHAIN_DESC1*    pDesc);

    HRESULT STDMETHODCALLTYPE GetAdapter(
            REFIID                    riid,
            void**                    ppvObject);
    
    HRESULT STDMETHODCALLTYPE GetDevice(
            REFIID                    riid,
            void**                    ppDevice);
    
    HRESULT STDMETHODCALLTYPE GetImage(
            UINT                      BufferId,
            REFIID                    riid,
            void**                    ppBuffer);

    UINT STDMETHODCALLTYPE GetImageIndex();

    UINT STDMETHODCALLTYPE GetFrameLatency();

    HANDLE STDMETHODCALLTYPE GetFrameLatencyEvent();

    HRESULT STDMETHODCALLTYPE ChangeProperties(
      const DXGI_SWAP_CHAIN_DESC1*    pDesc);

    HRESULT STDMETHODCALLTYPE SetPresentRegion(
      const RECT*                     pRegion);

    HRESULT STDMETHODCALLTYPE SetGammaControl(
            UINT                      NumControlPoints,
      const DXGI_RGB*                 pControlPoints);

    HRESULT STDMETHODCALLTYPE SetFrameLatency(
            UINT                      MaxLatency);

    HRESULT STDMETHODCALLTYPE Present(
            UINT                      SyncInterval,
            UINT                      PresentFlags,
      const DXGI_PRESENT_PARAMETERS*  pPresentParameters);

    void STDMETHODCALLTYPE NotifyModeChange(
            BOOL                      Windowed,
      const DXGI_MODE_DESC*           pDisplayMode);

  private:

    friend LRESULT CALLBACK D3D11SwapChainWndProc(HWND, UINT, WPARAM, LPARAM);

    enum BindingIds : uint32_t {
      Image = 0,
      Gamma = 1,
    };

    Com<D3D11DXGIDevice, false> m_dxgiDevice;
    
    D3D11Device*              m_parent;
    HWND                      m_window;

    DXGI_SWAP_CHAIN_DESC1     m_desc;

    Rc<DxvkDevice>            m_device;
    Rc<DxvkContext>           m_context;

    Rc<vk::Presenter>         m_presenter;
    Rc<DxvkDLFGPresenter>     m_dlfgPresenter;
    bool                     m_reflexSimulationStarted = false;

    Rc<DxvkImage>             m_swapImage;
    Rc<DxvkImageView>         m_swapImageView;
    Rc<DxvkImageView>         m_swapImageRtView;
    Rc<DxvkSwapchainBlitter>  m_blitter;

    Rc<hud::Hud>              m_hud;

    D3D11Texture2D*           m_backBuffer = nullptr;
    DxvkSubmitStatus          m_presentStatus;

    std::vector<Rc<DxvkImageView>> m_imageViews;

    uint64_t                m_frameId      = DXGI_MAX_SWAP_CHAIN_BUFFERS;
    uint32_t                m_frameLatency = DefaultFrameLatency;
    uint32_t                m_frameLatencyCap = 0;
    HANDLE                  m_frameLatencyEvent = nullptr;
    Rc<sync::CallbackFence> m_frameLatencySignal;

    bool                    m_dirty = true;
    bool                    m_vsync = true;
    uint32_t                m_lastPresentDrawCount = 0;
    uint32_t                m_scenePresentStreak = 0;
    bool                    m_remixOverlayArmed = false;
    WNDPROC                 m_prevWndProc = nullptr;

    double                  m_displayRefreshRate = 0.0;

    void InstallWndProcHook();

    HRESULT PresentImage(UINT SyncInterval);

    void SubmitPresent(
            D3D11ImmediateContext*  pContext,
      const vk::PresenterSync&      Sync,
            uint32_t                FrameId,
            bool                    IsPrimary,
            uint32_t                ImageIndex);

    VkResult SynchronizePresent();

    void RecreateSwapChain(
            BOOL                      Vsync);

    void CreateFrameLatencyEvent();

    void CreatePresenter(bool enableDlfg = false);

    void CreateRenderTargetViews();

    void CreateBackBuffer();

    void CreateBlitter();

    void CreateHud();

    void DestroyFrameLatencyEvent();

    void SyncFrameLatency();

    uint32_t GetActualFrameLatency();
    
    uint32_t PickFormats(
            DXGI_FORMAT               Format,
            VkSurfaceFormatKHR*       pDstFormats);
    
    uint32_t PickPresentModes(
            BOOL                      Vsync,
            VkPresentModeKHR*         pDstModes);
    
    uint32_t PickImageCount(
            UINT                      Preferred);
    
    VkFullScreenExclusiveEXT PickFullscreenMode();

    std::string GetApiName() const;

  };

}
