/*
* Copyright (c) 2021-2025, NVIDIA CORPORATION. All rights reserved.
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

#include "util_kenshi_diagnostic_output.h"

// DX11_V162_REAL_ENV_BYPASS_FUNCTION
#ifdef _WIN32
#include <windows.h>
#include <cstring>
#endif

// NV-DXVK start: Fix some circular inclusion stuff
#include <string>
// NV-DXVK end

namespace dxvk::env {
  
  /**
   * \brief Checks whether the host platform is 32-bit
   */
  constexpr bool is32BitHostPlatform() {
    return sizeof(void*) == 4;
  }

  /**
   * \brief Gets environment variable
   * 
   * If the variable is not defined, this will return
   * an empty string. Note that environment variables
   * may be defined with an empty value.
   * \param [in] name Name of the variable
   * \returns Value of the variable
   */
  std::string getEnvVar(const char* name);

  // NV-DXVK start: get environment variable with a fallback
  /**
   * \brief Gets environment variable as a specified type
   *
   * NOTE: Generalization is deleted, and specific implementations
   * are moved to .cpp, to prevent include of 'config/config.h',
   * which in turn uses this header, 'util_env.h'.
   *
   * If parsing the string fails because it is either
     * invalid or if the option is not defined, this
     * method will return a fallback value. 
   * \tparam T Return value type
   * \param [in] name Name of the variable
   * \param [in] fallback Fallback value
   * \returns Parsed environment variable value
   */
  template<typename T>
  T getEnvVar(const char* name, T fallback) = delete;
  template<>
  bool getEnvVar(const char* name, bool fallback);
  // NV-DXVK end

  /**
   * \brief Sets environment variable
   * 
   * If setting the variable was successful true is returned,
   * false otherwise.
   * \param [in] name Name of the variable
   * \param [in] value String to set the variable to
   * \returns A boolean indicating if setting the environment variable succeeded.
   */
  bool setEnvVar(const char* name, const char* value);

  /**
   * \brief Checks whether a file name has a given extension
   *
   * \param [in] name File name
   * \param [in] ext Extension to match, in lowercase letters
   * \returns Position of the extension within the file name, or
   *    \c std::string::npos if the file has a different extension
   */
  size_t matchFileExtension(const std::string& name, const char* ext);

  /**
   * \brief Gets the executable name
   * 
   * Returns the base name (not the full path) of the
   * program executable, including the file extension.
   * This function should be used to identify programs.
   * \returns Executable name
   */
  std::string getExeName();


  /**
   * \brief Gets the executable name without the .exe
   * 
   * Returns the base name (not the full path) of the
   * program executable, including the file extension.
   * This function should be used to identify programs.
   * \returns Executable name
   */
  std::string getExeNameNoSuffix();
  
  /**
   * \brief Gets the executable name without extension
   *
   * Same as \ref getExeName but without the file extension.
   * \returns Executable name
   */
  std::string getExeBaseName();

  /**
   * \brief Gets full path to executable
   * \returns Path to executable
   */
  std::string getExePath();

  // NV-DXVK start

  /**
   * \brief Appends "__X" numbered suffix to filename in path if file already exists
   * \returns De-duped filename IF path already exists, else return original
   */
  std::string dedupeFilename(const std::string& originalFilePath);

  /**
   * \brief Query whether we're running under the remix bridge IPC mechanism
   * \returns True if running under Remix bridge
   */
  bool isRemixBridgeActive();
  
  /**
   * \brief Gets full path to a given module
   * \param module The name of the module to search for
   * \returns Path to module
   */
  std::string getModulePath(const char* module);

  /**
   * \brief Gets available system physical memory (i.e. system physical memory not used by any process)
   * \param availableSize Available system physical memory in bytes
   * \returns true if the function succeeds
   */
  bool getAvailableSystemPhysicalMemory(uint64_t& availableSize);

  /**
   * \brief Gets full directory path to the current module
   */
  std::string getDllDirectory();
  // NV-DXVK end

  /**
   * \brief Sets name of the calling thread
   * \param [in] name Thread name
   */
  void setThreadName(const std::string& name);

  /**
   * \brief Creates a directory
   * 
   * \param [in] path Path to directory
   * \returns \c true on success
   */
  bool createDirectory(const std::string& path);
  
  /**
 * \brief Kills the current process via system
 */
  void killProcess();
}

namespace dxvk::env {

#ifdef _WIN32
  // DX11_V282_BOOT_BREADCRUMB: earliest-possible boot signal, written at DLL
  // attach with pure kernel32 calls (loader-lock safe: no LoadLibrary, no CRT
  // streams, no threads, no allocation). Appends one line to
  // remix-dx11-boot.log next to the game executable. Diagnostic value:
  // "the game will not start and no logs exist" splits into two different
  // failure classes -
  //   - breadcrumb file absent  -> this DLL never attached: an anti-cheat or
  //     the Windows loader blocked it (incomplete payload, wrong architecture,
  //     missing hard dependency);
  //   - breadcrumb present, main log absent -> attach succeeded and the
  //     process died before device creation (see [crash] lines / bypass state).
  inline void remixAppendBootLine(const char* component, const char* message) {
    if (!kenshi_telemetry::fileOutputEnabled()) return;
    char exePath[MAX_PATH] = {};
    const DWORD exeLen = ::GetModuleFileNameA(nullptr, exePath, DWORD(sizeof(exePath)));
    if (exeLen == 0 || exeLen >= DWORD(sizeof(exePath)))
      return;

    int dirLen = -1;
    for (int i = 0; exePath[i] != '\0'; ++i) {
      if (exePath[i] == '\\')
        dirLen = i;
    }
    if (dirLen < 0 || dirLen > MAX_PATH - 32)
      return;

    char logPath[MAX_PATH] = {};
    std::memcpy(logPath, exePath, size_t(dirLen) + 1);
    std::memcpy(logPath + dirLen + 1, "remix-dx11-boot.log", sizeof("remix-dx11-boot.log"));

    const HANDLE file = ::CreateFileA(logPath, FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return;

    char line[640];
    size_t pos = 0;
    auto append = [&](const char* s) {
      while (s != nullptr && *s != '\0' && pos < sizeof(line) - 2)
        line[pos++] = *s++;
    };
    auto appendU32 = [&](DWORD v) {
      char digits[12];
      int n = 0;
      do {
        digits[n++] = char('0' + v % 10u);
        v /= 10u;
      } while (v != 0 && n < 10);
      while (n > 0 && pos < sizeof(line) - 2)
        line[pos++] = digits[--n];
    };

    append("[remix-boot] ");
    append(component);
    append(" ");
    append(message);
    append(" exe=");
    append(exePath + dirLen + 1);
    append(" pid=");
    appendU32(::GetCurrentProcessId());
    append(" tick=");
    appendU32(::GetTickCount());
    line[pos++] = '\r';
    line[pos++] = '\n';

    DWORD written = 0;
    ::WriteFile(file, line, DWORD(pos), &written, nullptr);
    ::CloseHandle(file);
  }

  // DX11_V282: detects anti-cheat runtimes near the game executable (exe dir
  // and up to two parent dirs - Halo MCC keeps EasyAntiCheat two levels above
  // MCC-Win64-Shipping.exe). Detection and user guidance ONLY: Remix cannot
  // and must not run under an active anti-cheat; users must use the game's
  // OFFICIAL anti-cheat-disabled / modding launch mode (e.g. Halo MCC's
  // "Anti-Cheat Disabled" Steam launch option).
  inline const char* remixDetectAntiCheat() {
    char dir[MAX_PATH] = {};
    const DWORD exeLen = ::GetModuleFileNameA(nullptr, dir, DWORD(sizeof(dir)));
    if (exeLen == 0 || exeLen >= DWORD(sizeof(dir)))
      return nullptr;

    // First cut drops the exe name; each further cut climbs one directory.
    for (int level = 0; level < 3; ++level) {
      int cut = -1;
      for (int i = 0; dir[i] != '\0'; ++i) {
        if (dir[i] == '\\')
          cut = i;
      }
      if (cut <= 2) // stop at the drive root ("C:\")
        break;
      dir[cut] = '\0';
      if (cut > MAX_PATH - 20)
        continue;

      char probe[MAX_PATH];
      std::memcpy(probe, dir, size_t(cut));
      std::memcpy(probe + cut, "\\EasyAntiCheat", sizeof("\\EasyAntiCheat"));
      if (::GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES)
        return "EasyAntiCheat";
      std::memcpy(probe + cut, "\\BattlEye", sizeof("\\BattlEye"));
      if (::GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES)
        return "BattlEye";
    }
    return nullptr;
  }
#endif

#ifdef _WIN32
  // DX11_V290_RUNTIME_DIR: the Remix satellite payload (USD stack, NRD, DLSS,
  // XeSS, NRC, rtxio, ...) no longer has to be dumped flat next to the game
  // exe. Resolution order:
  //   1. DXVK_REMIX_RUNTIME_DIR environment variable (existing directory)
  //   2. "rtx-remix\runtime" beside this DLL (i.e. beside the game exe)
  //   3. none - the classic flat layout beside the exe keeps working.
  // Kernel32-only so it is safe from DllMain under the loader lock. Returns
  // the character count written to 'out' (0 = no runtime directory).
  inline DWORD remixResolveRuntimeDirectoryW(wchar_t* out, DWORD outCapacity) {
    if (out == nullptr || outCapacity == 0)
      return 0;
    out[0] = L'\0';

    const auto isDirectory = [](const wchar_t* path) {
      const DWORD attributes = ::GetFileAttributesW(path);
      return attributes != INVALID_FILE_ATTRIBUTES
          && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    };

    const DWORD envLen = ::GetEnvironmentVariableW(
      L"DXVK_REMIX_RUNTIME_DIR", out, outCapacity);
    if (envLen > 0 && envLen < outCapacity && isDirectory(out))
      return envLen;
    out[0] = L'\0';

    // This inline function is compiled into the calling DLL, so the module
    // resolved from its address is that DLL (d3d11.dll / dxgi.dll).
    HMODULE selfModule = nullptr;
    ::GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&remixResolveRuntimeDirectoryW), &selfModule);
    const DWORD moduleLen = ::GetModuleFileNameW(selfModule, out, outCapacity);
    if (moduleLen == 0 || moduleLen >= outCapacity) {
      out[0] = L'\0';
      return 0;
    }
    wchar_t* separator = ::wcsrchr(out, L'\\');
    if (separator == nullptr) {
      out[0] = L'\0';
      return 0;
    }
    *separator = L'\0';

    static constexpr wchar_t kSuffix[] = L"\\rtx-remix\\runtime";
    const size_t baseLen = size_t(separator - out);
    if (baseLen + (sizeof(kSuffix) / sizeof(wchar_t)) >= outCapacity) {
      out[0] = L'\0';
      return 0;
    }
    std::memcpy(out + baseLen, kSuffix, sizeof(kSuffix));
    if (isDirectory(out))
      return DWORD(baseLen + (sizeof(kSuffix) / sizeof(wchar_t)) - 1);

    out[0] = L'\0';
    return 0;
  }
#endif

  inline bool shouldBypassRemixForCurrentProcess() {
#ifdef _WIN32
    auto readFlag = [](const char* name, bool fallback) -> bool {
      char value[32] = {};
      const DWORD len = ::GetEnvironmentVariableA(name, value, DWORD(sizeof(value)));
      if (len == 0 || len >= DWORD(sizeof(value)))
        return fallback;

      if (!_stricmp(value, "1") || !_stricmp(value, "true") || !_stricmp(value, "yes") || !_stricmp(value, "on"))
        return true;

      if (!_stricmp(value, "0") || !_stricmp(value, "false") || !_stricmp(value, "no") || !_stricmp(value, "off"))
        return false;

      return fallback;
    };

    if (readFlag("DXVK_REMIX_FORCE_CURRENT_PROCESS", false))
      return false;

    if (readFlag("DXVK_REMIX_ALLOW_CURRENT_PROCESS", false))
      return false;

    if (readFlag("DXVK_REMIX_BYPASS_CURRENT_PROCESS", false))
      return true;

    char modulePath[MAX_PATH] = {};
    const DWORD pathLen = ::GetModuleFileNameA(nullptr, modulePath, DWORD(sizeof(modulePath)));
    if (pathLen != 0 && pathLen < DWORD(sizeof(modulePath))) {
      const char* fileName = std::strrchr(modulePath, '\\');
      fileName = fileName ? fileName + 1 : modulePath;

      // Keep the bridge/runtime path active. These helpers are part of the DX11
      // Remix chain and must not be auto-bypassed.
      if (!_stricmp(fileName, "NvRemixBridge.exe") || !_stricmp(fileName, "NvRemixLauncher32.exe"))
        return false;

      // DX11_V279_LAUNCHER_BYPASS: game LAUNCHERS render their small UI with
      // D3D11 from the same game folder, so the full Remix runtime (Vulkan
      // device, shader compilation, RTX init) spins up INSIDE the launcher
      // process and can crash or wedge it before the actual game is ever
      // spawned - "the game hard crashes going nowhere" (Bethesda:
      // SkyrimSELauncher.exe, Fallout4Launcher.exe; many other titles ship a
      // *Launcher*.exe the same way). A launcher does not need path tracing;
      // forward it to the system D3D11 and let the real game process get
      // Remix. If a game's MAIN exe is genuinely named *launcher*, opt back
      // in with DXVK_REMIX_FORCE_CURRENT_PROCESS=1 (checked above).
      char lowerName[MAX_PATH] = {};
      size_t n = 0;
      for (; fileName[n] != '\0' && n < MAX_PATH - 1; ++n)
        lowerName[n] = static_cast<char>(::tolower(static_cast<unsigned char>(fileName[n])));
      lowerName[n] = '\0';
      if (std::strstr(lowerName, "launcher") != nullptr)
        return true;

      // DX11_V319_OVERLAY_BYPASS: store overlays and engine helper processes
      // ship beside the game, load the game folder's d3d11.dll, and render a
      // small D3D11 UI of their own - so each one spun up a COMPLETE Remix
      // runtime: its own Vulkan device, RTX initializer, shader prewarm and
      // log file. Saints Row IV (Epic) starts the EOS overlay renderer over and
      // over: one session produced 33 d3d11 logs, 31 of them from
      // EOSOverlayRenderer-Win64-Shipping.exe, each holding a full Remix device
      // and its allocations for as long as it lived. None of them draws
      // anything the player sees ray traced.
      //
      // Matching on the helper ROLE rather than on one title's executable keeps
      // this general: every Epic/EOS overlay renderer, Unreal's CEF browser
      // subprocess and any crash reporter are handled by the same rule. As with
      // the launcher rule above, DXVK_REMIX_FORCE_CURRENT_PROCESS=1 opts a
      // process back in.
      static const char* const kHelperProcessMarkers[] = {
        "eosoverlay",           // Epic Online Services overlay renderer
        "overlayrenderer",      // its generic name, and other stores' equivalents
        "unrealcefsubprocess",  // Unreal's embedded browser helper
        "crashreport",          // CrashReportClient.exe and friends
        "webhelper",            // store web UI helpers
      };
      for (const char* marker : kHelperProcessMarkers) {
        if (std::strstr(lowerName, marker) != nullptr)
          return true;
      }
    }

    return false;
#else
    return false;
#endif
  }

}
