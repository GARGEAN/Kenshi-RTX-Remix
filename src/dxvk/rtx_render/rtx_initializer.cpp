#include "../../util/util_kenshi_telemetry.h"
/*
* Copyright (c) 2022-2025, NVIDIA CORPORATION. All rights reserved.
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
#pragma once

#include "rtx_initializer.h"
#include "rtx_options.h"
#include "../../util/log/log.h"
#include "../../util/util_env.h"
#include "../../util/util_string.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include "../../util/thread.h"
#include "dxvk_context.h"
#include "dxvk_device.h"
#include "rtx_shader_manager.h"
#include "rtx_texture_manager.h"
#include "rtx_io.h"
#include "dxvk_raytracing.h"
#include "rtx_debug_view.h"

namespace dxvk {
  namespace {
    // DX11_V283_ONE_PREWARM_WINDOW: boot shader compilation runs as several
    // sequential phases (RT-pipeline prewarm during Vulkan device init, then
    // the cached game-shader preload in the D3D11 device constructor), and
    // engines/emulators that re-create the device (adapter probes, renderer
    // restarts) run the RT phase again for every new device. Each phase used
    // to construct its own dialog with its own thread and top-level window,
    // so a single boot flashed two or more "Please wait" compiler windows in
    // a row, each restarting its count from zero. The dialog is now a
    // process-wide singleton: the first phase creates the one window, later
    // phases reuse it, a short idle linger bridges the gap between phases,
    // and shader counts / elapsed time accumulate across phases. At most one
    // window is ever created per process. The description text is also
    // word-wrapped now (DT_SINGLELINE used to clip it at the window edge)
    // and the window height is measured from the wrapped text instead of
    // being hardcoded.
    class ShaderPrewarmDialog {
    public:
      static void beginPhase(const wchar_t* phaseLabel, uint32_t pendingHint) {
        {
          std::lock_guard<dxvk::mutex> lock(mutex());
          s_activePhases.fetch_add(1u, std::memory_order_acq_rel);
          ::wcsncpy_s(s_phaseLabel, phaseLabel, _TRUNCATE);

          if (!s_threadLaunched) {
            s_threadLaunched = true;
            s_startMs.store(::GetTickCount64(), std::memory_order_release);
            // Detached on purpose: the thread only touches this class's
            // leaked / trivially-destructible statics, and joining a UI
            // thread from a static destructor during DLL unload can deadlock
            // on the loader lock.
            dxvk::thread dialogThread([] {
              env::setThreadName("rtx-shader-dialog");
              runDialogThread();
            });
            dialogThread.detach();
          }
        }

        update(pendingHint);

        // Give the window a moment to come up before pipeline compilation
        // saturates every core; otherwise the dialog can appear seconds late.
        const uint64_t waitStartMs = ::GetTickCount64();
        while (!s_windowReady.load(std::memory_order_acquire)
            && ::GetTickCount64() - waitStartMs < 2000u) {
          ::Sleep(1);
        }
      }

      static void update(uint32_t pending) {
        const uint32_t previousPending =
          s_pending.exchange(pending, std::memory_order_acq_rel);

        uint32_t observedMaximum = s_phaseMaxPending.load(std::memory_order_acquire);
        while (pending > observedMaximum
            && !s_phaseMaxPending.compare_exchange_weak(
                 observedMaximum, pending, std::memory_order_release,
                 std::memory_order_acquire)) {
        }

        // Repaint when the count changes, otherwise at most ~10 Hz so the
        // elapsed-time text keeps ticking without flooding the window thread.
        const uint64_t nowMs = ::GetTickCount64();
        if (previousPending == pending
         && nowMs - s_lastRedrawMs.load(std::memory_order_acquire) < 100u)
          return;
        s_lastRedrawMs.store(nowMs, std::memory_order_release);

        const HWND hwnd = s_hwnd.load(std::memory_order_acquire);
        if (hwnd != nullptr)
          ::InvalidateRect(hwnd, nullptr, FALSE);
      }

      static void endPhase() {
        std::lock_guard<dxvk::mutex> lock(mutex());
        uint32_t activePhases = s_activePhases.load(std::memory_order_acquire);
        if (activePhases > 0u)
          activePhases = s_activePhases.fetch_sub(1u, std::memory_order_acq_rel) - 1u;

        if (activePhases == 0u) {
          // Fold the finished phase's pipelines into the cumulative total so
          // the progress bar continues instead of restarting when the next
          // phase (e.g. cached game shaders after RT prewarm) begins.
          s_completedBase.fetch_add(
            s_phaseMaxPending.exchange(0u, std::memory_order_acq_rel),
            std::memory_order_acq_rel);
          s_pending.store(0u, std::memory_order_release);
          s_idleSinceMs.store(::GetTickCount64(), std::memory_order_release);
        }

        const HWND hwnd = s_hwnd.load(std::memory_order_acquire);
        if (hwnd != nullptr)
          ::InvalidateRect(hwnd, nullptr, FALSE);
      }

    private:
      static constexpr UINT_PTR kIdleTimerId = 1u;
      static constexpr UINT kIdleTimerPeriodMs = 250u;
      // Keeps the one window alive across the short gap between sequential
      // prewarm phases so a boot never flashes a second window.
      static constexpr uint64_t kIdleCloseDelayMs = 2000u;
      static constexpr int kClientWidth = 580;
      static constexpr int kMarginX = 24;
      static constexpr size_t kPhaseLabelCapacity = 128;

      static constexpr const wchar_t* kBodyText =
        L"RTX Remix is compiling the game's cached shaders and its path-tracing "
        L"pipelines so they do not stutter on first use. This takes a while only "
        L"on the first launch or after a driver or Remix update; the game "
        L"continues automatically once compilation finishes.";

      struct Layout {
        RECT title;
        RECT body;
        RECT phase;
        RECT status;
        RECT bar;
        int clientHeight;
      };

      static HFONT createTitleFont() {
        return ::CreateFontW(
          -22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      }

      static HFONT createBodyFont() {
        return ::CreateFontW(
          -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      }

      static Layout computeLayout(HDC dc, HFONT bodyFont, int clientWidth) {
        const LONG left = kMarginX;
        const LONG right = clientWidth - kMarginX;

        // The description wraps to however many lines the font needs; measure
        // it so nothing is ever clipped by a hardcoded window height.
        RECT bodyMeasure = { left, 0, right, 0 };
        const HGDIOBJ previousFont = ::SelectObject(dc, bodyFont);
        ::DrawTextW(dc, kBodyText, -1, &bodyMeasure,
          DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        ::SelectObject(dc, previousFont);

        Layout layout = {};
        layout.title = { left, 22, right, 54 };
        layout.body = { left, 62, right, 62 + (bodyMeasure.bottom - bodyMeasure.top) };
        layout.phase = { left, layout.body.bottom + 14, right, layout.body.bottom + 36 };
        layout.status = { left, layout.phase.bottom + 3, right, layout.phase.bottom + 25 };
        layout.bar = { left, layout.status.bottom + 12, right, layout.status.bottom + 31 };
        layout.clientHeight = layout.bar.bottom + 22;
        return layout;
      }

      static LRESULT CALLBACK windowProc(
          HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
          case WM_ERASEBKGND:
            return 1;

          case WM_PAINT:
            paint(hwnd);
            return 0;

          case WM_TIMER:
            if (wParam == kIdleTimerId) {
              // Keep the elapsed-time display ticking, and close the window
              // only once every phase has been idle long enough that no
              // follow-up phase is coming - this is what merges the phases
              // into a single window.
              if (s_activePhases.load(std::memory_order_acquire) == 0u
               && ::GetTickCount64() - s_idleSinceMs.load(std::memory_order_acquire)
                    >= kIdleCloseDelayMs) {
                ::DestroyWindow(hwnd);
              } else {
                ::InvalidateRect(hwnd, nullptr, FALSE);
              }
              return 0;
            }
            break;

          case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;

          case WM_NCDESTROY:
            s_hwnd.store(nullptr, std::memory_order_release);
            ::PostQuitMessage(0);
            return 0;

          default:
            break;
        }

        return ::DefWindowProcW(hwnd, message, wParam, lParam);
      }

      static void paint(HWND hwnd) {
        PAINTSTRUCT paintInfo = {};
        HDC windowDc = ::BeginPaint(hwnd, &paintInfo);
        RECT client = {};
        ::GetClientRect(hwnd, &client);

        // Double buffer: the window repaints continuously while counting down
        // and drawing straight to the screen flickers.
        HDC dc = ::CreateCompatibleDC(windowDc);
        HBITMAP backBuffer = ::CreateCompatibleBitmap(
          windowDc, client.right, client.bottom);
        const HGDIOBJ previousBitmap = ::SelectObject(dc, backBuffer);

        HFONT titleFont = createTitleFont();
        HFONT bodyFont = createBodyFont();
        const Layout layout = computeLayout(dc, bodyFont, client.right);

        HBRUSH background = ::CreateSolidBrush(RGB(28, 30, 34));
        ::FillRect(dc, &client, background);
        ::DeleteObject(background);

        ::SetBkMode(dc, TRANSPARENT);

        const HGDIOBJ previousFont = ::SelectObject(dc, titleFont);
        ::SetTextColor(dc, RGB(245, 245, 245));
        RECT titleRect = layout.title;
        ::DrawTextW(dc, L"Please wait", -1, &titleRect,
          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        ::SelectObject(dc, bodyFont);
        ::SetTextColor(dc, RGB(205, 210, 218));
        RECT bodyRect = layout.body;
        ::DrawTextW(dc, kBodyText, -1, &bodyRect,
          DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

        const uint32_t pending = s_pending.load(std::memory_order_acquire);
        const uint32_t phaseMaximum = s_phaseMaxPending.load(std::memory_order_acquire);
        const uint32_t completedBase = s_completedBase.load(std::memory_order_acquire);
        const uint32_t phasePending = pending < phaseMaximum ? pending : phaseMaximum;
        const uint32_t total = completedBase + phaseMaximum;
        const uint32_t completed = completedBase + (phaseMaximum - phasePending);
        const uint64_t elapsedMs =
          ::GetTickCount64() - s_startMs.load(std::memory_order_acquire);
        const bool idle = s_activePhases.load(std::memory_order_acquire) == 0u;

        RECT phaseRect = layout.phase;
        if (idle && pending == 0u) {
          ::DrawTextW(dc, L"Shader compilation complete.", -1, &phaseRect,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        } else {
          wchar_t phaseLabel[kPhaseLabelCapacity] = {};
          {
            std::lock_guard<dxvk::mutex> lock(mutex());
            ::wcsncpy_s(phaseLabel, s_phaseLabel, _TRUNCATE);
          }
          ::DrawTextW(dc, phaseLabel, -1, &phaseRect,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        ::SetTextColor(dc, RGB(245, 245, 245));
        wchar_t status[160] = {};
        _snwprintf_s(status, _TRUNCATE,
          L"%u of %u shader pipelines compiled  |  %llu.%01llu seconds",
          completed, total,
          static_cast<unsigned long long>(elapsedMs / 1000u),
          static_cast<unsigned long long>((elapsedMs % 1000u) / 100u));
        RECT statusRect = layout.status;
        ::DrawTextW(dc, status, -1, &statusRect,
          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

        RECT barRect = layout.bar;
        HBRUSH barBackground = ::CreateSolidBrush(RGB(61, 65, 72));
        ::FillRect(dc, &barRect, barBackground);
        ::DeleteObject(barBackground);

        if (total > 0u) {
          RECT fill = barRect;
          fill.right = fill.left + static_cast<LONG>(
            (uint64_t(barRect.right - barRect.left) * completed) / total);
          HBRUSH progress = ::CreateSolidBrush(RGB(118, 185, 0));
          ::FillRect(dc, &fill, progress);
          ::DeleteObject(progress);
        }

        ::SelectObject(dc, previousFont);
        ::DeleteObject(titleFont);
        ::DeleteObject(bodyFont);

        ::BitBlt(windowDc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        ::SelectObject(dc, previousBitmap);
        ::DeleteObject(backBuffer);
        ::DeleteDC(dc);
        ::EndPaint(hwnd, &paintInfo);
      }

      static void runDialogThread() {
        static constexpr wchar_t kClassName[] =
          L"RtxRemixShaderPrewarmDialog";
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &ShaderPrewarmDialog::windowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32514));
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kClassName;
        ::RegisterClassExW(&windowClass);

        int clientHeight = 0;
        {
          HDC screenDc = ::GetDC(nullptr);
          HFONT bodyFont = createBodyFont();
          clientHeight = computeLayout(screenDc, bodyFont, kClientWidth).clientHeight;
          ::DeleteObject(bodyFont);
          ::ReleaseDC(nullptr, screenDc);
        }

        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        constexpr DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
        RECT windowRect = { 0, 0, kClientWidth, clientHeight };
        ::AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;
        const int x = (::GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        const int y = (::GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        const HWND hwnd = ::CreateWindowExW(
          exStyle,
          kClassName,
          L"RTX Remix Shader Compiler",
          style,
          x, y, width, height,
          nullptr, nullptr, instance, nullptr);

        s_hwnd.store(hwnd, std::memory_order_release);
        s_windowReady.store(true, std::memory_order_release);
        if (hwnd == nullptr)
          return;

        ::ShowWindow(hwnd, SW_SHOWNORMAL);
        ::UpdateWindow(hwnd);
        ::SetTimer(hwnd, kIdleTimerId, kIdleTimerPeriodMs, nullptr);

        MSG message = {};
        while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
          ::TranslateMessage(&message);
          ::DispatchMessageW(&message);
        }
      }

      static dxvk::mutex& mutex() {
        // Intentionally leaked: the detached window thread can outlive static
        // destructors at process exit and must never touch a destroyed lock.
        static dxvk::mutex* s_mutex = new dxvk::mutex();
        return *s_mutex;
      }

      // All state is static and trivially destructible: the window belongs to
      // the process, not to any single prewarm phase or DXVK device.
      inline static std::atomic<HWND> s_hwnd { nullptr };
      inline static std::atomic<bool> s_windowReady { false };
      inline static std::atomic<uint32_t> s_activePhases { 0u };
      inline static std::atomic<uint32_t> s_pending { 0u };
      inline static std::atomic<uint32_t> s_phaseMaxPending { 0u };
      inline static std::atomic<uint32_t> s_completedBase { 0u };
      inline static std::atomic<uint64_t> s_startMs { 0u };
      inline static std::atomic<uint64_t> s_idleSinceMs { 0u };
      inline static std::atomic<uint64_t> s_lastRedrawMs { 0u };
      inline static bool s_threadLaunched = false;                  // guarded by mutex()
      inline static wchar_t s_phaseLabel[kPhaseLabelCapacity] = {}; // guarded by mutex()
    };

    // DX11_V298_ONE_PREWARM_WINDOW_PER_SESSION: launcher chains, overlay
    // hosts, and engine subprocesses that load this DLL and slip past the
    // helper-name filter each used to open their OWN prewarm window - the
    // "prewarmer opens too many instances" report. Only the first process in
    // the session to claim this named mutex ever shows the window; every
    // other process still compiles, just silently. The handle is kept for
    // the process lifetime on purpose (released automatically at exit).
    bool prewarmWindowOwnedByThisProcess() {
      static const bool owned = [] {
        const HANDLE mutex =
          ::CreateMutexW(nullptr, TRUE, L"Local\\RtxRemixPrewarmWindow");
        if (mutex == nullptr)
          return false;
        if (::GetLastError() == ERROR_ALREADY_EXISTS) {
          ::CloseHandle(mutex);
          return false;
        }
        return true;
      }();
      return owned;
    }

    // Scopes one compilation phase in the shared prewarm window; a phase that
    // runs with the dialog disabled becomes a no-op.
    class ShaderPrewarmDialogPhase {
    public:
      ShaderPrewarmDialogPhase(bool show, const wchar_t* phaseLabel, uint32_t pendingHint)
      : m_shown(show && prewarmWindowOwnedByThisProcess()) {
        if (m_shown)
          ShaderPrewarmDialog::beginPhase(phaseLabel, pendingHint);
      }

      ShaderPrewarmDialogPhase(const ShaderPrewarmDialogPhase&) = delete;
      ShaderPrewarmDialogPhase& operator=(const ShaderPrewarmDialogPhase&) = delete;

      ~ShaderPrewarmDialogPhase() {
        if (m_shown)
          ShaderPrewarmDialog::endPhase();
      }

      void update(uint32_t pending) {
        if (m_shown)
          ShaderPrewarmDialog::update(pending);
      }

    private:
      bool m_shown;
    };

    // DX11_V298_SILENT_PREWARMER: the prewarmer emits no log output by
    // default. Set DXVK_REMIX_PREWARM_LOG=1 to restore the progress and
    // diagnostic lines when debugging shader compilation.
    bool prewarmLoggingEnabled() {
      static const bool enabled =
        env::getEnvVar("DXVK_REMIX_PREWARM_LOG") == "1";
      return enabled;
    }
  }

  RtxInitializer::RtxInitializer(DxvkDevice* device)
  : CommonDeviceObject(device) { 
  }

  void RtxInitializer::initialize() {
    // DX11_V267_BOOT_MARKER: the very first line of Remix initialization at
    // game boot. Everything before the first pre-existing [init] line (RTXIO
    // init, texture-manager async start, graphics-preset device queries) ran
    // silently - a boot that froze or crashed in that window showed nothing
    // from the initializer at all. With this marker plus the step lines
    // below, a hung-boot log pinpoints the exact last-completed step.
    KENSHI_DIAGNOSTIC_INFO(str::format("[Remix-DX11][init] BEGIN RtxInitializer::initialize() game='",
      env::getExeName(), "' pid=", GetCurrentProcessId()));

    ShaderManager::getInstance()->setDevice(m_device);

#ifdef WITH_RTXIO
    if (RtxIo::enabled()) {
      KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] initializing RTXIO...");
      RtxIo::get().initialize(m_device);
    }
    // start async before starting asset loading
    KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] starting texture manager async worker...");
    DxvkObjects* pCommon = m_device->getCommon();
    pCommon->getTextureManager().startAsync();
#endif

    KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] applying graphics/upscaler/raytrace-mode presets...");

    // Initialize RTX settings presets
    // Todo: Improve this preset override functionality [REMIX-1482]
    // Currently this logic is very confusing and is intended to skip preset initialization from overriding options, but only results in weird behavior
    // when a termination frame is not set (due to running a test locally in a more open-ended way), or due to how the ultra preset is being used but
    // it is being treated more as a custom preset in practice (except it's not fully custom either due to other preset initialization happening in dxvk_imgui.cpp,
    // though to be fair this logic is not acutally invoked I think unless the Remix menu is opened, but it still shouldn't be split out like this especially if a user
    // is debugging tests and opens the menu only for all the graphics settings to change).
    // Additionally, skipping this logic skips the DLSS preset initialization which is also probably wrong (though the tests will have to explicitly ask for DLSS
    // to be disabled if this is changed).
    if (env::getEnvVar("DXVK_TERMINATE_APP_FRAME") == "" ||
        env::getEnvVar("DXVK_GRAPHICS_PRESET_TYPE") != "0") {
      const DxvkDeviceInfo& deviceInfo = m_device->adapter()->devicePropertiesExt();

      RtxOptions::updateUpscalerFromDlssPreset();
      RtxOptions::updateGraphicsPresets(m_device);
      RtxOptions::updateRaytraceModePresets(deviceInfo.core.properties.vendorID, deviceInfo.khrDeviceDriverProperties.driverID);
    } else {
      // Default, init to custom unless otherwise specified
      if (RtxOptions::graphicsPreset() == GraphicsPreset::Auto) {
        RtxOptions::graphicsPreset.setDeferred(GraphicsPreset::Custom);
      }

      // Need to initialize DLSS-RR settings in test cases.
      if (env::getEnvVar("DXVK_RAY_RECONSTRUCTION") != "0") {
        RtxOptions::updateLightingSetting(RtxOptions::graphicsPreset() == GraphicsPreset::Custom);
      }
    }

    // DX11_V228_ALBEDO_SELECTION: the generic albedo texture-selection reinforcement
    // (rtx.enableUnrealTextureFixes - boost strong-albedo mipmapped textures, demote scene/intermediate
    // surfaces) now defaults ON for ANY game/engine, so real textures are selected when they are
    // actually bound while genuinely untextured geometry remains untextured. Detect a few engines
    // from the exe name and enable their additional quirk fixes; log everything for diagnostics.
    // Explicit user/config still wins.
    {
      std::string exeLower = env::getExeName();
      for (char& ch : exeLower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      const bool unrealByExeName = exeLower.find("-shipping") != std::string::npos
                                || exeLower.find("unrealengine") != std::string::npos;

      // DX11_V319_UNREAL_LAYOUT_DETECTION: the exe-name test alone misses any
      // Unreal title that ships a renamed or launcher executable. Little
      // Nightmares II runs as "Little Nightmares II.exe" and was classified
      // engine=generic, so none of the Unreal-scoped fixes applied to it, while
      // SpongeBob (Pineapple-Win64-Shipping.exe) was detected purely by luck of
      // naming.
      //
      // The directory layout is the reliable signal and is the same for every
      // Unreal game: an "Engine" folder holding Binaries and/or Content sits at
      // the install root, with the executable either beside it or a few levels
      // down under <Project>/Binaries/Win64. Walk up from the executable looking
      // for it. This is a handful of directory probes, once, at init.
      bool unrealByLayout = false;
      try {
        std::error_code ec;
        std::filesystem::path dir =
          std::filesystem::path(env::getExePath()).parent_path();

        for (uint32_t level = 0; level < 5u && !dir.empty(); ++level) {
          const std::filesystem::path engineDir = dir / "Engine";
          if (std::filesystem::is_directory(engineDir, ec)
           && (std::filesystem::is_directory(engineDir / "Binaries", ec)
            || std::filesystem::is_directory(engineDir / "Content", ec))) {
            unrealByLayout = true;
            break;
          }
          const std::filesystem::path parent = dir.parent_path();
          if (parent == dir)
            break;
          dir = parent;
        }
      } catch (...) {
        // A probe failure just means "not detected"; never block startup for it.
      }

      const bool isUnreal = unrealByExeName || unrealByLayout;
      const bool isSource2 = exeLower.find("source2") != std::string::npos
                          || exeLower.find("dota2") != std::string::npos
                          || exeLower.find("hl_vr") != std::string::npos
                          || exeLower.find("csgo") != std::string::npos
                          || exeLower.find("cs2") != std::string::npos;
      if (isSource2 && !RtxOptions::enableSource2Fixes()) {
        RtxOptions::enableSource2Fixes.setDeferred(true);
      }
      // DX11_V298: the albedo texture-selection reinforcement is off by
      // default (it could promote the wrong texture on arbitrary engines) but
      // Unreal titles NEED it - without it linear-format normal maps outrank
      // real albedo in material selection ("normal maps take over"). Scope it
      // to detected Unreal executables; explicit user config still wins.
      if (isUnreal && !RtxOptions::enableUnrealTextureFixes()) {
        RtxOptions::enableUnrealTextureFixes.setDeferred(true);
      }

      const char* engine = isUnreal ? "Unreal" : (isSource2 ? "Source2" : "generic");
      KENSHI_DIAGNOSTIC_INFO(str::format("[Remix-DX11][init] game='", env::getExeName(), "' engine=", engine,
        " albedoTexFixes=", RtxOptions::enableUnrealTextureFixes() ? "on" : "off",
        " source2Fixes=", (RtxOptions::enableSource2Fixes() || isSource2) ? "on" : "off"));
    }

    // Configure shader manager to understand bindless layouts
    ShaderManager::getInstance()->addGlobalExtraLayout(pCommon->getSceneManager().getBindlessResourceManager().getGlobalBindlessTableLayout(BindlessResourceManager::Buffers));
    ShaderManager::getInstance()->addGlobalExtraLayout(pCommon->getSceneManager().getBindlessResourceManager().getGlobalBindlessTableLayout(BindlessResourceManager::Textures));
    ShaderManager::getInstance()->addGlobalExtraLayout(pCommon->getSceneManager().getBindlessResourceManager().getGlobalBindlessTableLayout(BindlessResourceManager::Samplers));

    // Need to promote all of the hardware support Options before prewarming shaders.
    KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] applying pending RtxOptions...");
    RtxOptionManager::applyPendingValues(m_device, /* forceOnChange */ true);

    // Kick off shader prewarming
    //
    // DX11_V319_VISIBLE_PREWARM_STEP: these two lines were behind
    // DXVK_REMIX_PREWARM_LOG, so a boot that spent minutes here left NOTHING
    // between "applying pending RtxOptions..." and "loading assets..." - the
    // only evidence that Call of Duty Advanced Warfare and Saints Row IV were
    // stuck in prewarm was a silent multi-minute gap between two timestamps.
    // They fire once per boot, so they cost nothing and they name the step.
    KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] starting shader prewarm...");
    const bool prewarmStarted = startPrewarmShaders();

    if (prewarmStarted && RtxOptions::Shader::waitForPrewarmOnBoot()) {
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11][init] waiting for boot shader prewarm before game initialization"
        " (up to ", RtxOptions::Shader::maxBootPrewarmWaitSeconds(),
        "s, then it continues in the background)..."));
      waitForShaderPrewarm(RtxOptions::Shader::showPrewarmDialog(),
                           /* allowBackgroundHandoff */ true);
    }

    // Load assets (if any) as early as possible
    KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] loading assets...");
    if (RtxOptions::asyncAssetLoading()) {
      // Async asset loading (USD)
      m_asyncAssetLoadThread = dxvk::thread([this] {
        env::setThreadName("rtx-initialize-assets");
        loadAssets();
      });
    } else {
      loadAssets();
    }

    // DX11_V227: The DLSS / DLSS Frame Generation lazy allocators are load-bearing
    // for the present + frame-generation path, so they MUST be constructed here on
    // every GPU. (A previous attempt to skip them on non-NGX GPUs broke device/
    // swapchain initialization across all vendors with error 0x00000008 - reverted.)
    KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] initializing DLSS + DLFG lazy allocators...");
    pCommon->metaDLSS(); // Lazy allocator triggers init in ctor
    pCommon->metaDLFG();

    if (!asyncShaderFinalizing()) {
      // Wait for all prewarming to complete before calling "RTX initialized"
      if (prewarmLoggingEnabled())
        KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] waiting for shader prewarm...");
      waitForShaderPrewarm(false);
    }

    // DX11_V296_BACKGROUND_COMPILE_CAP: from here on the game is running, so
    // Remix pipeline compiles (background boot prewarm and any later first-use
    // compile) are capped to a few concurrent builds. Uncapped, every compiler
    // worker chewed on a multi-second ray-tracing pipeline at once and the
    // driver's internal compiler pool saturated the CPU - the reported
    // "lagging games after the prewarmer". Blocking boot prewarm above and the
    // exit drain in waitForShaderPrewarm() run before/after this cap applies.
    {
      uint32_t concurrency = RtxOptions::Shader::backgroundCompilerConcurrency();
      if (concurrency == 0u) {
        const uint32_t cpuThreads = std::max(1u, dxvk::thread::hardware_concurrency());
        concurrency = std::clamp(cpuThreads / 4u, 1u, 4u);
      }
      pCommon->pipelineManager().setRemixCompileConcurrency(concurrency);
      if (prewarmLoggingEnabled()) {
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[Remix-DX11][init] background Remix compile concurrency capped at ",
          concurrency, " (rtx.shader.backgroundCompilerConcurrency=",
          RtxOptions::Shader::backgroundCompilerConcurrency(), ", 0=auto)."));
      }
    }

    // DX11_V296_NONBLOCKING_PREWARM_WINDOW: when boot prewarm runs in the
    // background, the old blocking wait was the only code path that showed the
    // progress window - so V295's non-blocking default silently compiled ~300
    // pipelines with no window at all ("games are grabbing shaders without the
    // window"). Drive the same shared window from a monitor thread instead:
    // progress stays visible, the game keeps running.
    if (prewarmStarted && !m_warmupComplete) {
      startBackgroundPrewarmMonitor();
    }

    KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] RtxInitializer::initialize() complete.");
  }

  void RtxInitializer::release() {
    if (asyncShaderFinalizing()) {
      // Wait for all prewarming to complete 
      waitForShaderPrewarm();
    }

    ShaderManager::destroyInstance();
#ifdef WITH_RTXIO
    RtxIo::get().release();
#endif
  }

  void RtxInitializer::loadAssets() {
    m_assetsLoaded = false;

    Rc<DxvkContext> ctx = m_device->createContext();

    ctx->beginRecording(m_device->createCommandList());

    DxvkObjects* pCommon = m_device->getCommon();
    pCommon->getSceneManager().initialize(ctx);

    ctx->flushCommandList();

    m_assetsLoaded = true;
  }

  bool RtxInitializer::startPrewarmShaders() {
    // If we want to run without shader prewarming, then pipelines will be built inline with other GPU work on first use (typically means
    // long stutters whenever a yet to be compiled pipeline comes into use).
    // DX11_V228_CROSS_VENDOR: bulk RT-pipeline shader prewarming crashes/deadlocks at LAUNCH across
    // every GPU vendor in this DX11 fork - AMD: long-standing deadlock (original WAR below); Intel Arc
    // (Battlemage B580) AND NVIDIA: launch crash inside startPrewarmShaders(), confirmed via the
    // [Remix-DX11][init] markers (the init log stops right after "starting shader prewarm..." with no
    // further step). Since all three IHVs fail here, disable prewarming unconditionally; the RT
    // pipelines then compile inline on first use (minor first-use stutter, but the game boots and
    // path tracing runs on any GPU). Re-evaluate per-vendor once the prewarm path is fixed.
    // DX11_V245_NVIDIA_PREWARM: the earlier blanket disable lumped NVIDIA in with the
    // vendors whose prewarm genuinely fails - AMD (long-standing deadlock) and Intel Arc
    // (Battlemage launch crash). NVIDIA prewarms correctly, and it NEEDS prewarm: without
    // it the large RGS ray-tracing pipelines (NVIDIA's default indirect-integrate path)
    // compile INLINE on the first ray-traced frame, causing long stutters and a first-frame
    // crash risk (matches the RTX 4060 / Minecraft crash that lands right at the first RT
    // frame). So prewarm on NVIDIA and keep it disabled on AMD/Intel. Escape hatch:
    // DXVK_REMIX_PREWARM = "0" forces off, "1" forces on, on any vendor.
    // DX11_V275_NO_PREWARM_BY_DEFAULT: prewarm registration (below) can hang or
    // crash SYNCHRONOUSLY at launch - the init log stops right after "starting
    // shader prewarm" with no further step. This is the "game only stays in
    // Task Manager and never boots" / launch-freeze report, and it has now been
    // seen on all three vendors (AMD deadlock, Intel Arc crash, NVIDIA hang),
    // not just AMD/Intel. Since V248 made the RT pipelines compile ASYNC +
    // NON-BLOCKING on first use (they enqueue to the state-cache workers and
    // the frame simply skips RT until each pipeline is ready - no inline stall,
    // no first-frame crash, which V244 NRC-opt-in also addressed), prewarm is
    // no longer needed to avoid first-frame stutter. Default it OFF on EVERY
    // vendor so the game ALWAYS boots ("any game must work"); pipelines warm up
    // in the background and RT engages once they are ready. Re-enable for
    // benchmarking / prewarm testing with env DXVK_REMIX_PREWARM=1.
    const bool doPrewarm = RtxOptions::Shader::prewarmOnBoot();

    if (!asyncShaderPrewarming() || !doPrewarm) {
      if (prewarmLoggingEnabled())
        KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] shader prewarm disabled by configuration; pipelines will compile asynchronously on first use.");
      return false;
    }

    // Prewarm belongs to the real game process. Launchers named *launcher*
    // never reach here (DX11_V279 forwards them to system D3D11), but other
    // helper-style processes from the game folder (updaters, crash handlers,
    // overlay hosts) also create devices; each used to run its own prewarm
    // pass with its own progress window and log - the "multiple prewarmers"
    // report. Those processes never need the Remix pipeline set.
    static const bool isHelperProcess = [] {
      std::string executable = env::getExeName();
      for (char& c : executable)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      static const char* const kHelperKeywords[] = {
        "launcher", "updater", "bootstrap", "installer", "unins", "setup",
        "crashhandler", "crash_handler", "crashreport", "crashpad",
        "cef", "overlay", "redist", "eac", "battleye", "diagnostic",
        "helper", "watchdog",
      };
      for (const char* keyword : kHelperKeywords) {
        if (executable.find(keyword) != std::string::npos)
          return true;
      }
      return false;
    }();
    if (isHelperProcess
     && env::getEnvVar("DXVK_REMIX_FORCE_CURRENT_PROCESS") != "1") {
      if (prewarmLoggingEnabled())
        KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] helper/launcher-style process; skipping shader prewarm - the game process prewarms after launcher handoff (override: DXVK_REMIX_FORCE_CURRENT_PROCESS=1).");
      return false;
    }

    // DX11_V285_PREWARM_ONCE_PER_PROCESS: emulators and engines re-create the
    // device repeatedly (per-game boots, renderer restarts, adapter probes).
    // Every new DxvkDevice used to re-register the complete Remix pipeline
    // set: another full compile pass, another progress-log block every few
    // seconds, and more pipeline memory for work already done in this
    // process - the "multiple prewarm passes spamming the log and eating
    // RAM" report. Register once per process; a re-created device compiles
    // any pipeline it actually needs asynchronously on first use, served by
    // the state and driver caches, without blocking or a progress window.
    static std::atomic<bool> s_prewarmRegisteredThisProcess { false };
    if (s_prewarmRegisteredThisProcess.exchange(true)) {
      if (prewarmLoggingEnabled())
        KENSHI_DIAGNOSTIC_INFO("[Remix-DX11][init] shader prewarm already ran in this process; skipping re-registration for the re-created device.");
      return false;
    }
    if (prewarmLoggingEnabled()) {
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11][init] shader prewarm ENABLED in game process after launcher handoff (allVariants=",
        RtxOptions::Shader::prewarmAllVariants() ? 1 : 0,
        ", waitingBeforeGameInit=",
        RtxOptions::Shader::waitForPrewarmOnBoot() ? 1 : 0, ")."));
    }

    DxvkObjects* pCommon = m_device->getCommon();

    // Prewarm all the shaders we'll need for RT by registering them (per-pass) with the driver
    pCommon->metaPathtracerGbuffer().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaPathtracerIntegrateDirect().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaPathtracerIntegrateIndirect().prewarmShaders(pCommon->pipelineManager());

    pCommon->metaDebugView().prewarmShaders(pCommon->pipelineManager());

    // Prewarm the rest of the pipelines that can be done automatically
    AutoShaderPipelinePrewarmer::prewarmComputePipelines(pCommon->pipelineManager());

    if (prewarmLoggingEnabled()) {
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11][init] shader prewarm registration complete; pendingPipelines=",
        pCommon->pipelineManager().remixShaderCompilationCount()));
    }
    return true;
  }

  void RtxInitializer::waitForShaderPrewarm(bool showProgressDialog,
                                            bool allowBackgroundHandoff) {
    // DX11_V296_NONBLOCKING_PREWARM_WINDOW: the background monitor references
    // this device, so it must be stopped before any blocking drain or device
    // teardown proceeds. It reacts to the stop flag within a poll interval.
    m_stopPrewarmMonitor.store(true);
    if (m_prewarmMonitorThread.joinable()) {
      m_prewarmMonitorThread.join();
    }

    if (m_warmupComplete) {
      return;
    }

    // DX11_V296_BACKGROUND_COMPILE_CAP: this wait blocks the game (boot wait or
    // exit drain), so restore full compile parallelism for its duration - the
    // gameplay cap would otherwise stretch the drain out severalfold.
    m_device->getCommon()->pipelineManager().setRemixCompileConcurrency(0u);

    // Full-variant prewarming can legitimately take several minutes on an empty
    // driver cache. A fixed total timeout released the game halfway through that
    // work, contradicting the all-variants setting and moving the remaining
    // stalls into gameplay. Wait for the Remix count to reach zero as long as it
    // is making progress. A separate no-progress timeout still protects launch
    // from a genuinely wedged driver/compiler without penalizing slow machines.
    constexpr uint64_t kRegularPrewarmTimeoutMs = 120000;
    constexpr uint64_t kNoProgressTimeoutMs = 180000;
    constexpr uint64_t kProgressLogIntervalMs = 5000;

    // DX11_V319_BOUNDED_BOOT_PREWARM: with full-variant prewarming there was no
    // total limit at all - only the no-progress timeout - so the game stayed
    // blocked for as long as pipelines kept compiling. Measured on a cold
    // per-game cache: Call of Duty Advanced Warfare held here for 7m21s and
    // Saints Row IV for 8m02s, with no log line between entering and leaving
    // the step. From outside the process that is indistinguishable from a hang,
    // which is exactly how it was reported ("it never loads").
    //
    // Handing off to the background monitor after a bounded wait keeps the
    // intent of DX11_V298 - compile before use where possible, and persist the
    // result so later boots are seconds - without a first boot that looks dead.
    // The monitor already owns the same progress window, releases the finalizer
    // and saves the pipeline cache when it finishes, so the handoff needs only
    // to leave m_warmupComplete false and return.
    const uint64_t maxBootWaitMs =
      uint64_t(RtxOptions::Shader::maxBootPrewarmWaitSeconds()) * 1000ull;
    const bool boundedBootWait = allowBackgroundHandoff && maxBootWaitMs != 0ull;

    const uint64_t startMs = ::GetTickCount64();
    uint64_t lastProgressMs = startMs;
    uint64_t lastProgressLogMs = startMs;
    const bool fullVariantPrewarm = RtxOptions::Shader::prewarmAllVariants();
    auto& pipelineManager = m_device->getCommon()->pipelineManager();
    uint32_t pendingPipelines = pipelineManager.remixShaderCompilationCount();
    bool aborted = false;
    bool stalled = false;
    bool handedOff = false;
    ShaderPrewarmDialogPhase progressDialog(showProgressDialog,
      L"Compiling Remix path-tracing pipelines...", pendingPipelines);

    while (pendingPipelines > 0u) {
      const uint64_t nowMs = ::GetTickCount64();
      const uint64_t elapsedMs = nowMs - startMs;
      const uint32_t currentPending =
        pipelineManager.remixShaderCompilationCount();
      if (currentPending != pendingPipelines) {
        pendingPipelines = currentPending;
        lastProgressMs = nowMs;
      }

      progressDialog.update(pendingPipelines);

      if (nowMs - lastProgressLogMs >= kProgressLogIntervalMs) {
        if (prewarmLoggingEnabled()) {
          KENSHI_DIAGNOSTIC_INFO(str::format(
            "[Remix-DX11][init] shader prewarm progress: pendingPipelines=",
            pendingPipelines, " elapsedMs=", elapsedMs));
        }
        lastProgressLogMs = nowMs;
      }

      stalled = nowMs - lastProgressMs >= kNoProgressTimeoutMs;
      const bool regularTimeout = !fullVariantPrewarm
        && elapsedMs >= kRegularPrewarmTimeoutMs;
      if (stalled || regularTimeout) {
        aborted = true;
        break;
      }

      // Still compiling, still making progress, but the game has waited long
      // enough. Hand the rest to the background monitor and let it start.
      if (boundedBootWait && elapsedMs >= maxBootWaitMs) {
        handedOff = true;
        break;
      }

      Sleep(10);
    }

    if (handedOff) {
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11][init] boot shader prewarm handed off to the background after ",
        (::GetTickCount64() - startMs) / 1000ull, "s (",
        pendingPipelines, " pipelines still compiling, rtx.shader."
        "maxBootPrewarmWaitSeconds=",
        RtxOptions::Shader::maxBootPrewarmWaitSeconds(),
        "). The game starts now and the remaining pipelines finish while it runs; "
        "they are saved to the pipeline cache so the next launch skips this wait."));
      // Deliberately leaves m_warmupComplete false, does not release the
      // finalizer and does not save the cache: the background monitor started
      // by initialize() owns all three once it drains the remaining work.
      return;
    }

    // The shared prewarm window stays up for a moment after this phase ends
    // (the phase is released by the progressDialog destructor) so any
    // immediately following phase reuses it instead of opening a new window.
    // One line per boot: the outcome of a step that can cost minutes is worth
    // stating unconditionally, unlike the every-5s progress lines above.
    const uint64_t totalElapsedMs = ::GetTickCount64() - startMs;
    if (aborted) {
      Logger::err(str::format(
        "[Remix-DX11][init] shader prewarm aborted: reason=",
        stalled ? "no-progress-180s" : "regular-time-limit-120s",
        " remainingPipelines=", pendingPipelines,
        " elapsedMs=", totalElapsedMs,
        "; continuing launch so a driver compiler failure cannot hang the game."));
    } else {
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11][init] shader prewarm complete: pendingPipelines=0 elapsedMs=",
        totalElapsedMs));
    }

    DxvkRaytracingPipeline::releaseFinalizer();

    // DX11_V298_PERSISTENT_PIPELINE_CACHE: everything the prewarm compiled is
    // now in the shared Vulkan pipeline cache - serialize it so the next
    // launch reuses these binaries instead of recompiling them.
    m_device->getCommon()->pipelineManager().savePipelineCache();

    m_warmupComplete = true;
  }

  void RtxInitializer::startBackgroundPrewarmMonitor() {
    m_stopPrewarmMonitor.store(false);
    m_prewarmMonitorThread = dxvk::thread([this] {
      env::setThreadName("rtx-prewarm-monitor");

      constexpr uint64_t kNoProgressTimeoutMs = 180000;
      constexpr uint64_t kProgressLogIntervalMs = 5000;
      constexpr uint32_t kPollIntervalMs = 250;

      auto& pipelineManager = m_device->getCommon()->pipelineManager();
      uint32_t pendingPipelines = pipelineManager.remixShaderCompilationCount();
      if (pendingPipelines == 0u) {
        // Compilation can finish between the boot handoff and this thread starting.
        DxvkRaytracingPipeline::releaseFinalizer();
        pipelineManager.savePipelineCache();
        m_warmupComplete = true;
        return;
      }

      const uint64_t startMs = ::GetTickCount64();
      uint64_t lastProgressMs = startMs;
      uint64_t lastProgressLogMs = startMs;
      uint64_t lastCheckpointMs = startMs;
      uint32_t checkpointPending = pendingPipelines;
      bool stalled = false;

      // Same shared window as the blocking boot wait; the game keeps rendering
      // while this thread keeps the count and progress bar live.
      ShaderPrewarmDialogPhase progressDialog(
        RtxOptions::Shader::showPrewarmDialog(),
        L"Compiling Remix pipelines in the background - the game stays playable...",
        pendingPipelines);

      while (!m_stopPrewarmMonitor.load()) {
        const uint64_t nowMs = ::GetTickCount64();
        const uint32_t currentPending =
          pipelineManager.remixShaderCompilationCount();
        if (currentPending != pendingPipelines) {
          pendingPipelines = currentPending;
          lastProgressMs = nowMs;
          stalled = false;
        }

        progressDialog.update(pendingPipelines);

        if (pendingPipelines == 0u) {
          break;
        }

        if (nowMs - lastProgressLogMs >= kProgressLogIntervalMs) {
          if (prewarmLoggingEnabled()) {
            KENSHI_DIAGNOSTIC_INFO(str::format(
              "[Remix-DX11][init] background shader prewarm progress: pendingPipelines=",
              pendingPipelines, " elapsedMs=", nowMs - startMs));
          }
          lastProgressLogMs = nowMs;
        }

        // Keep already-finished work across interrupted launches. This is the
        // existing DX11 prewarm monitor, not a new renderer/worker-thread hook.
        if (nowMs - lastCheckpointMs >= 60000 && pendingPipelines != checkpointPending) {
          pipelineManager.savePipelineCache();
          checkpointPending = pendingPipelines;
          lastCheckpointMs = nowMs;
        }

        if (!stalled && nowMs - lastProgressMs >= kNoProgressTimeoutMs) {
          stalled = true;
          Logger::warn(str::format("[prewarm-v664] slow compiler: no completion for180s, pending=",
            pendingPipelines, "; continuing to monitor and preserve the cache."));
        }

        ::Sleep(kPollIntervalMs);
      }

      const uint64_t totalElapsedMs = ::GetTickCount64() - startMs;
      if (pendingPipelines == 0u) {
        Logger::info(str::format(
          "[prewarm-v664] background complete: pendingPipelines=0 elapsedMs=", totalElapsedMs));
        DxvkRaytracingPipeline::releaseFinalizer();
        // DX11_V298_PERSISTENT_PIPELINE_CACHE: persist the freshly compiled
        // pipelines for the next launch.
        m_device->getCommon()->pipelineManager().savePipelineCache();
        m_warmupComplete = true;
      } else {
        pipelineManager.savePipelineCache();
      }
      // A stop request (device teardown / blocking drain) just ends the
      // monitor; waitForShaderPrewarm owns completion from there.
    });
  }

  void RtxInitializer::runBootShaderScanPhase(const std::function<void()>& scan) {
    if (!scan)
      return;
    // Building the cache from game data can take minutes on a first boot;
    // keep the shared prewarm window up (with a scan label) for its duration
    // so the user sees the work instead of a frozen, windowless launch.
    ShaderPrewarmDialogPhase progressDialog(
      RtxOptions::Shader::showPrewarmDialog(),
      L"Building shader cache from game data (first launch only)...", 1u);
    scan();
  }

  void RtxInitializer::prewarmCachedGameShaders(
      uint32_t cachedShaderCount,
      const GameShaderRegistrar& registerShaders) {
    if (cachedShaderCount == 0u || !registerShaders)
      return;

    constexpr uint64_t kNoProgressTimeoutMs = 180000;
    constexpr uint64_t kProgressLogIntervalMs = 5000;
    const uint64_t startMs = ::GetTickCount64();
    ShaderPrewarmDialogPhase progressDialog(
      RtxOptions::Shader::showPrewarmDialog(),
      L"Compiling cached game shaders...", cachedShaderCount);

    if (prewarmLoggingEnabled()) {
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11][game-shader-cache] pre-init preload started: cachedShaders=",
        cachedShaderCount));
    }

    registerShaders([&](uint32_t remainingShaders) {
      progressDialog.update(remainingShaders);
    });

    // DX11_V298_GAME_SHADER_WAIT_SCOPE: this wait runs inside D3D11 device
    // creation, before the game can even show its window. The old loop waited
    // on the TOTAL compile count, which includes the multi-minute Remix
    // ray-tracing pipeline prewarm running in the background - Fallout 4 sat
    // windowless for ~6.5 minutes on an RTX 5060 because of it. Wait only on
    // the GAME shader pipelines this preload actually registered; the Remix
    // prewarm keeps compiling in the background after the game is up.
    auto& pipelineManager = m_device->getCommon()->pipelineManager();
    auto gamePendingPipelines = [&pipelineManager]() -> uint32_t {
      const uint32_t total = pipelineManager.shaderCompilationCount();
      const uint32_t remix = pipelineManager.remixShaderCompilationCount();
      // The two counters decrement non-atomically with respect to each other;
      // clamp the transient case where remix momentarily exceeds total.
      return total > remix ? total - remix : 0u;
    };

    uint32_t pendingPipelines = gamePendingPipelines();
    uint64_t lastProgressMs = ::GetTickCount64();
    uint64_t lastProgressLogMs = lastProgressMs;
    bool stalled = false;

    progressDialog.update(pendingPipelines);

    if (prewarmLoggingEnabled()) {
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Remix-DX11][game-shader-cache] cached shader registration complete; pendingPipelines=",
        pendingPipelines));
    }

    while (pendingPipelines > 0u) {
      const uint64_t nowMs = ::GetTickCount64();
      const uint64_t elapsedMs = nowMs - startMs;
      const uint32_t currentPending = gamePendingPipelines();
      if (currentPending != pendingPipelines) {
        pendingPipelines = currentPending;
        lastProgressMs = nowMs;
      }

      progressDialog.update(pendingPipelines);

      if (nowMs - lastProgressLogMs >= kProgressLogIntervalMs) {
        if (prewarmLoggingEnabled()) {
          KENSHI_DIAGNOSTIC_INFO(str::format(
            "[Remix-DX11][game-shader-cache] pipeline prewarm progress: pendingPipelines=",
            pendingPipelines, " elapsedMs=", elapsedMs));
        }
        lastProgressLogMs = nowMs;
      }

      if (nowMs - lastProgressMs >= kNoProgressTimeoutMs) {
        stalled = true;
        break;
      }
      ::Sleep(10);
    }

    const uint64_t elapsedMs = ::GetTickCount64() - startMs;
    if (prewarmLoggingEnabled()) {
      if (stalled) {
        Logger::err(str::format(
          "[Remix-DX11][game-shader-cache] pipeline prewarm stopped after no progress for 180 seconds; remainingPipelines=",
          pendingPipelines, " elapsedMs=", elapsedMs,
          "; continuing launch so a driver compiler failure cannot hang the game."));
      } else {
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[Remix-DX11][game-shader-cache] pre-init preload complete: pendingPipelines=0 elapsedMs=",
          elapsedMs));
      }
    }

    // DX11_V298_PERSISTENT_PIPELINE_CACHE: persist the game pipelines this
    // preload just compiled so the next launch loads them as binaries.
    pipelineManager.savePipelineCache();
  }
}
