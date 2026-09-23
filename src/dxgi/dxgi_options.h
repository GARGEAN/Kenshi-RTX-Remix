#pragma once

#include "../util/config/config.h"

#include "../dxvk/dxvk_include.h"

#include "dxgi_include.h"

namespace dxvk {

  /**
   * \brief DXGI options
   * 
   * Per-app options that control the
   * behaviour of some DXGI classes.
   */
  struct DxgiOptions {
    DxgiOptions(const Config& config);

    /// Override PCI vendor and device IDs reported to the
    /// application. This may make apps think they are running
    /// on a different GPU than they do and behave differently.
    int32_t customVendorId;
    int32_t customDeviceId;
    std::string customDeviceDesc;
    
    /// Override maximum reported VRAM size. This may be
    /// useful for some 64-bit games which do not support
    /// more than 4 GiB of VRAM.
    VkDeviceSize maxDeviceMemory;
    VkDeviceSize maxSharedMemory;

    /// Emulate UMA
    bool emulateUMA;

    /// Emulate exclusive fullscreen as a borderless topless window without
    /// changing the display mode. Cures games that minimize and restore in a
    /// loop when the exclusive-mode switch interacts badly with focus changes
    /// (overlays, second monitors, the Remix UI focus handling).
    bool emulateFullscreen;

    /// Enables the legacy NvAPI vendor spoof workaround.
    /// Disabled by default in this DX11 Remix fork so adapters
    /// report their real vendor unless explicitly overridden.
    bool nvapiHack;

    /// DX11_V319: raise a game's requested fullscreen refresh rate to the
    /// monitor's current desktop rate. This exists so 60Hz-locked titles
    /// (Skyrim SE) run at the display's real rate, and it only ever upgrades -
    /// a game asking for more than the desktop rate is left alone.
    ///
    /// It is a setting because it changes a mode the game explicitly asked for,
    /// which is a plausible way to upset a title that cares. Call of Duty
    /// Advanced Warfare requests 50Hz, gets 144Hz, and its log ends at the
    /// ChangeDisplaySettingsEx that follows with no further output and no draws
    /// - set dxgi.overrideRefreshRate = False to take this out of the picture
    /// when diagnosing a game that stops at the fullscreen transition.
    bool overrideRefreshRate;
  };
  
}
