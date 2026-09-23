/*
* Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
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
#include "vulkan_presenter.h"
#include "../util/util_kenshi_telemetry.h"
#include "../dxvk/dxvk_scoped_annotation.h"

#include "../dxvk/dxvk_format.h"
#include "../util/util_monitor.h"
#include "../util/util_env.h"

#include <thread>     // DX11_V233: worker thread for deadlock-safe swapchain creation
#include <atomic>
#include <chrono>

// DX11_V238_D3D11_INTEROP_PRESENT: real-D3D11/DXGI present path for Intel (bypasses the broken Intel
// Vulkan WSI). Uses the REAL Microsoft d3d11/dxgi driver copies deployed into the game dir (the game-dir
// d3d11.dll/dxgi.dll are the Remix/DXVK DLLs and cannot present natively).
#include <d3d12.h>
#include <dxgi1_4.h>

namespace dxvk::vk {

  // ---- Intel D3D11-interop present: premise validation (increment 1) -------------------------------
  // Returns the directory of our own (Remix) d3d11.dll, where the real-driver copies are deployed.
  static std::wstring getRemixModuleDir() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&getRemixModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(self, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) slash[1] = 0;
    return std::wstring(path);
  }

  // ===== Interop present state: GDI blit of the RTX frame to the window ============================
  // The Intel Vulkan WSI (vkCreateSwapchainKHR for windowed surfaces) deadlocks/runs away inside the
  // driver, so we bypass it entirely. Remix/Vulkan renders the RTX frame into a device-local VkImage;
  // we read it back into a HOST_VISIBLE buffer (vkCmdCopyImageToBuffer) and blit that to the window via
  // GDI StretchDIBits. This is intentionally NOT a D3D12/DXGI swapchain on the HWND: that approach pulls
  // the real d3d11.dll into the present path for DWM composition and crashes on swapchain recreate
  // (resolution/fullscreen changes) - which every game does. GDI blit has no swapchain, no DXGI/d3d11
  // dependency, no HWND-association churn, and works on ANY GPU/driver/window. Correctness + robustness
  // first; a zero-copy GPU path can replace it later without changing the Presenter contract.
  struct PresenterInterop {
    HWND                          window = nullptr;
    uint32_t                      w = 0, h = 0;
    std::vector<VkDeviceMemory>   vkMem;      // memory backing the raw present images (we own it)

    // CPU-staging frame transfer: Remix renders the RTX frame into a device-local VkImage; we copy it
    // into this HOST_VISIBLE buffer (vkCmdCopyImageToBuffer), then StretchDIBits it to the window DC.
    VkBuffer                      vkStaging    = VK_NULL_HANDLE;
    VkDeviceMemory                vkStagingMem = VK_NULL_HANDLE;
    uint8_t*                      vkStagingPtr = nullptr;     // persistently mapped
    VkDeviceSize                  vkStagingSize = 0;
    uint32_t                      vkRowBytes   = 0;           // tightly packed src pitch (w*4)
    VkCommandPool                 vkPool       = VK_NULL_HANDLE;
    VkCommandBuffer               vkCmd        = VK_NULL_HANDLE;
    VkFence                       vkFence      = VK_NULL_HANDLE;
  };

  static void teardownInterop(PresenterInterop* ip) {
    if (!ip) return;
    delete ip;
  }

  // Set up the interop present path on 'window' at w x h. GDI-based: no device creation can fail, so this
  // always succeeds (the heavy Vulkan resources are created by the caller in recreateSwapChain).
  static PresenterInterop* setupInterop(HWND window, uint32_t w, uint32_t h, uint32_t imageCount) {
    auto ip = std::make_unique<PresenterInterop>();
    ip->window = window; ip->w = w; ip->h = h;
    Logger::info(str::format("[Remix-DX11][interop] setup: GDI present ACTIVE (", w, "x", h,
      ") - Vulkan WSI bypassed, no D3D12/DXGI/d3d11 dependency"));
    return ip.release();
  }

  // Present one finished RTX frame: the frame has already been copied into ip->vkStagingPtr (tightly
  // packed B8G8R8A8 top-down, vkRowBytes per row) by the Vulkan side. Blit it to the window's client
  // area via GDI, scaling to the current client rect if the window was resized.
  static void presentInterop(PresenterInterop* ip) {
    if (!ip || !ip->window || !ip->vkStagingPtr) return;

    // DX11_V241 OUTPUT DIAGNOSTIC: sample the staged frame (the image Remix rendered the RTX output
    // into and we read back). If it's all-black, the composited RTX frame is NOT reaching our present
    // image (output-wiring bug) - that explains "raytracing renders black on every game/GPU". If it's
    // non-black, the RTX output is here and the problem is purely GDI display. Logged every 120 frames.
    {
      static uint32_t s_presentSampleCount = 0;
      if (kenshi_telemetry::enabled() && (s_presentSampleCount++ % 120) == 0) {
        uint32_t maxC = 0; uint64_t sum = 0; uint32_t nonzero = 0; const uint32_t kSamples = 64;
        for (uint32_t i = 0; i < kSamples; ++i) {
          const uint32_t sx = (ip->w * ((i * 37u) % 64u)) / 64u;
          const uint32_t sy = (ip->h * ((i * 53u) % 64u)) / 64u;
          const uint8_t* px = ip->vkStagingPtr + size_t(sy) * ip->vkRowBytes + size_t(sx) * 4u;
          const uint32_t b = px[0], g = px[1], r = px[2];
          maxC = std::max(maxC, std::max(r, std::max(g, b)));
          sum += uint64_t(r) + g + b;
          if (r | g | b) ++nonzero;
        }
        KENSHI_DIAGNOSTIC_INFO(str::format("[Remix-DX11][outdiag] present image sample: maxChannel=", maxC,
          " avg=", uint32_t(sum / (kSamples * 3)), " nonzeroOf64=", nonzero,
          (maxC == 0 ? " => BLACK (RTX output not reaching present image)" : " => has content (display path issue)")));
      }
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = LONG(ip->w);
    bmi.bmiHeader.biHeight = -LONG(ip->h);   // negative => top-down (matches our VkImage row order)
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;    // 32bpp BI_RGB is BGRA byte order == VK_FORMAT_B8G8R8A8_UNORM

    HDC hdc = ::GetDC(ip->window);
    if (!hdc) return;

    RECT rc = {};
    int dstW = int(ip->w), dstH = int(ip->h);
    if (::GetClientRect(ip->window, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
      dstW = rc.right - rc.left; dstH = rc.bottom - rc.top;
    }

    ::SetStretchBltMode(hdc, COLORONCOLOR);
    ::StretchDIBits(hdc,
      0, 0, dstW, dstH,                       // dest rect (window client area)
      0, 0, int(ip->w), int(ip->h),           // src rect (full frame)
      ip->vkStagingPtr, &bmi, DIB_RGB_COLORS, SRCCOPY);

    ::ReleaseDC(ip->window, hdc);
  }



  Presenter::Presenter(
          HWND            window,
    const Rc<InstanceFn>& vki,
    const Rc<DeviceFn>&   vkd,
          PresenterDevice device,
    const PresenterDesc&  desc)
  : m_vki(vki), m_vkd(vkd), m_device(device), m_window(window) {
    // As of Wine 5.9, winevulkan provides this extension, but does
    // not filter the pNext chain for VkSwapchainCreateInfoKHR properly
    // before passing it to the Linux sude, which breaks RenderDoc.
    if (m_device.features.fullScreenExclusive && ::GetModuleHandle("winevulkan.dll")) {
      Logger::warn("winevulkan detected, disabling exclusive fullscreen support");
      m_device.features.fullScreenExclusive = false;
    }

    // DX11_V237_INTEL_FORCE_FSE: FORCE exclusive fullscreen on Intel (per design decision). Live
    // captures proved the Intel Arc WINDOWED present path is broken both ways inside igvk64
    // (IMMEDIATE -> OpenGL ChoosePixelFormat stall; FIFO -> D3D12CreateDevice CPU-runaway), so a
    // windowed swapchain never renders. Real fullscreen-exclusive scans out directly with no GL/D3D12
    // windowed-blit interop, so it's the route that can actually present on this driver. We therefore
    // KEEP the FSE feature ENABLED on Intel here (do NOT disable it) and force application-controlled
    // FSE in D3D11SwapChain::PickFullscreenMode(); the V233 message pump services the FSE window
    // messaging that would otherwise deadlock.

    if (createSurface() != VK_SUCCESS)
      throw DxvkError("Failed to create surface");

    if (recreateSwapChain(desc) != VK_SUCCESS)
      throw DxvkError("Failed to create swap chain");
  }

  
  Presenter::~Presenter() {
    destroySwapchain();
    destroySurface();
  }


  PresenterInfo Presenter::info() const {
    return m_info;
  }


  PresenterImage Presenter::getImage(uint32_t index) const {
    return m_images.at(index);
  }

  //VkSemaphore Presenter::getCurrentPresentWaitSemaphore() const {
  //  return m_semaphores.at(m_frameIndex).present;
  //}

  VkResult Presenter::acquireNextImage(PresenterSync& sync, uint32_t& index,
                                       // NV-DXVK start: DLFG integration
                                       bool isDlfgPresenting
                                       // NV-DXVK end
                                       ) {
    ScopedCpuProfileZone();

    sync = m_semaphores.at(m_frameIndex);

    // DX11_V238 interop: no real WSI acquire; the present image is always available. Signal the acquire
    // semaphore (DXVK's render waits on it) via an empty submit so the render proceeds normally.
    if (m_interop) {
      index = m_imageIndex = m_frameIndex;
      VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
      si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sync.acquire;
      m_vkd->vkQueueSubmit(m_device.queue, 1, &si, VK_NULL_HANDLE);
      m_acquireStatus = VK_SUCCESS;
      return VK_SUCCESS;
    }

    // NV-DXVK start: DLFG integration
    if (isDlfgPresenting) {
      // DLFG manages swapchain images directly and can have more than one acquire outstanding at a time
      m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),
                                                     m_swapchain,
                                                     std::numeric_limits<uint64_t>::max(),
                                                     sync.acquire,
                                                     VK_NULL_HANDLE,
                                                     &index);
      assert(m_acquireStatus != VK_NOT_READY);
    } else {
      // Don't acquire more than one image at a time
      if (m_acquireStatus == VK_NOT_READY) {
        m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),
          m_swapchain, std::numeric_limits<uint64_t>::max(),
          sync.acquire, VK_NULL_HANDLE, &m_imageIndex);
      }
    }

    if (m_acquireStatus == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
      acquireFullscreenExclusive();

    if (m_acquireStatus != VK_SUCCESS && m_acquireStatus != VK_SUBOPTIMAL_KHR)
      return m_acquireStatus;

    if (!isDlfgPresenting) {
      index = m_imageIndex;
    }

    return m_acquireStatus;
  }

  VkResult Presenter::presentImage(
    // NV-DXVK start: DLFG integration
    std::atomic<VkResult>*,
    const DxvkPresentInfo&,
    const DxvkFrameInterpolationInfo&,
    std::uint32_t imageIndex,
    bool isDlfgPresenting,
    VkSetPresentConfigNV* presentMetering
    // NV-DXVK end
  ) {
    ScopedCpuProfileZone();
    // NV-DXVK start: DLFG integration
    PresenterSync sync;

    sync = m_semaphores.at(m_frameIndex);
    // NV-DXVK end

    // DX11_V238 interop: present the finished RTX frame via real D3D12 (Vulkan WSI bypassed on Intel).
    if (m_interop) {
      auto* ip = reinterpret_cast<PresenterInterop*>(m_interop);
      // Consume the present semaphore DXVK signaled after rendering into m_images[m_imageIndex], copy that
      // image into the host-visible staging buffer, then display it via D3D12. The image-copy submit waits
      // on sync.present, so the readback only runs once the RTX frame is finished.
      VkImage frame = m_images[m_imageIndex].image;
      VkCommandBuffer cmd = ip->vkCmd;
      VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      m_vkd->vkResetCommandBuffer(cmd, 0);
      m_vkd->vkBeginCommandBuffer(cmd, &bi);

      // PRESENT_SRC (Remix's blit target) -> TRANSFER_SRC for the readback copy.
      VkImageMemoryBarrier toSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
      toSrc.srcAccessMask = 0; toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      toSrc.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toSrc.image = frame; toSrc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      m_vkd->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toSrc);

      VkBufferImageCopy region = {};
      region.bufferOffset = 0; region.bufferRowLength = 0; region.bufferImageHeight = 0;
      region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      region.imageOffset = { 0, 0, 0 }; region.imageExtent = { m_info.imageExtent.width, m_info.imageExtent.height, 1 };
      m_vkd->vkCmdCopyImageToBuffer(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ip->vkStaging, 1, &region);

      // restore layout so Remix can render into the image again next frame
      VkImageMemoryBarrier toPresent = toSrc;
      toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; toPresent.dstAccessMask = 0;
      toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      m_vkd->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toPresent);
      m_vkd->vkEndCommandBuffer(cmd);

      VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
      si.waitSemaphoreCount = 1; si.pWaitSemaphores = &sync.present; si.pWaitDstStageMask = &waitStage;
      si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
      m_vkd->vkResetFences(m_vkd->device(), 1, &ip->vkFence);
      const VkResult submitRes = m_vkd->vkQueueSubmit(m_device.queue, 1, &si, ip->vkFence);

      // DX11_V247_NO_INFINITE_WAITS: this used to be vkWaitForFences(..., UINT64_MAX).
      // When the GPU hangs under the RT workload (TDR / VK_ERROR_DEVICE_LOST - observed
      // at the end of real game logs), an unbounded wait freezes the render thread
      // forever; the game's main thread is typically blocked on Present -> the whole
      // process turns into a black, non-responding window. Bound the wait and handle
      // failure: timeout skips this present (frame indices still advance so the loop
      // cannot wedge), device-lost propagates so DXVK/DXGI report device removal
      // instead of hanging.
      VkResult waitRes = submitRes;
      if (submitRes == VK_SUCCESS) {
        constexpr uint64_t kReadbackFenceTimeoutNs = 2'000'000'000ull; // 2 s ≈ TDR window
        waitRes = m_vkd->vkWaitForFences(m_vkd->device(), 1, &ip->vkFence, VK_TRUE, kReadbackFenceTimeoutNs);
      }

      if (waitRes == VK_SUCCESS) {
        presentInterop(ip);
      } else {
        static uint32_t s_interopWaitFailLog = 0;
        if (s_interopWaitFailLog < 16) {
          ++s_interopWaitFailLog;
          Logger::err(str::format("[Remix-DX11][interop] present readback ",
            submitRes != VK_SUCCESS ? "submit" : "fence wait",
            " failed: ", waitRes, " - skipping this present"));
        }
      }

      m_frameIndex = (m_frameIndex + 1) % std::max<size_t>(m_semaphores.size(), size_t(1));
      m_imageIndex = m_frameIndex;
      m_fpsLimiter.delay(true);

      // Timeout: the GPU may still be executing (heavy frame) - report success and
      // let the next frame retry; if the device is truly lost the driver will fail
      // a later call with DEVICE_LOST which then propagates. Real errors propagate
      // now so DXVK's device-loss path runs instead of an eternal black hang.
      if (waitRes == VK_TIMEOUT)
        return VK_SUCCESS;
      return waitRes;
    }

    VkPresentInfoKHR info;
    info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.pNext              = presentMetering;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores    = &sync.present;
    info.swapchainCount     = 1;
    info.pSwapchains        = &m_swapchain;
    // NV-DXVK start: DLFG integration
    if (isDlfgPresenting) {
      info.pImageIndices = &imageIndex;
    } else {
      info.pImageIndices = &m_imageIndex;
    }
    // NV-DXVK end
    info.pResults           = nullptr;

    VkResult status = m_vkd->vkQueuePresentKHR(m_device.queue, &info);

    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
      return status;

    // NV-DXVK start: App Controlled FSE
    if (status == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
      acquireFullscreenExclusive();
    // NV-DXVK end

    // NV-DXVK start: DLFG integration
    if (!isDlfgPresenting) {
    // NV-DXVK end
      // Try to acquire next image already, in order to hide
      // potential delays from the application thread.
      m_frameIndex += 1;
      m_frameIndex %= m_semaphores.size();

      sync = m_semaphores.at(m_frameIndex);

      m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),
        m_swapchain, std::numeric_limits<uint64_t>::max(),
        sync.acquire, VK_NULL_HANDLE, &m_imageIndex);
    }

    bool vsync = m_info.presentMode == VK_PRESENT_MODE_FIFO_KHR
              || m_info.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;

    m_fpsLimiter.delay(vsync);
    return status;
  }

  
  VkResult Presenter::recreateSwapChain(const PresenterDesc& desc) {
    if (m_swapchain)
      destroySwapchain();

    // True when this invocation is the one-shot GDI fallback re-entry (see the
    // native-WSI failure path below). Bounds the fallback to a single retry so a
    // double failure can never recurse forever.
    const bool inGdiFallbackAttempt = m_gdiFallback;

    // DX11_V237_FORCE_FOREGROUND_FOR_FSE: bring the swapchain window visible + foreground before
    // creating the swapchain. On Intel we force FSE (ALLOWED) and the driver only acquires exclusive
    // fullscreen DURING vkCreateSwapchainKHR when the window is foreground and covers the output - so we
    // must surface it first, otherwise the driver falls back to the broken windowed (GL/D3D12) path and
    // stalls. Also restores a minimized window on every GPU. Runs on the window-owning thread.
    if (m_window) {
      if (::IsIconic(m_window))
        ::ShowWindow(m_window, SW_RESTORE);

      // DX11_V242_GDI_INTEROP_NOT_ALL_GPUS: the GDI-interop present (readback + StretchDIBits) was
      // force-enabled for EVERY GPU to dodge the Intel Arc Vulkan-WSI deadlock. But that blit copies the
      // finished RTX frame into a host buffer and then fails to reach the monitor on real game windows -
      // the "[outdiag] ... has content (display path issue)" black screen: RTX renders fine, the display
      // path drops it. The standard Vulkan WSI present path below is DWM-composited and displays correctly
      // on every vendor, and ONLY Intel's windowed WSI ever deadlocked. So use GDI interop by default only
      // on Intel; NVIDIA/AMD take the normal present path (fixes their black screen with zero deadlock
      // risk, since they never had the WSI bug). Override with DXVK_REMIX_GDI_PRESENT: "1" forces GDI on
      // any GPU, "0" forces the normal WSI present on any GPU (e.g. to test a newer Intel driver where the
      // WSI deadlock is fixed - this machine is on 109.634.0, far newer than the 103.108.0 that hung).
      bool useGdiInterop = false;
      {
        VkPhysicalDeviceProperties adapterProps = {};
        m_vki->vkGetPhysicalDeviceProperties(m_device.adapter, &adapterProps);
        useGdiInterop = (adapterProps.vendorID == 0x8086); // Intel: keep the deadlock-safe workaround
        const std::string forcedPresent = env::getEnvVar("DXVK_REMIX_GDI_PRESENT");
        if (forcedPresent == "1")
          useGdiInterop = true;
        else if (forcedPresent == "0")
          useGdiInterop = false;
        // A prior native-WSI attempt in this call chain asked to fall back to GDI
        // (e.g. the window is already owned by another swapchain). Honor it once.
        if (m_gdiFallback) {
          useGdiInterop = true;
          m_gdiFallback = false;
        }
        Logger::info(str::format("[Remix-DX11][interop] present path: ",
          useGdiInterop ? "GDI blit" : "native Vulkan WSI",
          " (vendorID=0x", std::hex, adapterProps.vendorID, std::dec,
          forcedPresent.empty() ? "" : ", DXVK_REMIX_GDI_PRESENT override applied", ")"));
      }

      // INTEROP PRESENT - ALL GPUs (NVIDIA/AMD/Intel). The Intel windowed Vulkan WSI is broken
      // (vkCreateSwapchainKHR deadlocks/runs away), and rather than maintain two divergent present
      // paths we use ONE robust path on every vendor: Remix/Vulkan renders the RTX frame into raw
      // device-local present images, we read the finished frame back and blit it to the window via GDI
      // (see presentInterop). DXVK already normalizes the Vulkan rendering across vendors, so this is
      // uniform and avoids the vendor-specific WSI bugs entirely. Skip vkCreateSwapchainKHR.
      // NOTE: do NOT call SetForegroundWindow/ShowWindow here - recreateSwapChain can run on the render
      // thread (via D3D11SwapChain::Present -> RecreateSwapChain), and those APIs SendMessage to the
      // window-owning main thread which is blocked waiting on the render thread -> deadlock.
      if (useGdiInterop) {
        const uint32_t iw = std::max<uint32_t>(desc.imageExtent.width, 1u);
        const uint32_t ih = std::max<uint32_t>(desc.imageExtent.height, 1u);
        const uint32_t imageCount = 3;

        if (!m_interop)
          m_interop = setupInterop(m_window, iw, ih, imageCount);

        if (m_interop) {
          m_info.format       = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
          m_info.imageExtent  = { iw, ih };
          m_info.imageCount   = imageCount;
          m_info.presentMode  = VK_PRESENT_MODE_FIFO_KHR;
          m_info.appOwnedFSE  = false;

          VkPhysicalDeviceMemoryProperties memProps;
          m_vki->vkGetPhysicalDeviceMemoryProperties(m_device.adapter, &memProps);
          auto deviceLocalType = [&](uint32_t typeBits) -> uint32_t {
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
              if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
            return 0;
          };

          m_images.resize(imageCount);
          for (uint32_t i = 0; i < imageCount; ++i) {
            VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ici.imageType = VK_IMAGE_TYPE_2D; ici.format = m_info.format.format;
            ici.extent = { iw, ih, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                      | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImage img = VK_NULL_HANDLE;
            if (m_vkd->vkCreateImage(m_vkd->device(), &ici, nullptr, &img) != VK_SUCCESS) {
              Logger::err("[Remix-DX11][interop] vkCreateImage failed"); return VK_ERROR_INITIALIZATION_FAILED;
            }
            VkMemoryRequirements req; m_vkd->vkGetImageMemoryRequirements(m_vkd->device(), img, &req);
            VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size; mai.memoryTypeIndex = deviceLocalType(req.memoryTypeBits);
            VkDeviceMemory mem = VK_NULL_HANDLE;
            m_vkd->vkAllocateMemory(m_vkd->device(), &mai, nullptr, &mem);
            m_vkd->vkBindImageMemory(m_vkd->device(), img, mem, 0);
            reinterpret_cast<PresenterInterop*>(m_interop)->vkMem.push_back(mem);
            m_images[i].image = img;
            VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = m_info.format.format;
            vci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            m_vkd->vkCreateImageView(m_vkd->device(), &vci, nullptr, &m_images[i].view);
          }

          m_semaphores.resize(imageCount);
          for (uint32_t i = 0; i < imageCount; ++i) {
            VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            m_vkd->vkCreateSemaphore(m_vkd->device(), &sci, nullptr, &m_semaphores[i].acquire);
            m_vkd->vkCreateSemaphore(m_vkd->device(), &sci, nullptr, &m_semaphores[i].present);
          }

          // HOST_VISIBLE staging buffer + command resources for the per-frame VkImage->buffer readback.
          {
            auto* ip = reinterpret_cast<PresenterInterop*>(m_interop);
            auto hostVisibleType = [&](uint32_t typeBits) -> uint32_t {
              const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
              for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
                if ((typeBits & (1u << i)) && ((memProps.memoryTypes[i].propertyFlags & want) == want)) return i;
              return 0;
            };
            ip->vkRowBytes  = iw * 4u;
            ip->vkStagingSize = VkDeviceSize(ip->vkRowBytes) * ih;
            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = ip->vkStagingSize; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            m_vkd->vkCreateBuffer(m_vkd->device(), &bci, nullptr, &ip->vkStaging);
            VkMemoryRequirements breq; m_vkd->vkGetBufferMemoryRequirements(m_vkd->device(), ip->vkStaging, &breq);
            VkMemoryAllocateInfo bmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            bmai.allocationSize = breq.size; bmai.memoryTypeIndex = hostVisibleType(breq.memoryTypeBits);
            m_vkd->vkAllocateMemory(m_vkd->device(), &bmai, nullptr, &ip->vkStagingMem);
            m_vkd->vkBindBufferMemory(m_vkd->device(), ip->vkStaging, ip->vkStagingMem, 0);
            m_vkd->vkMapMemory(m_vkd->device(), ip->vkStagingMem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&ip->vkStagingPtr));

            VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = m_device.queueFamily;
            m_vkd->vkCreateCommandPool(m_vkd->device(), &pci, nullptr, &ip->vkPool);
            VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cbai.commandPool = ip->vkPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
            m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &cbai, &ip->vkCmd);
            VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            m_vkd->vkCreateFence(m_vkd->device(), &fci, nullptr, &ip->vkFence);
          }

          m_frameIndex = 0; m_imageIndex = 0; m_acquireStatus = VK_SUCCESS;
          Logger::info("[Remix-DX11][interop] PRESENT PATH ACTIVE (all GPUs): RTX renders in Vulkan, displays via GDI blit (no Vulkan WSI)");
          return VK_SUCCESS;
        }
        Logger::warn("[Remix-DX11][interop] setup failed - falling back to native Vulkan WSI");
      }
    }

    // Query surface capabilities. Some properties might
    // have changed, including the size limits and supported
    // present modes, so we'll just query everything again.
    VkSurfaceCapabilitiesKHR        caps;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR>   modes;

    VkResult status;
    
    if ((status = m_vki->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        m_device.adapter, m_surface, &caps)) != VK_SUCCESS) {
      if (status == VK_ERROR_SURFACE_LOST_KHR) {
        // Recreate the surface and try again.
        if (m_surface)
          destroySurface();
        if ((status = createSurface()) != VK_SUCCESS)
          return status;
        status = m_vki->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            m_device.adapter, m_surface, &caps);
      }
      if (status != VK_SUCCESS)
        return status;
    }

    if ((status = getSupportedFormats(formats, desc)) != VK_SUCCESS)
      return status;

    if ((status = getSupportedPresentModes(modes, desc)) != VK_SUCCESS)
      return status;

    // Select actual swap chain properties and create swap chain
    m_info.format       = pickFormat(formats.size(), formats.data(), desc.numFormats, desc.formats);
    m_info.presentMode  = pickPresentMode(modes.size(), modes.data(), desc.numPresentModes, desc.presentModes);
    m_info.imageExtent  = pickImageExtent(caps, desc.imageExtent);
    m_info.imageCount   = pickImageCount(caps, m_info.presentMode, desc.imageCount);

    // NV-DXVK start: App controlled FSE
    m_info.appOwnedFSE  = m_device.features.fullScreenExclusive && (desc.fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT);
    // NV-DXVK end

    if (!m_info.imageExtent.width || !m_info.imageExtent.height) {
      m_info.imageCount = 0;
      m_info.format     = { VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      return VK_SUCCESS;
    }

    // NV-DXVK start: App controlled FSE
    VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo;
    fullScreenInfo.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
    fullScreenInfo.pNext = nullptr;
    fullScreenInfo.fullScreenExclusive = desc.fullScreenExclusive;

    VkSurfaceFullScreenExclusiveWin32InfoEXT fullScreenInfoWin32;
    if (m_info.appOwnedFSE) {
      fullScreenInfoWin32.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT;
      fullScreenInfoWin32.pNext = nullptr;
      fullScreenInfoWin32.hmonitor = GetDefaultMonitor();
      fullScreenInfo.pNext = &fullScreenInfoWin32;
    }
    // NV-DXVK end

    VkSwapchainCreateInfoKHR swapInfo;
    swapInfo.sType                  = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.pNext                  = nullptr;
    swapInfo.flags                  = 0;
    swapInfo.surface                = m_surface;
    swapInfo.minImageCount          = m_info.imageCount;
    swapInfo.imageFormat            = m_info.format.format;
    swapInfo.imageColorSpace        = m_info.format.colorSpace;
    swapInfo.imageExtent            = m_info.imageExtent;
    swapInfo.imageArrayLayers       = 1;
    swapInfo.imageUsage             = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                    | VK_IMAGE_USAGE_TRANSFER_DST_BIT
    // NV-DXVK start: Add storage bit for Frameview because it runs computer shader
                                    | VK_IMAGE_USAGE_STORAGE_BIT;
    // NV-DXVK end
    swapInfo.imageSharingMode       = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.queueFamilyIndexCount  = 0;
    swapInfo.pQueueFamilyIndices    = nullptr;
    swapInfo.preTransform           = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapInfo.compositeAlpha         = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode            = m_info.presentMode;
    swapInfo.clipped                = VK_TRUE;
    swapInfo.oldSwapchain           = m_swapchain;

    if (m_device.features.fullScreenExclusive)
      swapInfo.pNext = &fullScreenInfo;

    // DX11_V233_WSI_SELFDEADLOCK_PUMP: cross-vendor swapchain-creation deadlock fix. On some drivers
    // (confirmed Intel Arc igvk64: its WSI calls OpenGL ChoosePixelFormat/wglChoosePixelFormat on an
    // internal helper thread, which SendMessage()s the swapchain HWND) vkCreateSwapchainKHR must have
    // the window's message pump serviced to complete. When the swapchain is (re)created ON the
    // window-OWNING thread (e.g. a Unity game creating a new swapchain on window restore), that thread
    // is blocked inside vkCreateSwapchainKHR and can never service the sent message -> permanent
    // self-deadlock (game frozen, audio continues; verified via two live cdb captures). Fix: when we
    // are on the window thread, run the create on a worker thread and keep THIS thread pumping SENT
    // messages until it completes, so the driver's cross-thread SendMessage is serviced. This is
    // vendor-agnostic and a no-op on NVIDIA/AMD (their create does not message the window, so the
    // worker returns immediately and we never wait on messages). Helps Intel + any IHV with the same
    // window-thread dependency. vkCreateSwapchainKHR is externally synchronised and m_swapchain is not
    // used concurrently during (re)creation, so running it on a worker is safe.
    auto createSwapchainDeadlockSafe = [&](VkSwapchainCreateInfoKHR& ci) -> VkResult {
      const bool onWindowThread = m_window != nullptr
        && ::GetWindowThreadProcessId(m_window, nullptr) == ::GetCurrentThreadId();
      if (!onWindowThread)
        return m_vkd->vkCreateSwapchainKHR(m_vkd->device(), &ci, nullptr, &m_swapchain);

      HANDLE doneEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
      VkResult workerStatus = VK_NOT_READY;
      std::thread worker([&] {
        workerStatus = m_vkd->vkCreateSwapchainKHR(m_vkd->device(), &ci, nullptr, &m_swapchain);
        if (doneEvent) ::SetEvent(doneEvent);
      });
      if (doneEvent) {
        for (;;) {
          const DWORD r = ::MsgWaitForMultipleObjectsEx(1, &doneEvent, INFINITE, QS_SENDMESSAGE, 0);
          if (r == WAIT_OBJECT_0)
            break; // worker finished creating the swapchain
          // A cross-thread SENT message is pending (the driver's ChoosePixelFormat). Flush sent
          // messages so the driver helper unblocks, WITHOUT removing/dispatching POSTED (input/resize)
          // messages - that avoids reentering the game's own window handling mid-create.
          MSG msg;
          ::PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        }
      }
      worker.join();
      if (doneEvent) ::CloseHandle(doneEvent);
      return workerStatus;
    };

    Logger::info(str::format(
      "Presenter: Actual swap chain properties:"
      "\n  Format:       ", m_info.format.format,
      "\n  Present mode: ", m_info.presentMode,
      "\n  Buffer size:  ", m_info.imageExtent.width, "x", m_info.imageExtent.height,
      "\n  Image count:  ", m_info.imageCount,
      "\n  Exclusive FS: ", desc.fullScreenExclusive));

    if ((status = createSwapchainDeadlockSafe(swapInfo)) != VK_SUCCESS) {

      const auto errString(str::format("Presenter: vkCreateSwapchainKHR failed, error code: ", status));

      if (swapInfo.pNext) {
        Logger::warn(errString);
        Logger::info("Presenter: retrying to create swap chain without Exclusive FS");

        m_info.appOwnedFSE = false;
        swapInfo.pNext = nullptr;

        if ((status = createSwapchainDeadlockSafe(swapInfo)) != VK_SUCCESS) {
          Logger::err(str::format("Presenter: vkCreateSwapchainKHR failed again, error code: ", status, ". Giving up."));

          // DX11_V243: native Vulkan WSI could not create a swapchain for this
          // window (commonly VK_ERROR_NATIVE_WINDOW_IN_USE_KHR when a game creates
          // a second swapchain on a window that already owns one - Minecraft). The
          // GDI-blit interop present has no such per-window limit, so fall back to
          // it once instead of failing swapchain creation (which would throw
          // "Failed to create swap chain" back to the game). One-shot guarded by
          // inGdiFallbackAttempt so a double failure returns the error normally.
          if (!inGdiFallbackAttempt && m_window) {
            Logger::info("Presenter: falling back to GDI interop present after native WSI failure");
            m_gdiFallback = true;
            return recreateSwapChain(desc);
          }

          return status;
        }
      }
      else {
        Logger::err(errString);
        return status;
      }
    }

    // NV-DXVK start: App Controlled FSE
    acquireFullscreenExclusive();
    // NV-DXVK end

    // Acquire images and create views
    std::vector<VkImage> images;

    if ((status = getSwapImages(images)) != VK_SUCCESS)
      return status;
    
    // Update actual image count
    m_info.imageCount = images.size();
    m_images.resize(m_info.imageCount);

    for (uint32_t i = 0; i < m_info.imageCount; i++) {
      m_images[i].image = images[i];

      VkImageViewCreateInfo viewInfo;
      viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      viewInfo.pNext    = nullptr;
      viewInfo.flags    = 0;
      viewInfo.image    = images[i];
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format   = m_info.format.format;
      viewInfo.components = VkComponentMapping {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
      viewInfo.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0, 1, 0, 1 };
      
      if ((status = m_vkd->vkCreateImageView(m_vkd->device(),
          &viewInfo, nullptr, &m_images[i].view)) != VK_SUCCESS)
        return status;
    }

    // Create one set of semaphores per swap image
    m_semaphores.resize(m_info.imageCount);

    for (uint32_t i = 0; i < m_semaphores.size(); i++) {
      VkSemaphoreCreateInfo semInfo;
      semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semInfo.pNext = nullptr;
      semInfo.flags = 0;

      if ((status = m_vkd->vkCreateSemaphore(m_vkd->device(),
          &semInfo, nullptr, &m_semaphores[i].acquire)) != VK_SUCCESS)
        return status;

      if ((status = m_vkd->vkCreateSemaphore(m_vkd->device(),
          &semInfo, nullptr, &m_semaphores[i].present)) != VK_SUCCESS)
        return status;

      // NV-DXVK start: add debug names to VkImage objects
      if (m_vkd->vkSetDebugUtilsObjectNameEXT) {
        VkDebugUtilsObjectNameInfoEXT nameInfo;
        nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        nameInfo.pNext = nullptr;
        nameInfo.objectType = VK_OBJECT_TYPE_SEMAPHORE;
        nameInfo.objectHandle = (uint64_t) m_semaphores[i].acquire;
        nameInfo.pObjectName = "Presenter: acquire semaphore";
        m_vkd->vkSetDebugUtilsObjectNameEXT(m_vkd->device(), &nameInfo);
        
        nameInfo.objectHandle = (uint64_t) m_semaphores[i].present;
        nameInfo.pObjectName = "Presenter: present semaphore";
        m_vkd->vkSetDebugUtilsObjectNameEXT(m_vkd->device(), &nameInfo);
      }
      // NV-DXVK end
    }
    
    // Invalidate indices
    m_imageIndex = 0;
    m_frameIndex = 0;
    m_acquireStatus = VK_NOT_READY;
    return VK_SUCCESS;
  }


  void Presenter::setFrameRateLimit(double frameRate) {
    m_fpsLimiter.setTargetFrameRate(frameRate);
  }


  void Presenter::setFrameRateLimiterRefreshRate(double refreshRate) {
    m_fpsLimiter.setDisplayRefreshRate(refreshRate);
  }


  VkResult Presenter::getSupportedFormats(std::vector<VkSurfaceFormatKHR>& formats, const PresenterDesc& desc) {
    uint32_t numFormats = 0;

    VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo;
    fullScreenInfo.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
    fullScreenInfo.pNext = nullptr;
    fullScreenInfo.fullScreenExclusive = desc.fullScreenExclusive;

    VkSurfaceFullScreenExclusiveWin32InfoEXT fullScreenInfoWin32;

    if (desc.fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT) {
      fullScreenInfoWin32.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT;
      fullScreenInfoWin32.pNext = nullptr;
      fullScreenInfoWin32.hmonitor = GetDefaultMonitor();
      fullScreenInfo.pNext = &fullScreenInfoWin32;
    }

    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo;
    surfaceInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surfaceInfo.pNext = &fullScreenInfo;
    surfaceInfo.surface = m_surface;

    VkResult status;
    
    if (m_device.features.fullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfaceFormats2KHR(
        m_device.adapter, &surfaceInfo, &numFormats, nullptr);
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfaceFormatsKHR(
        m_device.adapter, m_surface, &numFormats, nullptr);
    }

    if (status != VK_SUCCESS)
      return status;
    
    formats.resize(numFormats);

    if (m_device.features.fullScreenExclusive) {
      std::vector<VkSurfaceFormat2KHR> tmpFormats(numFormats, 
        { VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR, nullptr, VkSurfaceFormatKHR() });

      status = m_vki->vkGetPhysicalDeviceSurfaceFormats2KHR(
        m_device.adapter, &surfaceInfo, &numFormats, tmpFormats.data());

      for (uint32_t i = 0; i < numFormats; i++)
        formats[i] = tmpFormats[i].surfaceFormat;
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfaceFormatsKHR(
        m_device.adapter, m_surface, &numFormats, formats.data());
    }

    return status;
  }

  
  VkResult Presenter::getSupportedPresentModes(std::vector<VkPresentModeKHR>& modes, const PresenterDesc& desc) {
    uint32_t numModes = 0;

    VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo;
    fullScreenInfo.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
    fullScreenInfo.pNext = nullptr;
    fullScreenInfo.fullScreenExclusive = desc.fullScreenExclusive;
    
    VkSurfaceFullScreenExclusiveWin32InfoEXT fullScreenInfoWin32;

    if (desc.fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT) {
      fullScreenInfoWin32.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT;
      fullScreenInfoWin32.pNext = nullptr;
      fullScreenInfoWin32.hmonitor = GetDefaultMonitor();
      fullScreenInfo.pNext = &fullScreenInfoWin32;
    }

    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo;
    surfaceInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surfaceInfo.pNext = &fullScreenInfo;
    surfaceInfo.surface = m_surface;

    VkResult status;

    if (m_device.features.fullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModes2EXT(
        m_device.adapter, &surfaceInfo, &numModes, nullptr);
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_device.adapter, m_surface, &numModes, nullptr);
    }

    if (status != VK_SUCCESS)
      return status;
    
    modes.resize(numModes);

    if (m_device.features.fullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModes2EXT(
        m_device.adapter, &surfaceInfo, &numModes, modes.data());
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_device.adapter, m_surface, &numModes, modes.data());
    }

    return status;
  }


  VkResult Presenter::getSwapImages(std::vector<VkImage>& images) {
    uint32_t imageCount = 0;

    VkResult status = m_vkd->vkGetSwapchainImagesKHR(
      m_vkd->device(), m_swapchain, &imageCount, nullptr);
    
    if (status != VK_SUCCESS)
      return status;
    
    images.resize(imageCount);

    return m_vkd->vkGetSwapchainImagesKHR(
      m_vkd->device(), m_swapchain, &imageCount, images.data());
  }


  VkSurfaceFormatKHR Presenter::pickFormat(
          uint32_t                  numSupported,
    const VkSurfaceFormatKHR*       pSupported,
          uint32_t                  numDesired,
    const VkSurfaceFormatKHR*       pDesired) {
    if (numDesired > 0) {
      // If the implementation allows us to freely choose
      // the format, we'll just use the preferred format.
      if (numSupported == 1 && pSupported[0].format == VK_FORMAT_UNDEFINED)
        return pDesired[0];
      
      // If the preferred format is explicitly listed in
      // the array of supported surface formats, use it
      for (uint32_t i = 0; i < numDesired; i++) {
        for (uint32_t j = 0; j < numSupported; j++) {
          if (pSupported[j].format     == pDesired[i].format
           && pSupported[j].colorSpace == pDesired[i].colorSpace)
            return pSupported[j];
        }
      }

      // If that didn't work, we'll fall back to a format
      // which has similar properties to the preferred one
      DxvkFormatFlags prefFlags = imageFormatInfo(pDesired[0].format)->flags;

      for (uint32_t j = 0; j < numSupported; j++) {
        auto currFlags = imageFormatInfo(pSupported[j].format)->flags;

        if ((currFlags & DxvkFormatFlag::ColorSpaceSrgb)
         == (prefFlags & DxvkFormatFlag::ColorSpaceSrgb))
          return pSupported[j];
      }
    }
    
    // Otherwise, fall back to the first supported format
    return pSupported[0];
  }


  VkPresentModeKHR Presenter::pickPresentMode(
          uint32_t                  numSupported,
    const VkPresentModeKHR*         pSupported,
          uint32_t                  numDesired,
    const VkPresentModeKHR*         pDesired) {
    // Just pick the first desired and supported mode
    for (uint32_t i = 0; i < numDesired; i++) {
      for (uint32_t j = 0; j < numSupported; j++) {
        if (pSupported[j] == pDesired[i])
          return pSupported[j];
      }
    }
    
    // Guaranteed to be available
    return VK_PRESENT_MODE_FIFO_KHR;
  }


  VkExtent2D Presenter::pickImageExtent(
    const VkSurfaceCapabilitiesKHR& caps,
          VkExtent2D                desired) {
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
      return caps.currentExtent;
    
    VkExtent2D actual;
    actual.width  = clamp(desired.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
    actual.height = clamp(desired.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return actual;
  }


  uint32_t Presenter::pickImageCount(
    const VkSurfaceCapabilitiesKHR& caps,
          VkPresentModeKHR          presentMode,
          uint32_t                  desired) {
    uint32_t count = caps.minImageCount;
    
    if (presentMode != VK_PRESENT_MODE_IMMEDIATE_KHR)
      count = caps.minImageCount + 1;
    
    if (count < desired)
      count = desired;
    
    if (count > caps.maxImageCount && caps.maxImageCount != 0)
      count = caps.maxImageCount;
    
    return count;
  }


  VkResult Presenter::createSurface() {
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
      GetWindowLongPtr(m_window, GWLP_HINSTANCE));
    
    VkWin32SurfaceCreateInfoKHR info;
    info.sType      = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.pNext      = nullptr;
    info.flags      = 0;
    info.hinstance  = instance;
    info.hwnd       = m_window;
    
    VkResult status = m_vki->vkCreateWin32SurfaceKHR(
      m_vki->instance(), &info, nullptr, &m_surface);
    
    if (status != VK_SUCCESS)
      return status;
    
    VkBool32 supportStatus = VK_FALSE;

    if ((status = m_vki->vkGetPhysicalDeviceSurfaceSupportKHR(m_device.adapter,
        m_device.queueFamily, m_surface, &supportStatus)) != VK_SUCCESS)
      return status;
    
    if (!supportStatus) {
      m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);
      return VK_ERROR_OUT_OF_HOST_MEMORY; // just abuse this
    }

    return VK_SUCCESS;
  }


  void Presenter::destroySwapchain() {
    releaseFullscreenExclusive();

    for (const auto& img : m_images)
      m_vkd->vkDestroyImageView(m_vkd->device(), img.view, nullptr);

    // DX11_V238 interop: we own the raw present images + their memory (swapchain images are owned by
    // the VkSwapchain and must NOT be destroyed). Free them and tear down the D3D12 present path.
    if (m_interop) {
      auto* ip = reinterpret_cast<PresenterInterop*>(m_interop);
      m_vkd->vkDeviceWaitIdle(m_vkd->device());
      for (const auto& img : m_images)
        m_vkd->vkDestroyImage(m_vkd->device(), img.image, nullptr);
      for (auto mem : ip->vkMem)
        m_vkd->vkFreeMemory(m_vkd->device(), mem, nullptr);
      // free the host-visible readback staging resources
      if (ip->vkFence)   m_vkd->vkDestroyFence(m_vkd->device(), ip->vkFence, nullptr);
      if (ip->vkPool)    m_vkd->vkDestroyCommandPool(m_vkd->device(), ip->vkPool, nullptr);
      if (ip->vkStaging) m_vkd->vkDestroyBuffer(m_vkd->device(), ip->vkStaging, nullptr);
      if (ip->vkStagingMem) m_vkd->vkFreeMemory(m_vkd->device(), ip->vkStagingMem, nullptr);
      teardownInterop(ip);
      m_interop = nullptr;
    }

    for (const auto& sem : m_semaphores) {
      m_vkd->vkDestroySemaphore(m_vkd->device(), sem.acquire, nullptr);
      m_vkd->vkDestroySemaphore(m_vkd->device(), sem.present, nullptr);
    }

    m_vkd->vkDestroySwapchainKHR(m_vkd->device(), m_swapchain, nullptr);

    m_images.clear();
    m_semaphores.clear();

    m_swapchain = VK_NULL_HANDLE;
  }


  void Presenter::destroySurface() {
    m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);
  }

  // NV-DXVK start: App Controlled FSE
  VkResult Presenter::acquireFullscreenExclusive() {
    if (!m_info.appOwnedFSE)
      return VK_SUCCESS;

    if (!m_swapchain)
      return VK_ERROR_UNKNOWN;

    VkResult result = m_vkd->vkAcquireFullScreenExclusiveModeEXT(m_vkd->device(), m_swapchain);

    // Already acquired?
    if (result == VK_ERROR_INITIALIZATION_FAILED)
      return VK_SUCCESS;

    if (result == VK_SUCCESS) {
      Logger::debug("Acquired Fullscreen Exclusive");
    } else {
      Logger::warn("Fullscreen exclusive failed to acquire"); // This is not the end of the world.
    }

    return result;
  }

  VkResult Presenter::releaseFullscreenExclusive() {
    if (!m_info.appOwnedFSE)
      return VK_SUCCESS;

    if (!m_swapchain)
      return VK_ERROR_UNKNOWN;

    VkResult result = m_vkd->vkReleaseFullScreenExclusiveModeEXT(m_vkd->device(), m_swapchain);

    // Already released?
    if (result == VK_ERROR_INITIALIZATION_FAILED)
      return VK_SUCCESS;

    if (result == VK_SUCCESS) {
      Logger::debug("Released Fullscreen Exclusive");
    } else {
      Logger::err("Fullscreen exclusive failed to release"); // This is bad.
    }

    return result;
  }
  // NV-DXVK end
}
