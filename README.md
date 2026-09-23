# Kenshi Remix

Path-traced rendering for Kenshi, based on NVIDIA RTX Remix.

## Requirements

- Kenshi, Steam version 1.0.68. Other versions are untested.
- Windows 10 or 11, 64-bit.
- Microsoft Visual C++ Redistributable 2015-2022 (x64).
- GPU with Vulkan Ray Tracing support (theoretically any is supported, practical recommendation - RTX 40/50 series with 12GB+ VRAM).

## Installation

1. Download the archive from the Releases page.
2. Extract its contents into the Kenshi folder, next to `kenshi_x64.exe`.
3. Start the game.

The first start takes longer while shaders are built. Can be significant in time - up to 10-15 minutes on stronger CPUs, longer by unknown amount on weaker CPUs.

## Settings

Alt+X opens the settings menu. Saved settings are stored in `user.conf`.

Alt+X > Developer Settings Menu > Game Setup > Step 2: Parameter Tuning > Kenshi opens Kenshi-specific settings set. Contains both visual and performance settings. Some of them, such as anti-culling distance, can significantly affect performance. Tune for better performance/visuals tradeoff and for personal visual preferences.

## Uninstallation

Delete the files extracted from the archive, the `usd` folder and the `rtx-remix` folder.

## Crash reports

After a crash, a file named `remix-dx11-crash-<number>.dmp` is written to `%TEMP%`. Attach it when reporting the crash.

## Building from source

Requires Visual Studio 2022 with the C++ desktop development workload. In PowerShell, from the repository folder:

```powershell
$env:Path = "C:\Program Files (x86)\Microsoft Visual Studio\Installer;$env:Path"
.\Build-X64ReleaseNow.ps1
```

The script ends with exit code 1 even when the build succeeds; success is indicated by the line `DONE. Output folder`. Copy `_Comp64Release\src\d3d11\d3d11.dll` and `_Comp64Release\src\dxgi\dxgi.dll` over the same files of an existing installation. Both files must come from the same build.

## Credits and licenses

Modified version of [DXVK-Remix DX11](https://github.com/Murray2k6/dxvk-remix-DX11) by Murray2k6, which is based on [NVIDIA RTX Remix](https://github.com/NVIDIAGameWorks/dxvk-remix) and [DXVK](https://github.com/doitsujin/dxvk).

- DXVK: zlib license, see `LICENSE`.
- NVIDIA RTX Remix: MIT license, see `LICENSE-MIT`.
- Third-party components, including NVIDIA DLSS: see `ThirdPartyLicenses.txt`.

This project is not affiliated with or endorsed by NVIDIA or Lo-Fi Games.
