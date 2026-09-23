#include "../util/util_kenshi_telemetry.h"
#include "d3d11_context_imm.h"
#include "d3d11_device.h"
#include "d3d11_swapchain.h"

#include "../dxvk/imgui/dxvk_imgui.h"
#include "../dxvk/rtx_render/rtx_option_manager.h"
#include "../dxvk/rtx_render/rtx_options.h"
#include "../util/log/log.h"
#include "../util/util_kenshi_terrain_audit.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <unordered_map>

namespace dxvk {


  static UINT getRemixToggleUiMessage() {
    static const UINT s_msg = RegisterWindowMessageA("DXVK_REMIX_TOGGLE_UI");
    return s_msg;
  }

  // Per-HWND map so the static WndProc callback can find its swapchain.
  static std::mutex                                    g_d3d11WndProcMutex;
  static std::unordered_map<HWND, D3D11SwapChain*>    g_d3d11WndProcMap;
  static std::atomic_bool                              g_remixScreenshotRequested { false };
  static std::atomic_bool                              g_printScreenPolledDown { false };
  static std::atomic<uint64_t>                         g_lastPrintScreenMessageMs { 0u };

  // Primary swap chain: only this one renders the Remix UI overlay and drives
  // RT frame boundaries (EndFrame / OnPresent).  Multi-device games (Unity,
  // UE4) and emulators create multiple swap chains; without this guard the
  // overlay would render twice and RT state would be confused.  The first
  // swap chain to present claims primary.  If it's destroyed, the next one
  // takes over.
  static std::mutex          g_primaryMutex;
  static D3D11SwapChain*     g_primarySwapChain = nullptr;

  // Hysteresis state for primary election: a candidate must be "clearly better"
  // for kPrimaryHysteresisFrames consecutive frames before it can steal primary.
  // Prevents single-frame thrashing between similar swapchains in Unity.
  // Raised from 3 to 5: at 60fps this requires ~83ms of sustained advantage
  // (up from ~50ms), which prevents typical UI bursts (4–6 frames) from
  // causing election thrashing between the main game and overlay swapchains.
  static constexpr uint32_t  kPrimaryHysteresisFrames = 5;
  static constexpr uint32_t  kRemixOverlayArmSceneFrames = 2;
  static D3D11SwapChain*     g_primaryChallenger = nullptr;
  static uint32_t            g_primaryChallengerFrames = 0;

  static bool isVirtualKeyPressedNow(UINT vk) {
    return (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
  }

  static bool matchesMenuHotkeyMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN)
      return false;

    if ((lParam & 0x40000000u) != 0)
      return false;

    const VirtualKeys& hotkey = RtxOptions::remixMenuKeyBinds();
    if (hotkey.empty())
      return false;

    bool hasNonModifier = false;
    bool matchedTriggerKey = false;

    for (const auto& vk : hotkey) {
      switch (vk.val) {
      case VK_SHIFT:
        if (!(wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT || isVirtualKeyPressedNow(VK_SHIFT)))
          return false;
        break;
      case VK_CONTROL:
        if (!(wParam == VK_CONTROL || wParam == VK_LCONTROL || wParam == VK_RCONTROL || isVirtualKeyPressedNow(VK_CONTROL)))
          return false;
        break;
      case VK_MENU:
        if (!(wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU || isVirtualKeyPressedNow(VK_MENU)))
          return false;
        break;
      default:
        hasNonModifier = true;
        matchedTriggerKey = matchedTriggerKey || (static_cast<UINT>(vk.val) == static_cast<UINT>(wParam));
        break;
      }
    }

    return hasNonModifier && matchedTriggerKey;
  }

  static bool isPrintScreenMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP
      || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
     && wParam == VK_SNAPSHOT)
      return true;

    if (msg != WM_INPUT)
      return false;

    RAWINPUT input = {};
    UINT inputSize = sizeof(input);
    const UINT read = GetRawInputData(
      reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
      &input, &inputSize, sizeof(RAWINPUTHEADER));
    return read != UINT(-1)
        && read >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD)
        && input.header.dwType == RIM_TYPEKEYBOARD
        && input.data.keyboard.VKey == VK_SNAPSHOT;
  }

  static VkExtent2D getWindowClientExtent(HWND window) {
    VkExtent2D extent = { 0u, 0u };
    RECT clientRect = {};

    if (IsWindow(window) && GetClientRect(window, &clientRect)) {
      const LONG width = clientRect.right - clientRect.left;
      const LONG height = clientRect.bottom - clientRect.top;

      if (width > 0 && height > 0) {
        extent.width = static_cast<uint32_t>(width);
        extent.height = static_cast<uint32_t>(height);
      }
    }

    return extent;
  }

  static VkExtent2D getImageExtent2D(const Rc<DxvkImage>& image) {
    if (image == nullptr)
      return { 0u, 0u };

    const VkExtent3D extent = image->info().extent;
    return { extent.width, extent.height };
  }

  static bool isWindowPresentOccluded(HWND window, VkExtent2D* clientExtent = nullptr) {
    const VkExtent2D extent = getWindowClientExtent(window);

    if (clientExtent != nullptr)
      *clientExtent = extent;

    return !IsWindow(window)
        || !IsWindowVisible(window)
        || IsIconic(window)
        || extent.width == 0u
        || extent.height == 0u;
  }

  static void logOccludedPresent(HWND window, const VkExtent2D& clientExtent, const char* stage) {
    static std::atomic<uint32_t> s_logCount { 0u };

    if (s_logCount.fetch_add(1u, std::memory_order_relaxed) >= 8u)
      return;

    KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] Present occluded; skipping WSI/RTX work: stage=",
      stage,
      " HWND=", (uintptr_t) window,
      " visible=", (IsWindow(window) && IsWindowVisible(window)) ? 1 : 0,
      " iconic=", (IsWindow(window) && IsIconic(window)) ? 1 : 0,
      " client=", clientExtent.width, "x", clientExtent.height));
  }

  static void logDeferredRemixUiUntilScene(HWND window, const char* source) {
    static std::atomic<uint32_t> s_logCount { 0u };

    if (s_logCount.fetch_add(1u, std::memory_order_relaxed) >= 12u)
      return;

    const VkExtent2D clientExtent = getWindowClientExtent(window);
    KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] Deferring Remix UI until a real scene is observed: source=",
      source,
      " HWND=", (uintptr_t) window,
      " visible=", (IsWindow(window) && IsWindowVisible(window)) ? 1 : 0,
      " iconic=", (IsWindow(window) && IsIconic(window)) ? 1 : 0,
      " client=", clientExtent.width, "x", clientExtent.height));
  }

  static LRESULT CALLBACK D3D11SwapChainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Reentrancy guard: driver-injected WndProcs (e.g. Intel OPENGL32.dll)
    // can re-dispatch messages to the same HWND, causing infinite recursion.
    // On re-entry, skip our processing but still forward through the chain
    // so the game's WndProc receives messages. Hard-cap depth to prevent
    // stack overflow from pathological chains.
    thread_local int recursionDepth = 0;
    ++recursionDepth;

    if (recursionDepth > 8) {
      --recursionDepth;
      return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    if (recursionDepth > 1) {
      WNDPROC prev = nullptr;
      {
        std::lock_guard<std::mutex> lk(g_d3d11WndProcMutex);
        auto it = g_d3d11WndProcMap.find(hWnd);
        if (it != g_d3d11WndProcMap.end())
          prev = it->second->m_prevWndProc;
      }
      LRESULT result = prev ? CallWindowProcW(prev, hWnd, msg, wParam, lParam)
                            : DefWindowProcW(hWnd, msg, wParam, lParam);
      --recursionDepth;
      return result;
    }

    D3D11SwapChain* sc  = nullptr;
    WNDPROC         prev = nullptr;
    {
      std::lock_guard<std::mutex> lk(g_d3d11WndProcMutex);
      auto it = g_d3d11WndProcMap.find(hWnd);
      if (it != g_d3d11WndProcMap.end()) {
        sc   = it->second;
        prev = sc->m_prevWndProc;
      }
    }

    if (sc != nullptr && msg == getRemixToggleUiMessage()) {
      if (!sc->m_remixOverlayArmed) {
        sc->m_remixOverlayArmed = true;
        KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] Remix overlay armed by explicit UI toggle message: HWND=",
          (uintptr_t) hWnd, " this=", (uintptr_t) sc));
      }

      {
          std::lock_guard<std::mutex> pk(g_primaryMutex);
        g_primarySwapChain = sc;
        g_primaryChallenger = nullptr;
        g_primaryChallengerFrames = 0;
      }

      auto& gui = sc->m_device->getCommon()->getImgui();
      gui.switchMenu(static_cast<UIType>(wParam), lParam != 0, false);
      --recursionDepth;
      return 0;
    }

    // Print Screen belongs to Remix for an injected DX11 title. Queue the
    // exact same final-image path as the Developer Settings button and consume
    // both legacy and raw-input forms so the vanilla game cannot also save its
    // pre-Remix raster backbuffer. Some keyboards emit raw + legacy and/or only
    // a key-up; a short message debounce turns all of those into one capture.
    if (sc != nullptr && isPrintScreenMessage(msg, wParam, lParam)) {
      const uint64_t nowMs = GetTickCount64();
      const uint64_t previousMs = g_lastPrintScreenMessageMs.exchange(
        nowMs, std::memory_order_relaxed);
      if (previousMs == 0u || nowMs - previousMs >= 250u) {
        g_remixScreenshotRequested.store(true, std::memory_order_release);
        KENSHI_DIAGNOSTIC_INFO("[D3D11SwapChain] Print Screen routed to Remix screenshot capture");
      }
      --recursionDepth;
      return 0;
    }

    if (sc != nullptr && matchesMenuHotkeyMessage(msg, wParam, lParam)) {
      if (!sc->m_remixOverlayArmed) {
        sc->m_remixOverlayArmed = true;
        KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] Remix overlay armed by explicit WndProc hotkey: HWND=",
          (uintptr_t) hWnd, " this=", (uintptr_t) sc));
      }

      {
        std::lock_guard<std::mutex> pk(g_primaryMutex);
        g_primarySwapChain = sc;
        g_primaryChallenger = nullptr;
        g_primaryChallengerFrames = 0;
      }

      auto& gui = sc->m_device->getCommon()->getImgui();
      // DX11_V270_MENU_TOGGLE_DEBOUNCE: single debounced authority; consumes
      // the toggle at most once per window across ALL hotkey paths.
      gui.toggleMenuFromHotkey();
      gui.markRemixMenuHotkeyHandled();
      --recursionDepth;
      return 0;
    }

    // --- In-process input handling ---
    // The D3D11 bridge loads directly into the game process. The game's
    // WndProc and DefWindowProcW have direct side effects on the message queue:
    //
    //  - DefWindowProcW(WM_SYSKEYDOWN) activates the system menu, which
    //    generates spurious WM_KEYUP events that reset ImGui's key state.
    //    This makes hotkeys rapid-toggle (open/close every frame) instead
    //    of toggling once per press.
    //
    //  - WM_SYSCHAR triggers SC_KEYMENU in DefWindowProcW, adding more
    //    key state corruption.
    //
    // Fix: when the Remix UI is open, let ImGui process messages first and
    // block system key messages from reaching the game's WndProc / DefWindowProcW.
    // Allow Alt+F4 (close), Alt+Enter (fullscreen), and bare Alt
    // (VK_MENU) through so standard OS combos still work.

    if (sc != nullptr) {
      auto& gui = sc->m_device->getCommon()->getImgui();
      if (gui.isInit()) {
        const bool isKeyMsg   = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST)
                             || (msg >= WM_SYSKEYDOWN && msg <= WM_SYSDEADCHAR);
        const bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
        const bool isRawInputMsg = msg == WM_INPUT;
        const bool uiOpen = gui.isMenuOpen();

        // ImGui only receives and consumes game-window messages while the
        // Remix UI is open. This keeps normal game input flowing when the
        // menu is closed, even if the ImGui backend still has capture state
        // from a previous interaction.
        const bool consumedByGui = uiOpen
          ? gui.wndProcHandler(hWnd, msg, wParam, lParam)
          : false;

        if (consumedByGui) {
          --recursionDepth;
          return 0;
        }

        // When the Remix UI is open, block game input unconditionally.
        // Games like Skyrim SE read mouse/keyboard via WM_INPUT (raw input)
        // and direct WM_MOUSE* / WM_KEY* messages in their own WndProc —
        // if we let those through, the player's camera keeps spinning, the
        // weapon swings, and the game reacts to every click on a Remix
        // widget, which feels like "the game is stealing control".
        //
        // blockInputToGameInUI was previously opt-in, but the correct default
        // for any modded game is to freeze input while the overlay is up.
        // The setting is kept as a debug escape hatch: set it to False only
        // if you explicitly want the game to react while you're in the menu.
        if (uiOpen && RtxOptions::blockInputToGameInUI()) {
          if (isKeyMsg || isMouseMsg || isRawInputMsg) {
            --recursionDepth;
            return 0;
          }
        }

        // Block system key messages from the game ONLY when UI is open to prevent key state corruption.
        // Bare Alt (VK_MENU) passes through so games can detect Alt held/released.
        // Alt+F4 and Alt+Enter pass through for close and fullscreen toggle.
        if (uiOpen) {
          switch (msg) {
          case WM_SYSKEYDOWN:
          case WM_SYSKEYUP:
            if (wParam == VK_MENU || wParam == VK_F4 || wParam == VK_RETURN)
              break;  // allow these through to the game
            --recursionDepth;
            return 0;
          case WM_SYSCHAR:
            --recursionDepth;
            return 0;
          }
        }
      }
    }

    // Decrement AFTER CallWindowProcW returns — the previous WndProc
    // (e.g. opengl32) can pump messages that re-enter this callback.
    LRESULT result = prev ? CallWindowProcW(prev, hWnd, msg, wParam, lParam)
                          : DefWindowProcW(hWnd, msg, wParam, lParam);
    --recursionDepth;
    return result;
  }

  static uint16_t MapGammaControlPoint(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return uint16_t(65535.0f * x);
  }

  void D3D11SwapChain::InstallWndProcHook() {
    if (!IsWindow(m_window))
      return;

    std::lock_guard<std::mutex> lk(g_d3d11WndProcMutex);

    auto mapped = g_d3d11WndProcMap.find(m_window);
    WNDPROC preservedPrev = nullptr;
    if (mapped != g_d3d11WndProcMap.end() && mapped->second != nullptr) {
      preservedPrev = mapped->second->m_prevWndProc;
    }

    WNDPROC current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(m_window, GWLP_WNDPROC));
    if (current != D3D11SwapChainWndProc) {
      m_prevWndProc = current;
      SetWindowLongPtrW(m_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(D3D11SwapChainWndProc));
    } else if (m_prevWndProc == nullptr && preservedPrev != nullptr) {
      m_prevWndProc = preservedPrev;
    }

    g_d3d11WndProcMap[m_window] = this;
  }


  D3D11SwapChain::D3D11SwapChain(
          D3D11DXGIDevice*        pContainer,
          D3D11Device*            pDevice,
          HWND                    hWnd,
    const DXGI_SWAP_CHAIN_DESC1*  pDesc)
  : m_dxgiDevice(pContainer),
    m_parent    (pDevice),
    m_window    (hWnd),
    m_desc      (*pDesc),
    m_device    (pDevice->GetDXVKDevice()),
    m_context   (m_device->createContext()),
    m_frameLatencyCap(pDevice->GetOptions()->maxFrameLatency) {
    RtxOptions::Create();
    CreateFrameLatencyEvent();

    // Hook the game's WndProc so ImGui receives WM_KEYDOWN/WM_KEYUP for hotkeys.
    // Must use SetWindowLongPtrW (GWLP_WNDPROC) — the correct x64 API.
    InstallWndProcHook();

    KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] Created: HWND=", (uintptr_t)hWnd,
      " ", pDesc->Width, "x", pDesc->Height,
      " fmt=", pDesc->Format,
      " buffers=", pDesc->BufferCount,
      " this=", (uintptr_t)this));

    if (!pDevice->GetOptions()->deferSurfaceCreation)
      CreatePresenter();
    
    CreateBackBuffer();
    CreateBlitter();
    CreateHud();
  }


  D3D11SwapChain::~D3D11SwapChain() {
    // Restore the original WndProc and remove from map FIRST — prevents the
    // static WndProc callback from accessing this swap chain while waitForIdle
    // runs.  Messages during cleanup reach the game's own handler.
    if (m_prevWndProc != nullptr) {
      std::lock_guard<std::mutex> lk(g_d3d11WndProcMutex);
      auto it = g_d3d11WndProcMap.find(m_window);
      if (it != g_d3d11WndProcMap.end() && it->second == this) {
        WNDPROC current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(m_window, GWLP_WNDPROC));
        if (current == D3D11SwapChainWndProc) {
          SetWindowLongPtrW(m_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_prevWndProc));
        }
        g_d3d11WndProcMap.erase(it);
      }
      m_prevWndProc = nullptr;
    }

    // Use a bounded wait so the destructor cannot block the message queue
    // indefinitely when the GPU is stalled or a submission is stuck.
    constexpr auto kDestructorTimeout = std::chrono::seconds(5);

    auto waitFuture = std::async(std::launch::async, [this]() {
      m_device->waitForSubmission(&m_presentStatus);
      m_device->waitForIdle();
    });

    if (waitFuture.wait_for(kDestructorTimeout) == std::future_status::timeout) {
      Logger::warn("D3D11SwapChain::~D3D11SwapChain: "
        "waitForIdle timed out after 5 seconds — proceeding with destruction");
    }

    // Release primary swap chain ownership so the next one can take over.
    {
      std::lock_guard<std::mutex> lk(g_primaryMutex);
      if (g_primarySwapChain == this) {
        g_primarySwapChain = nullptr;
        g_primaryChallenger = nullptr;
        g_primaryChallengerFrames = 0;
      }
      // If this swapchain was the pending challenger, clear that too.
      if (g_primaryChallenger == this) {
        g_primaryChallenger = nullptr;
        g_primaryChallengerFrames = 0;
      }
    }
    
    if (m_backBuffer)
      m_backBuffer->ReleasePrivate();

    DestroyFrameLatencyEvent();
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::QueryInterface(
          REFIID                  riid,
          void**                  ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    InitReturnPtr(ppvObject);

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDXGIVkSwapChain)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("D3D11SwapChain::QueryInterface: Unknown interface query");
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDesc(
          DXGI_SWAP_CHAIN_DESC1*    pDesc) {
    *pDesc = m_desc;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetAdapter(
          REFIID                    riid,
          void**                    ppvObject) {
    return m_dxgiDevice->GetParent(riid, ppvObject);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDevice(
          REFIID                    riid,
          void**                    ppDevice) {
    return m_dxgiDevice->QueryInterface(riid, ppDevice);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetImage(
          UINT                      BufferId,
          REFIID                    riid,
          void**                    ppBuffer) {
    InitReturnPtr(ppBuffer);

    if (BufferId > 0) {
      Logger::err("D3D11: GetImage: BufferId > 0 not supported");
      return DXGI_ERROR_UNSUPPORTED;
    }

    return m_backBuffer->QueryInterface(riid, ppBuffer);
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetImageIndex() {
    return 0;
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatency() {
    return m_frameLatency;
  }


  HANDLE STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatencyEvent() {
    return m_frameLatencyEvent;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::ChangeProperties(
    const DXGI_SWAP_CHAIN_DESC1*  pDesc) {

    const bool swapchainGeometryChanged = m_desc.Format      != pDesc->Format
                                       || m_desc.Width       != pDesc->Width
                                       || m_desc.Height      != pDesc->Height
                                       || m_desc.BufferCount != pDesc->BufferCount
                                       || m_desc.Flags       != pDesc->Flags;

    m_dirty |= swapchainGeometryChanged;

    if (swapchainGeometryChanged) {
      // Live resize/fullscreen transitions can replace the backbuffer while the
      // previous frame is still in flight. Drain the old present work first so
      // the new backbuffer does not race the old swapchain images and trip a
      // device loss when emulators resize while rendering.
      m_device->waitForSubmission(&m_presentStatus);
      m_device->waitForIdle();
    }

    m_desc = *pDesc;
    CreateBackBuffer();
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetPresentRegion(
    const RECT*                     pRegion) {
    // TODO implement
    return E_NOTIMPL;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetGammaControl(
          UINT                      NumControlPoints,
    const DXGI_RGB*                 pControlPoints) {
    bool isIdentity = true;

    if (NumControlPoints > 1) {
      std::array<DxvkGammaCp, 1025> cp;

      if (NumControlPoints > cp.size())
        return E_INVALIDARG;
      
      for (uint32_t i = 0; i < NumControlPoints; i++) {
        uint16_t identity = MapGammaControlPoint(float(i) / float(NumControlPoints - 1));

        cp[i].r = MapGammaControlPoint(pControlPoints[i].Red);
        cp[i].g = MapGammaControlPoint(pControlPoints[i].Green);
        cp[i].b = MapGammaControlPoint(pControlPoints[i].Blue);
        cp[i].a = 0;

        isIdentity &= cp[i].r == identity
                   && cp[i].g == identity
                   && cp[i].b == identity;
      }

      if (!isIdentity)
        m_blitter->setGammaRamp(NumControlPoints, cp.data());
    }

    if (isIdentity)
      m_blitter->setGammaRamp(0, nullptr);

    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetFrameLatency(
          UINT                      MaxLatency) {
    if (MaxLatency == 0 || MaxLatency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
      return DXGI_ERROR_INVALID_CALL;

    if (m_frameLatencyEvent) {
      // Windows DXGI does not seem to handle the case where the new maximum
      // latency is less than the current value, and some games relying on
      // this behaviour will hang if we attempt to decrement the semaphore.
      // Thus, only increment the semaphore as necessary.
      if (MaxLatency > m_frameLatency)
        ReleaseSemaphore(m_frameLatencyEvent, MaxLatency - m_frameLatency, nullptr);
    }

    m_frameLatency = MaxLatency;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::Present(
          UINT                      SyncInterval,
          UINT                      PresentFlags,
    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {
    auto options = m_parent->GetOptions();

    if (options->syncInterval >= 0)
      SyncInterval = options->syncInterval;
    if (m_context->isDLFGEnabled())
      SyncInterval = 0;

    if (!(PresentFlags & DXGI_PRESENT_TEST)) {
      bool vsync = SyncInterval != 0;

      m_dirty |= vsync != m_vsync;
      m_vsync  = vsync;
    }

    VkExtent2D clientExtent = {};
    if (isWindowPresentOccluded(m_window, &clientExtent)) {
      if (!(PresentFlags & DXGI_PRESENT_TEST)) {
        Com<ID3D11DeviceContext> deviceContext = nullptr;
        m_parent->GetImmediateContext(&deviceContext);

        if (deviceContext != nullptr)
          static_cast<D3D11ImmediateContext*>(deviceContext.ptr())->Flush();

        logOccludedPresent(m_window, clientExtent, "pre-present");
      }

      return DXGI_STATUS_OCCLUDED;
    }

    if (m_presenter == nullptr)
      CreatePresenter();

    HRESULT hr = S_OK;

    if (!m_presenter->hasSwapChain()) {
      RecreateSwapChain(m_vsync);
      m_dirty = false;
    }

    if (!m_presenter->hasSwapChain())
      hr = DXGI_STATUS_OCCLUDED;

    if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)
      hr = DXGI_ERROR_DEVICE_REMOVED;
    else if (m_device->getDeviceStatus() != VK_SUCCESS)
      hr = DXGI_ERROR_DEVICE_RESET;

    if ((PresentFlags & DXGI_PRESENT_TEST) || hr != S_OK)
      return hr;

    if (std::exchange(m_dirty, false))
      RecreateSwapChain(m_vsync);
    
    try {
      hr = PresentImage(SyncInterval);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      hr = E_FAIL;
    }

    return hr;
  }


  void STDMETHODCALLTYPE D3D11SwapChain::NotifyModeChange(
          BOOL                      Windowed,
    const DXGI_MODE_DESC*           pDisplayMode) {
    if (Windowed || !pDisplayMode) {
      // Display modes aren't meaningful in windowed mode
      m_displayRefreshRate = 0.0;
    } else {
      DXGI_RATIONAL rate = pDisplayMode->RefreshRate;
      m_displayRefreshRate = double(rate.Numerator) / double(rate.Denominator);
    }

    if (m_presenter != nullptr)
      m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);
  }


  HRESULT D3D11SwapChain::PresentImage(UINT SyncInterval) {
    Com<ID3D11DeviceContext> deviceContext = nullptr;
    m_parent->GetImmediateContext(&deviceContext);

    auto immediateContext = static_cast<D3D11ImmediateContext*>(deviceContext.ptr());
    // Determine primary swap chain BEFORE flush.
    // Only the primary drives RT frame boundaries and the Remix overlay.
    // Prefer the visible, window-sized presenter with real scene draws so
    // transient helper or dummy chains do not steal authority.
    bool isPrimary = false;
    bool stole = false;
    const uint32_t thisDraws = immediateContext->m_rtx.getDrawCallID();
    const uint32_t thisSceneDraws = immediateContext->m_rtx.getAcceptedSceneDrawCount();
    m_lastPresentDrawCount = thisDraws;

    // DX11_V293_FIRST_PRESENT_TRACE: some games (CoD AW; Dolphin-style hosts)
    // leave logs that end right after swapchain creation with no frame ever
    // presented. Bracket the first presents so one log distinguishes "the
    // game never calls Present" (engine-side wait) from "Present itself
    // hangs" - a begin line without its end line means the hang is inside.
    static std::atomic<uint32_t> s_presentTraceCounter { 0u };
    const uint32_t presentTraceIndex =
      s_presentTraceCounter.load(std::memory_order_relaxed) < 8u
        ? s_presentTraceCounter.fetch_add(1u, std::memory_order_relaxed)
        : 8u;
    struct PresentTraceGuard {
      uint32_t index;
      bool active;
      ~PresentTraceGuard() {
        if (active)
          KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] PresentImage end #", index));
      }
    } presentTraceGuard { presentTraceIndex, presentTraceIndex < 8u };
    if (presentTraceIndex < 8u) {
      KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] PresentImage begin #", presentTraceIndex,
        " draws=", thisDraws, " sync=", SyncInterval,
        " HWND=", (uintptr_t) m_window));
    }

    // Fallback for games/window systems that do not deliver keyboard messages
    // to the hooked WndProc. Rising-edge polling requests Remix capture; the
    // WndProc path above remains responsible for blocking vanilla handling.
    const bool printScreenDown =
      (::GetAsyncKeyState(VK_SNAPSHOT) & 0x8000) != 0;
    const bool printScreenWasDown = g_printScreenPolledDown.exchange(
      printScreenDown, std::memory_order_relaxed);
    if (printScreenDown && !printScreenWasDown)
      g_remixScreenshotRequested.store(true, std::memory_order_release);

    if (thisSceneDraws > 0u) {
      if (m_scenePresentStreak < kRemixOverlayArmSceneFrames)
        ++m_scenePresentStreak;
    } else {
      m_scenePresentStreak = 0u;
    }

    if (!m_remixOverlayArmed && m_scenePresentStreak >= kRemixOverlayArmSceneFrames) {
      m_remixOverlayArmed = true;
      KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] Remix overlay armed after scene initialization: HWND=",
        (uintptr_t)m_window,
        " sceneDraws=", thisSceneDraws,
        " draws=", thisDraws,
        " ", m_desc.Width, "x", m_desc.Height,
        " this=", (uintptr_t)this));
    }

    // Device-independent Alt+X poll: if the user pressed the Remix hotkey,
    // force this visible swapchain to claim primary so the UI can render
    // even when primary election is stuck on another (idle/dummy) chain.
    // Uses GetAsyncKeyState rising-edge so it works regardless of WndProc state.
    {
      static bool s_altXPrevDown = false;
      const bool altDown = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
      const bool xDown   = (::GetAsyncKeyState('X') & 0x8000) != 0;
      const bool bothDown = altDown && xDown;
      const bool rising = bothDown && !s_altXPrevDown;
      s_altXPrevDown = bothDown;

      if (rising && IsWindow(m_window) && IsWindowVisible(m_window) && !IsIconic(m_window)) {
        if (!m_remixOverlayArmed) {
          m_remixOverlayArmed = true;
          KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] Remix overlay armed by explicit async hotkey: HWND=",
            (uintptr_t) m_window,
            " draws=", thisDraws,
            " sceneDraws=", thisSceneDraws,
            " ", m_desc.Width, "x", m_desc.Height,
            " this=", (uintptr_t) this));
        }

        // Re-install WndProc hook on this window so subsequent key events reach ImGui.
        InstallWndProcHook();

        // Force primary to this chain so the very next frame renders UI.
        {
        std::lock_guard<std::mutex> pk(g_primaryMutex);
          if (g_primarySwapChain != this) {
          KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] Alt+X pressed — forcing primary from ",
            (uintptr_t)g_primarySwapChain, " to ", (uintptr_t)this, " (HWND=", (uintptr_t)m_window, ")"));
          g_primarySwapChain = this;
          // Reset hysteresis so the forced primary is not immediately challenged.
          g_primaryChallenger = nullptr;
          g_primaryChallengerFrames = 0;
        }
        }

        auto& gui = m_device->getCommon()->getImgui();
        // DX11_V270_MENU_TOGGLE_DEBOUNCE: shared debounced toggle so this
        // async poll cannot double-fire against the WndProc/ImGui paths.
        gui.toggleMenuFromHotkey();
        gui.markRemixMenuHotkeyHandled();
      }
    }

    struct PrimaryCandidateInfo {
      bool visible = false;
      bool isForeground = false;
      bool remixArmed = false;
      bool exactClientMatch = false;
      bool nearClientMatch = false;
      bool hasDraws = false;
      uint64_t area = 0;
      uint32_t draws = 0;
      uint32_t clientWidth = 0;
      uint32_t clientHeight = 0;
    };

    auto describeCandidate = [](D3D11SwapChain* sc) {
      PrimaryCandidateInfo info;

      if (sc == nullptr)
        return info;

      info.draws = sc->m_lastPresentDrawCount;
      info.hasDraws = info.draws > 0;
      info.remixArmed = sc->m_remixOverlayArmed;

      const uint32_t width = sc->m_desc.Width;
      const uint32_t height = sc->m_desc.Height;
      info.area = uint64_t(width) * uint64_t(height);

      if (width < 64 || height < 64)
        return info;

      info.visible = IsWindow(sc->m_window) && IsWindowVisible(sc->m_window) && !IsIconic(sc->m_window);
      info.isForeground = (sc->m_window == GetForegroundWindow());

      RECT clientRect = {};
      if (IsWindow(sc->m_window) && GetClientRect(sc->m_window, &clientRect)) {
        auto nonNegative = [](LONG value) -> uint32_t {
          return value > 0 ? static_cast<uint32_t>(value) : 0u;
        };
        auto absDiff = [](uint32_t a, uint32_t b) -> uint32_t {
          return a > b ? a - b : b - a;
        };

        info.clientWidth = nonNegative(clientRect.right - clientRect.left);
        info.clientHeight = nonNegative(clientRect.bottom - clientRect.top);

        if (info.clientWidth > 0 && info.clientHeight > 0) {
          const uint32_t exactWidthTolerance = info.clientWidth / 16u + 8u;
          const uint32_t exactHeightTolerance = info.clientHeight / 16u + 8u;
          const uint32_t nearWidthTolerance = info.clientWidth / 4u + 64u;
          const uint32_t nearHeightTolerance = info.clientHeight / 4u + 64u;

          info.exactClientMatch = absDiff(width, info.clientWidth) <= exactWidthTolerance
                               && absDiff(height, info.clientHeight) <= exactHeightTolerance;
          info.nearClientMatch = absDiff(width, info.clientWidth) <= nearWidthTolerance
                              && absDiff(height, info.clientHeight) <= nearHeightTolerance;
        }
      }

      return info;
    };

    auto isClearlyBetterCandidate = [](const PrimaryCandidateInfo& candidate, const PrimaryCandidateInfo& current) {
      // Priority 1: Visibility — invisible windows never win.
      if (candidate.visible != current.visible)
        return candidate.visible;

      // Priority 2: Remix arming. Once a swapchain has produced real scene
      // geometry, it should beat an unarmed launcher/settings chain even if
      // the launcher was the first presenter.
      if (candidate.remixArmed != current.remixArmed)
        return candidate.remixArmed;

      // NOTE: exactClientMatch and nearClientMatch are intentionally NOT checked here.
      // These fields are retained in PrimaryCandidateInfo for logging/diagnostics only.
      // Client-rect matching penalizes games rendering at non-native resolution (e.g., 720p
      // in a 1080p window) and allows smaller secondary swapchains to steal primary when their
      // backbuffer happens to match their window size. Backbuffer area and draw count are more
      // reliable signals for identifying the main game swapchain. (See requirement 2.2)

      // Priority 3: Foreground window — strongest signal for the main game window in
      // multi-window games. The foreground window is the one the user is interacting with.
      // (See requirements 2.1, 2.6)
      if (candidate.isForeground != current.isForeground)
        return candidate.isForeground;

      // Priority 4: Has draws — a swapchain actively drawing is more likely the game.
      if (candidate.hasDraws != current.hasDraws)
        return candidate.hasDraws;

      if (current.area == 0)
        return candidate.area > 0;

      // Priority 5: Area with incumbency advantage — require the candidate to have more than
      // double the current primary's area (100% margin). This prevents marginal candidates
      // (e.g., a UI burst on a slightly larger surface) from stealing primary while the
      // current primary is still healthy. (See requirement 2.5)
      if (candidate.area > current.area * 2)
        return true;

      // Priority 6: Draw count with higher threshold — require 64 extra draws to differentiate.
      // A higher threshold prevents transient draw-count spikes (UI updates, loading screens)
      // from causing election thrashing. (See requirement 2.5)
      if (candidate.draws > current.draws + 64)
        return true;

      return false;
    };

    D3D11SwapChain* previousPrimary = nullptr;

    {
      std::lock_guard<std::mutex> lk(g_primaryMutex);
      previousPrimary = g_primarySwapChain;
      if (g_primarySwapChain == nullptr || !IsWindow(g_primarySwapChain->m_window)) {
        // No current primary or its window is gone — elect immediately (no hysteresis).
        g_primarySwapChain = this;
        g_primaryChallenger = nullptr;
        g_primaryChallengerFrames = 0;
        stole = true;
      } else if (g_primarySwapChain != this && thisDraws > 0) {
        const PrimaryCandidateInfo candidate = describeCandidate(this);
        const PrimaryCandidateInfo current = describeCandidate(g_primarySwapChain);
        if (isClearlyBetterCandidate(candidate, current)) {
          // Candidate is better — apply hysteresis before allowing the steal.
          if (g_primaryChallenger == this) {
            ++g_primaryChallengerFrames;
          } else {
            // Different challenger or first challenge — start counting.
            g_primaryChallenger = this;
            g_primaryChallengerFrames = 1;
          }
          if (g_primaryChallengerFrames >= kPrimaryHysteresisFrames) {
            g_primarySwapChain = this;
            g_primaryChallenger = nullptr;
            g_primaryChallengerFrames = 0;
            stole = true;
          }
        } else {
          // Current primary is still at least as good — reset challenger state.
          if (g_primaryChallenger == this) {
            g_primaryChallenger = nullptr;
            g_primaryChallengerFrames = 0;
          }
        }
      }
      isPrimary = (g_primarySwapChain == this);
    }

    // Ctrl+Alt+P is the safe-capture baseline: rigid opaque
    // world instances are submitted while the native raster image is retained,
    // and the frontend state is restored around the capture work.
    {
      static bool s_ctrlAltPPrevDown = false;
      const bool ctrlDown = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool altDown = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
      const bool pDown = (::GetAsyncKeyState('P') & 0x8000) != 0;
      const bool chordDown = kenshi_telemetry::enabled() && ctrlDown && altDown && pDown;
      const bool rising = chordDown && !s_ctrlAltPPrevDown;
      s_ctrlAltPPrevDown = chordDown;

      if (isPrimary && rising) {
        const bool enabled = immediateContext->m_rtx.ToggleUserSafeCaptureBaseline();
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[D3D11SwapChain] Ctrl+Alt+P: safe capture baseline=",
          enabled ? 1 : 0,
          enabled
            ? " (rigid-world submission plus frontend-state-restore; native raster forced)"
            : " (safe capture baseline disabled)"));
      }
    }

    if (stole && previousPrimary != nullptr && previousPrimary != this && previousPrimary->m_device != m_device) {
      auto& previousGui = previousPrimary->m_device->getCommon()->getImgui();
      auto& currentGui = m_device->getCommon()->getImgui();
      const UIType previousMenuType = previousGui.getMenuType();
      const UIType currentMenuType = currentGui.getMenuType();

      if (previousMenuType != currentMenuType) {
        KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] Mirroring UI state onto new primary device: old=",
          static_cast<int>(previousMenuType),
          " new=", static_cast<int>(currentMenuType),
          " oldSwapChain=", (uintptr_t)previousPrimary,
          " newSwapChain=", (uintptr_t)this));
        currentGui.switchMenu(previousMenuType, true, false);
      }
    }

    if (isPrimary && m_remixOverlayArmed) {
      InstallWndProcHook();
    }

    VkExtent2D clientExtent = {};
    if (isWindowPresentOccluded(m_window, &clientExtent)) {
      immediateContext->Flush();
      logOccludedPresent(m_window, clientExtent, "present-image");
      return DXGI_STATUS_OCCLUDED;
    }

    if (stole) {
      const PrimaryCandidateInfo candidate = describeCandidate(this);
      KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] Primary claimed: HWND=", (uintptr_t)m_window,
        " draws=", thisDraws,
        " ", m_desc.Width, "x", m_desc.Height,
        " client=", candidate.clientWidth, "x", candidate.clientHeight,
        " exact=", candidate.exactClientMatch ? 1 : 0,
        " near=", candidate.nearClientMatch ? 1 : 0,
        " armed=", candidate.remixArmed ? 1 : 0,
        " this=", (uintptr_t)this));
    }

    const bool wantDlfg = isPrimary && m_context->isDLFGEnabled();
    if (wantDlfg != (m_dlfgPresenter != nullptr)) {
      CreatePresenter(wantDlfg);
      // Match D3D9's CreatePresenter -> RecreateSwapChain sequence. The base constructor cannot dispatch to
      // the DLFG override, which establishes the application backbuffers and sizes the real WSI chain.
      // m_dirty was consumed before entering PresentImage, so do this now.
      RecreateSwapChain(wantDlfg ? FALSE : m_vsync);
    }
    if (wantDlfg)
      SyncInterval = 0;

    auto& reflex = m_device->getCommon()->metaReflex();
    if (isPrimary) {
      reflex.setLatencyPingThread();
      if (!m_reflexSimulationStarted) {
        reflex.beginSimulation(D3D11Rtx::GetReflexFrameId());
        m_reflexSimulationStarted = true;
      }
      reflex.endSimulation(D3D11Rtx::GetReflexFrameId());
    }

    // EndFrame BEFORE Flush — EndFrame emits injectRTX to the CS queue;
    // the Flush below submits those RT commands to the GPU so the backbuffer
    // contains the ray-traced composite when the blitter copies it to the
    // Vulkan swap chain image.
    if (isPrimary) {
      // Ctrl+Alt+O: the general capture on the foreground primary swapchain.
      static bool s_ctrlAltOPrevDown = false;
      DWORD captureForegroundPid = 0;
      GetWindowThreadProcessId(GetForegroundWindow(), &captureForegroundPid);
      const bool captureChordDown = captureForegroundPid == GetCurrentProcessId()
        && isVirtualKeyPressedNow(VK_CONTROL) && isVirtualKeyPressedNow(VK_MENU)
        && isVirtualKeyPressedNow('O');
      const bool captureRising = captureChordDown && !s_ctrlAltOPrevDown;
      s_ctrlAltOPrevDown = captureChordDown;
      if (captureRising && kenshi_telemetry::fileOutputEnabled() && kenshi_telemetry::enabled()) {
        immediateContext->m_rtx.ArmOnDemandDiagnostics();
        immediateContext->m_rtx.RequestKenshiDebugScreenshot();
        // Terrain metadata belongs to the same capture action.
        terrain_audit::requested.store(true, std::memory_order_relaxed);
        KENSHI_DIAGNOSTIC_INFO("[D3D11SwapChain][capture-v744] Ctrl+Alt+O: main capture armed; terrain trace included; keep cursor on target; join by frame+draw ID");
      }
      if (g_remixScreenshotRequested.exchange(false, std::memory_order_acq_rel)) {
        immediateContext->m_rtx.RequestScreenshot();
        KENSHI_DIAGNOSTIC_INFO("[D3D11SwapChain] Remix screenshot request queued for the primary composite");
      }
      immediateContext->m_rtx.EndFrame(m_swapImage, getImageExtent2D(m_swapImage));
    }

    // Flush all pending CS work: game draw commands AND (if primary) the
    // injectRTX RT blit.  After this, m_swapImage holds the composited image.
    immediateContext->Flush();

    // Bump our frame id.
    ++m_frameId;
    
    for (uint32_t i = 0; i < SyncInterval || i < 1; i++) {
      const VkResult presentStatus = SynchronizePresent();
      if (presentStatus == VK_ERROR_DEVICE_LOST) {
        Logger::err("[D3D11SwapChain] Present failed with VK_ERROR_DEVICE_LOST; returning DXGI_ERROR_DEVICE_REMOVED without swapchain recreation.");
        return DXGI_ERROR_DEVICE_REMOVED;
      }

      if (!m_presenter->hasSwapChain())
        return DXGI_STATUS_OCCLUDED;

      // Presentation semaphores and WSI swap chain image
      vk::PresenterInfo info = m_presenter->info();
      vk::PresenterSync sync;
      const VkExtent2D remixRenderExtent = getImageExtent2D(m_swapImage);

      uint32_t imageIndex = 0;

      VkResult status = m_presenter->acquireNextImage(sync, imageIndex);

      uint32_t acquireRetries = 0;
      while (status != VK_SUCCESS) {
        RecreateSwapChain(m_vsync);

        if (!m_presenter->hasSwapChain())
          return DXGI_STATUS_OCCLUDED;
        
        info = m_presenter->info();
        status = m_presenter->acquireNextImage(sync, imageIndex);

        if (status == VK_SUBOPTIMAL_KHR)
          break;

        // Bail out after a few retries — the surface keeps invalidating
        // (window backgrounded, minimized, or being resized).  The caller
        // will retry on the next Present call.
        if (++acquireRetries > 3)
          return DXGI_STATUS_OCCLUDED;
      }

      // Resolve back buffer if it is multisampled. We
      // only have to do it only for the first frame.
      m_context->beginRecording(
        m_device->createCommandList());

      if (m_swapImageRtView != nullptr) {
        DxvkRenderTargets renderTargets;
        renderTargets.color[0].view = m_swapImageRtView;
        renderTargets.color[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        m_context->bindRenderTargets(renderTargets);

        if (m_hud != nullptr)
          m_hud->render(m_context, info.format, remixRenderExtent);

        if (isPrimary && m_remixOverlayArmed) {
          // Render Remix ImGui into the currently bound Remix backbuffer extent.
          // The final blitter owns any scaling/copy to the WSI present image.
          RtxOptionManager::applyPendingValues(m_device.ptr(), false);
          auto& gui = m_device->getCommon()->getImgui();
          gui.render(m_window, m_context, remixRenderExtent, m_vsync);
        }
      }
      
      m_blitter->presentImage(m_context.ptr(),
        m_imageViews.at(imageIndex), VkRect2D(),
        m_swapImageView, VkRect2D());

      if (i + 1 >= SyncInterval)
        m_context->signal(m_frameLatencySignal, m_frameId);

      if (isPrimary) {
        // Register the acquired swapchain image as the present target.
        const Rc<DxvkImage>& presentImage = m_imageViews.at(imageIndex)->image();
        immediateContext->m_rtx.OnPresent(presentImage, getImageExtent2D(presentImage));
      }

      SubmitPresent(immediateContext, sync, i, isPrimary, imageIndex);
    }

    SyncFrameLatency();
    if (isPrimary) {
      reflex.sleep();
      D3D11Rtx::IncrementReflexFrameId();
      reflex.beginSimulation(D3D11Rtx::GetReflexFrameId());
      reflex.latencyPing(D3D11Rtx::GetReflexFrameId());
    }
    return S_OK;
  }


  void D3D11SwapChain::SubmitPresent(
          D3D11ImmediateContext*  pContext,
    const vk::PresenterSync&      Sync,
          uint32_t                FrameId,
          bool                    IsPrimary,
          uint32_t                ImageIndex) {
    auto lock = pContext->LockContext();

    // Present from CS thread so that we don't
    // have to synchronize with it first.
    m_presentStatus.result = VK_NOT_READY;

    pContext->EmitCs([this,
      cReflexFrameId = IsPrimary ? D3D11Rtx::GetReflexFrameId() : 0,
      cImageIndex = ImageIndex,
      cFrameId     = FrameId,
      cSync        = Sync,
      cHud         = m_hud,
      cIsPrimary   = IsPrimary,
      cCommandList = m_context->endRecording()
    ] (DxvkContext* ctx) {
      m_device->submitCommandList(cCommandList,
        cSync.acquire, cSync.present);

      if (cHud != nullptr && !cFrameId)
        cHud->update(m_dlfgPresenter != nullptr ? m_dlfgPresenter->getPresentFrameCount() : 1);

      const bool markers = cIsPrimary && (m_dlfgPresenter == nullptr || (__DLFG_REFLEX_WORKAROUND != 0));
      m_device->presentImage(cReflexFrameId, markers, cImageIndex, m_presenter, &m_presentStatus);

      // Only the primary chain advances the Remix frame counter.  Secondary
      // chains (video/loading) present their own images but must not bump
      // getCurrentFrameId() — that would desynchronize the camera validity
      // check in injectRTX and cause "not detecting a valid camera" errors.
      if (cIsPrimary) {
        m_device->incrementPresentCount();
        const uint32_t newFrameId = m_device->getCurrentFrameId();
        if (newFrameId < 32 || (newFrameId < 512 && (newFrameId % 64) == 0)) {
          KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] CS incrementPresentCount: newFrameId=",
            newFrameId));
        }
      }
    });

    pContext->FlushCsChunk();
  }


  VkResult D3D11SwapChain::SynchronizePresent() {
    // Recreate swap chain if the previous present call failed
    VkResult status = m_device->waitForSubmission(&m_presentStatus);

    if (status == VK_ERROR_DEVICE_LOST) {
      return status;
    }

    if (status != VK_SUCCESS && status != VK_EVENT_SET)
      RecreateSwapChain(m_vsync);

    return status;
  }


  void D3D11SwapChain::RecreateSwapChain(BOOL Vsync) {
    // Logged so we can correlate "game refreshes / black flash" events in
    // the log with actual swapchain recreations. Very useful when the UI
    // triggers a settings change that forces a recreate.
    KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain] RecreateSwapChain: ",
      m_desc.Width, "x", m_desc.Height,
      " bufCount=", m_desc.BufferCount,
      " vsync=", Vsync ? 1 : 0,
      " fmt=", (uint32_t) m_desc.Format));

    // Ensure that we can safely destroy the swap chain
    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();

    m_presentStatus.result = VK_SUCCESS;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = { m_desc.Width, m_desc.Height };
    presenterDesc.imageCount      = PickImageCount(m_desc.BufferCount + 1);
    presenterDesc.numFormats      = PickFormats(m_desc.Format, presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(Vsync, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    if (m_presenter->recreateSwapChain(presenterDesc) != VK_SUCCESS)
      throw DxvkError("D3D11SwapChain: Failed to recreate swap chain");
    
    CreateRenderTargetViews();
  }


  void D3D11SwapChain::CreateFrameLatencyEvent() {
    m_frameLatencySignal = new sync::CallbackFence(m_frameId);

    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
      m_frameLatencyEvent = CreateSemaphore(nullptr, m_frameLatency, DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);
  }


  void D3D11SwapChain::CreatePresenter(bool enableDlfg) {
    // Flush CS work before replacing a presenter referenced by pending presents.
    Com<ID3D11DeviceContext> deviceContext;
    m_parent->GetImmediateContext(&deviceContext);
    auto* immediate = static_cast<D3D11ImmediateContext*>(deviceContext.ptr());
    immediate->Flush();
    immediate->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
    m_device->waitForSubmission(&m_presentStatus);
    if (m_dlfgPresenter != nullptr)
      m_dlfgPresenter->synchronize();
    m_device->waitForIdle();
    m_presenter = nullptr;
    m_dlfgPresenter = nullptr;
    m_presentStatus.result = VK_SUCCESS;
    DxvkDeviceQueue graphicsQueue = enableDlfg ? m_device->queues().present : m_device->queues().graphics;

    vk::PresenterDevice presenterDevice;
    presenterDevice.queueFamily   = graphicsQueue.queueFamily;
    presenterDevice.queue         = graphicsQueue.queueHandle;
    presenterDevice.adapter       = m_device->adapter()->handle();
    presenterDevice.features.fullScreenExclusive = m_device->extensions().extFullScreenExclusive;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = { m_desc.Width, m_desc.Height };
    presenterDesc.imageCount      = PickImageCount(m_desc.BufferCount + 1);
    presenterDesc.numFormats      = PickFormats(m_desc.Format, presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(false, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    if (enableDlfg) {
      ++presenterDesc.imageCount;
      m_dlfgPresenter = new DxvkDLFGPresenter(m_device, m_context, m_window,
        m_device->adapter()->vki(), m_device->vkd(), presenterDevice, presenterDesc);
      m_presenter = m_dlfgPresenter;
    } else {
      m_presenter = new vk::Presenter(m_window,
        m_device->adapter()->vki(), m_device->vkd(), presenterDevice, presenterDesc);
    }
    KENSHI_DIAGNOSTIC_INFO(str::format("[D3D11SwapChain][DLFG] presenter=", enableDlfg ? "frame-generation" : "ordinary"));
    
    m_presenter->setFrameRateLimit(m_parent->GetOptions()->maxFrameRate);
    m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);

    CreateRenderTargetViews();
  }


  void D3D11SwapChain::CreateRenderTargetViews() {
    vk::PresenterInfo info = m_presenter->info();

    m_imageViews.clear();
    m_imageViews.resize(info.imageCount);

    DxvkImageCreateInfo imageInfo;
    imageInfo.type        = VK_IMAGE_TYPE_2D;
    imageInfo.format      = info.format.format;
    imageInfo.flags       = 0;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent      = { info.imageExtent.width, info.imageExtent.height, 1 };
    imageInfo.numLayers   = 1;
    imageInfo.mipLevels   = 1;
    imageInfo.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.stages      = 0;
    imageInfo.access      = 0;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout      = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageInfo.shared      = VK_TRUE;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type         = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format       = info.format.format;
    viewInfo.usage        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.aspect       = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel     = 0;
    viewInfo.numLevels    = 1;
    viewInfo.minLayer     = 0;
    viewInfo.numLayers    = 1;

    for (uint32_t i = 0; i < info.imageCount; i++) {
      VkImage imageHandle = m_presenter->getImage(i).image;
      
      Rc<DxvkImage> image = new DxvkImage(
        m_device.ptr(), imageInfo, imageHandle);

      m_imageViews[i] = new DxvkImageView(
        m_device->vkd(), image, viewInfo);
    }
  }


  void D3D11SwapChain::CreateBackBuffer() {
    // Explicitly destroy current swap image before
    // creating a new one to free up resources
    if (m_backBuffer)
      m_backBuffer->ReleasePrivate();
    
    m_swapImage         = nullptr;
    m_swapImageView     = nullptr;
    m_swapImageRtView   = nullptr;
    m_backBuffer        = nullptr;

    // Create new back buffer
    D3D11_COMMON_TEXTURE_DESC desc;
    desc.Width              = std::max(m_desc.Width,  1u);
    desc.Height             = std::max(m_desc.Height, 1u);
    desc.Depth              = 1;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = m_desc.Format;
    desc.SampleDesc         = m_desc.SampleDesc;
    desc.Usage              = D3D11_USAGE_DEFAULT;
    desc.BindFlags          = 0;
    desc.CPUAccessFlags     = 0;
    desc.MiscFlags          = 0;
    desc.TextureLayout      = D3D11_TEXTURE_LAYOUT_UNDEFINED;

    if (m_desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT)
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;

    if (m_desc.BufferUsage & DXGI_USAGE_SHADER_INPUT)
      desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    if (m_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
      desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    
    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)
      desc.MiscFlags |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    
    DXGI_USAGE dxgiUsage = DXGI_USAGE_BACK_BUFFER;

    if (m_desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD
     || m_desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD)
      dxgiUsage |= DXGI_USAGE_DISCARD_ON_PRESENT;

    m_backBuffer = new D3D11Texture2D(m_parent, &desc, dxgiUsage, VK_NULL_HANDLE);
    m_backBuffer->AddRefPrivate();

    m_swapImage = GetCommonTexture(m_backBuffer)->GetImage();

    // Create an image view that allows the
    // image to be bound as a shader resource.
    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type       = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format     = m_swapImage->info().format;
    viewInfo.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel   = 0;
    viewInfo.numLevels  = 1;
    viewInfo.minLayer   = 0;
    viewInfo.numLayers  = 1;
    m_swapImageView = m_device->createImageView(m_swapImage, viewInfo);

    viewInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    m_swapImageRtView = m_device->createImageView(m_swapImage, viewInfo);
    
    // Initialize the image so that we can use it. Clearing
    // to black prevents garbled output for the first frame.
    VkImageSubresourceRange subresources;
    subresources.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    subresources.baseMipLevel   = 0;
    subresources.levelCount     = 1;
    subresources.baseArrayLayer = 0;
    subresources.layerCount     = 1;

    VkClearColorValue clearColor;
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 0.0f;

    m_context->beginRecording(
      m_device->createCommandList());
    
    m_context->clearColorImage(
      m_swapImage, clearColor, subresources);

    m_device->submitCommandList(
      m_context->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE);
  }


  void D3D11SwapChain::CreateBlitter() {
    m_blitter = new DxvkSwapchainBlitter(m_device);    
  }


  void D3D11SwapChain::CreateHud() {
    m_hud = hud::Hud::createHud(m_device);

    if (m_hud != nullptr)
      m_hud->addItem<hud::HudClientApiItem>("api", 1, GetApiName());
  }


  // Close a handle without ever letting a c0000008 (STATUS_INVALID_HANDLE) propagate. Games that enable
  // strict handle checking (PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY - Unity titles do) turn any
  // close of an invalid/stale handle into a fatal exception that terminates the process. DXVK closing a
  // bookkeeping handle must never be able to kill the game, so we swallow that exception. SEH lives in
  // this standalone helper (no C++ objects to unwind) so it composes with /EHsc cleanly.
  static void SafeCloseHandle(HANDLE h) {
    if (!h)
      return;
    __try {
      ::CloseHandle(h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // handle was already closed / invalid under strict handle checking - ignore
    }
  }

  void D3D11SwapChain::DestroyFrameLatencyEvent() {
    // Only the FRAME_LATENCY_WAITABLE_OBJECT swapchain ever creates this handle; a non-waitable
    // swapchain leaves it null. Close it through SafeCloseHandle and null it so teardown can never
    // close a null/stale handle and terminate the process. Affects every game that recreates or
    // releases its swapchain (resolution/fullscreen changes - i.e. the menu->gameplay transition).
    HANDLE h = m_frameLatencyEvent;
    m_frameLatencyEvent = nullptr;
    SafeCloseHandle(h);
  }


  void D3D11SwapChain::SyncFrameLatency() {
    // Wait for the sync event so that we respect the maximum frame latency
    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());

    if (m_frameLatencyEvent) {
      m_frameLatencySignal->setCallback(m_frameId, [cFrameLatencyEvent = m_frameLatencyEvent] () {
        ReleaseSemaphore(cFrameLatencyEvent, 1, nullptr);
      });
    }
  }


  uint32_t D3D11SwapChain::GetActualFrameLatency() {
    uint32_t maxFrameLatency = m_frameLatency;

    if (!(m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      m_dxgiDevice->GetMaximumFrameLatency(&maxFrameLatency);

    if (m_frameLatencyCap)
      maxFrameLatency = std::min(maxFrameLatency, m_frameLatencyCap);

    maxFrameLatency = std::min(maxFrameLatency, m_desc.BufferCount + 1);
    return maxFrameLatency;
  }


  uint32_t D3D11SwapChain::PickFormats(
          DXGI_FORMAT               Format,
          VkSurfaceFormatKHR*       pDstFormats) {
    uint32_t n = 0;

    switch (Format) {
      default:
        Logger::warn(str::format("D3D11SwapChain: Unexpected format: ", m_desc.Format));
        
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R10G10B10A2_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        pDstFormats[n++] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
    }

    return n;
  }


  uint32_t D3D11SwapChain::PickPresentModes(
          BOOL                      Vsync,
          VkPresentModeKHR*         pDstModes) {
    uint32_t n = 0;

    // DX11_V236_INTEL_FIFO_NO_TEARING_BYPASS: on Intel Arc, IMMEDIATE (tearing) present on a WINDOWED
    // surface forces the driver's DWM-bypass path, whose vkCreateSwapchainKHR internally calls OpenGL
    // ChoosePixelFormat/wglChoosePixelFormat and stalls indefinitely - the game never renders (verified
    // via live captures; persists on the latest Arc driver). FIFO presents THROUGH the DWM compositor
    // and never touches that GL path. Force FIFO on Intel so the swapchain is actually created and the
    // game renders. Trade-off: vsync-capped/no-tearing on Intel, which is correct vs. "doesn't render".
    if (DxvkGpuVendor(m_device->properties().core.properties.vendorID) == DxvkGpuVendor::Intel) {
      if (m_parent->GetOptions()->tearFree == Tristate::False)
        pDstModes[n++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
      return n;
    }

    if (Vsync) {
      if (m_parent->GetOptions()->tearFree == Tristate::False)
        pDstModes[n++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
    } else {
      if (m_parent->GetOptions()->tearFree != Tristate::True)
        pDstModes[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    return n;
  }


  uint32_t D3D11SwapChain::PickImageCount(
          UINT                      Preferred) {
    int32_t option = m_parent->GetOptions()->numBackBuffers;
    return option > 0 ? uint32_t(option) : uint32_t(Preferred);
  }


  VkFullScreenExclusiveEXT D3D11SwapChain::PickFullscreenMode() {
    // DX11_V237_INTEL_FORCE_FSE: on Intel Arc the WINDOWED Vulkan present path is broken inside the
    // driver (IMMEDIATE -> OpenGL ChoosePixelFormat stall; FIFO -> D3D12CreateDevice CPU-runaway), so a
    // windowed swapchain never renders. Real fullscreen-exclusive presents via direct scanout and
    // avoids that interop entirely. Force application-controlled FSE on Intel so the swapchain acquires
    // exclusive fullscreen (DXVK calls vkAcquireFullScreenExclusiveModeEXT) and can actually present.
    // The FSE feature is kept enabled on Intel (see vulkan_presenter.cpp V237) and the V233 pump
    // services the FSE window messaging.
    // ALLOWED (not APPLICATION_CONTROLLED): with ALLOWED the driver may acquire FSE DURING
    // vkCreateSwapchainKHR, so the create goes straight to exclusive scanout and skips the broken
    // windowed (GL/D3D12) setup. APPLICATION_CONTROLLED creates windowed first then acquires, which
    // still hits the broken path. Pairs with the window being forced foreground in recreateSwapChain.
    if (DxvkGpuVendor(m_device->properties().core.properties.vendorID) == DxvkGpuVendor::Intel)
      return VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT;

    if (!RtxOptions::allowFSE())
      return VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;

    return m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
      ? VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT
      : VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
  }


  std::string D3D11SwapChain::GetApiName() const {
    Com<IDXGIDXVKDevice> device;
    m_parent->QueryInterface(__uuidof(IDXGIDXVKDevice), reinterpret_cast<void**>(&device));

    uint32_t apiVersion = device->GetAPIVersion();
    uint32_t featureLevel = m_parent->GetFeatureLevel();

    uint32_t flHi = (featureLevel >> 12);
    uint32_t flLo = (featureLevel >> 8) & 0x7;

    return str::format("D3D", apiVersion, " FL", flHi, "_", flLo);
  }

}
