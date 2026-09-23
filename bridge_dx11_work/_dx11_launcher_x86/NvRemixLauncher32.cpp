#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>
#include <string>

static void dirnameOf(const wchar_t* in, wchar_t* out, DWORD cap) {
  out[0] = 0;
  if (!in || !in[0]) return;
  lstrcpynW(out, in, cap);
  wchar_t* slash = wcsrchr(out, L'\\');
  if (slash) slash[1] = 0;
}

static void appendLog(const wchar_t* root, const wchar_t* msg) {
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L"NvRemixLauncher32.log");
  HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME st; GetLocalTime(&st);
  char line[4096] = {};
  char m[3072] = {};
  WideCharToMultiByte(CP_UTF8, 0, msg ? msg : L"", -1, m, sizeof(m), nullptr, nullptr);
  int n = sprintf_s(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, m);
  DWORD wrote = 0;
  if (n > 0) WriteFile(h, line, (DWORD)n, &wrote, nullptr);
  CloseHandle(h);
}

static bool existsFile(const wchar_t* p) {
  DWORD a = GetFileAttributesW(p);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool isExcludedExe(const wchar_t* name) {
  return _wcsicmp(name, L"NvRemixLauncher32.exe") == 0 ||
         _wcsicmp(name, L"NvRemixBridge.exe") == 0 ||
         _wcsicmp(name, L"dxvk-remix.exe") == 0 ||
         _wcsicmp(name, L"setup.exe") == 0 ||
         _wcsicmp(name, L"unins000.exe") == 0;
}

static bool autoFindGameExe(const wchar_t* root, wchar_t* out, DWORD cap) {
  wchar_t pattern[MAX_PATH] = {};
  lstrcpynW(pattern, root, MAX_PATH);
  lstrcatW(pattern, L"*.exe");
  WIN32_FIND_DATAW fd = {};
  HANDLE h = FindFirstFileW(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !isExcludedExe(fd.cFileName)) {
      lstrcpynW(out, root, cap);
      lstrcatW(out, fd.cFileName);
      FindClose(h);
      return true;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return false;
}

static void quoteAppend(wchar_t* dst, DWORD cap, const wchar_t* s) {
  lstrcatW(dst, L"\"");
  lstrcatW(dst, s);
  lstrcatW(dst, L"\"");
}

static bool pathLooksRooted(const wchar_t* p) {
  return (p && ((wcslen(p) > 2 && p[1] == L':') || (p[0] == L'\\' && p[1] == L'\\')));
}


// DX11_V219_STEAM_AWARE_FALLBACK_HELPER
// Normal Steam/Unity path: start the game through Steam, then the game loads
// local d3d11.dll/dxgi.dll and those DLLs run the bridge client.
// This launcher remains packaged as required DLL-started launcher/client helper, but if it is used
// inside steamapps/common, it must hand off to Steam instead of direct-launching
// the Unity EXE, or SteamAPI may exit/relaunch the process.
static const wchar_t* findNoCaseW(const wchar_t* haystack, const wchar_t* needle) {
  if (!haystack || !needle || !needle[0]) return nullptr;
  const size_t needleLen = wcslen(needle);
  for (const wchar_t* p = haystack; *p; ++p) {
    if (_wcsnicmp(p, needle, needleLen) == 0) return p;
  }
  return nullptr;
}

static bool readTextFileA(const wchar_t* path, char* out, DWORD cap) {
  if (!out || cap < 2) return false;
  out[0] = 0;
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD read = 0;
  BOOL ok = ReadFile(h, out, cap - 1, &read, nullptr);
  CloseHandle(h);
  if (!ok) return false;
  out[read] = 0;
  return true;
}

static bool parseFirstDigitsA(const char* text, wchar_t* out, DWORD cap) {
  if (!text || !out || cap < 2) return false;
  const char* p = text;
  while (*p && (*p < '0' || *p > '9')) ++p;
  if (!*p) return false;
  DWORD n = 0;
  while (*p >= '0' && *p <= '9' && n + 1 < cap) out[n++] = (wchar_t)*p++;
  out[n] = 0;
  return n > 0;
}

static bool parseAcfQuotedValueA(const char* text, const char* key, char* out, DWORD cap) {
  if (!text || !key || !out || cap < 2) return false;
  out[0] = 0;
  const char* p = strstr(text, key);
  if (!p) return false;
  p += strlen(key);
  while (*p && *p != '"') ++p;
  if (!*p) return false;
  ++p;
  DWORD n = 0;
  while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
  out[n] = 0;
  return n > 0;
}

static bool strEqNoCaseA(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    char ca = *a++, cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = char(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return *a == 0 && *b == 0;
}

static bool getLeafDirNameA(const wchar_t* root, char* out, DWORD cap) {
  if (!root || !out || cap < 2) return false;
  wchar_t tmp[MAX_PATH] = {};
  lstrcpynW(tmp, root, MAX_PATH);
  size_t len = wcslen(tmp);
  while (len > 0 && (tmp[len - 1] == L'\\' || tmp[len - 1] == L'/')) tmp[--len] = 0;
  const wchar_t* slash = wcsrchr(tmp, L'\\');
  const wchar_t* leaf = slash ? slash + 1 : tmp;
  int got = WideCharToMultiByte(CP_UTF8, 0, leaf, -1, out, cap, nullptr, nullptr);
  return got > 1;
}

static bool findSteamAppsRoot(const wchar_t* root, wchar_t* out, DWORD cap) {
  const wchar_t* marker = findNoCaseW(root, L"\\steamapps\\common\\");
  if (!marker) return false;
  size_t prefixLen = (size_t)(marker - root) + wcslen(L"\\steamapps\\");
  if (prefixLen + 1 >= cap) return false;
  wcsncpy_s(out, cap, root, prefixLen);
  out[prefixLen] = 0;
  return true;
}

static bool findSteamAppId(const wchar_t* root, wchar_t* appid, DWORD appidCap) {
  appid[0] = 0;
  wchar_t appidTxt[MAX_PATH] = {};
  lstrcpynW(appidTxt, root, MAX_PATH);
  lstrcatW(appidTxt, L"steam_appid.txt");
  char small[256] = {};
  if (readTextFileA(appidTxt, small, sizeof(small)) && parseFirstDigitsA(small, appid, appidCap)) return true;

  wchar_t steamapps[MAX_PATH] = {};
  if (!findSteamAppsRoot(root, steamapps, MAX_PATH)) return false;

  char installDir[260] = {};
  if (!getLeafDirNameA(root, installDir, sizeof(installDir))) return false;

  wchar_t pattern[MAX_PATH] = {};
  lstrcpynW(pattern, steamapps, MAX_PATH);
  lstrcatW(pattern, L"appmanifest_*.acf");

  WIN32_FIND_DATAW fd = {};
  HANDLE hFind = FindFirstFileW(pattern, &fd);
  if (hFind == INVALID_HANDLE_VALUE) return false;

  bool found = false;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    wchar_t path[MAX_PATH] = {};
    lstrcpynW(path, steamapps, MAX_PATH);
    lstrcatW(path, fd.cFileName);
    char acf[65536] = {};
    if (!readTextFileA(path, acf, sizeof(acf))) continue;
    char acfInstall[260] = {};
    char acfAppid[64] = {};
    if (!parseAcfQuotedValueA(acf, "\"installdir\"", acfInstall, sizeof(acfInstall))) continue;
    if (!strEqNoCaseA(acfInstall, installDir)) continue;
    if (!parseAcfQuotedValueA(acf, "\"appid\"", acfAppid, sizeof(acfAppid))) continue;
    MultiByteToWideChar(CP_UTF8, 0, acfAppid, -1, appid, appidCap);
    found = appid[0] != 0;
    break;
  } while (FindNextFileW(hFind, &fd));

  FindClose(hFind);
  return found;
}

static bool handOffToSteamIfSteamGame(const wchar_t* root) {
  wchar_t appid[64] = {};
  if (!findSteamAppId(root, appid, 64)) return false;
  wchar_t uri[128] = {};
  swprintf_s(uri, L"steam://run/%s", appid);
  wchar_t msg[512] = {};
  swprintf_s(msg, L"Steam game detected. Handing off to %s. The Steam-launched game will load local d3d11.dll/dxgi.dll bridge client.", uri);
  appendLog(root, msg);
  HINSTANCE result = ShellExecuteW(nullptr, L"open", uri, nullptr, root, SW_SHOWNORMAL);
  INT_PTR code = reinterpret_cast<INT_PTR>(result);
  if (code <= 32) {
    wchar_t err[512] = {};
    swprintf_s(err, L"ShellExecuteW(%s) failed with code %Id. Start the game from Steam normally.", uri, code);
    appendLog(root, err);
    MessageBoxW(nullptr, err, L"DX11 Remix Launcher", MB_ICONERROR);
  }
  return true;
}



// DX11_V219_GAME_CMD_FILE_ANYWHERE
static bool readLauncherTextFileV219(const wchar_t* path, wchar_t* out, DWORD cap) {
  if (!path || !path[0] || !out || cap < 2) return false;
  out[0] = 0;
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  char raw[8192] = {};
  DWORD read = 0;
  BOOL ok = ReadFile(h, raw, sizeof(raw) - 1, &read, nullptr);
  CloseHandle(h);
  if (!ok || read == 0) return false;
  raw[read] = 0;
  while (read > 0 && (raw[read - 1] == '\r' || raw[read - 1] == '\n')) raw[--read] = 0;
  int wrote = MultiByteToWideChar(CP_UTF8, 0, raw, -1, out, cap);
  if (wrote <= 0) {
    wrote = MultiByteToWideChar(CP_ACP, 0, raw, -1, out, cap);
  }
  return wrote > 0 && out[0] != 0;
}

static bool readLauncherGuidFileV219(const wchar_t* root, wchar_t* out, DWORD cap) {
  if (!root || !out || cap < 37) return false;
  out[0] = 0;
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L".trex\\dx11_bridge_guid.txt");
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  char raw[128] = {};
  DWORD read = 0;
  BOOL ok = ReadFile(h, raw, sizeof(raw) - 1, &read, nullptr);
  CloseHandle(h);
  if (!ok || read == 0) return false;
  raw[read] = 0;
  while (read > 0 && (raw[read - 1] == '\r' || raw[read - 1] == '\n' || raw[read - 1] == ' ' || raw[read - 1] == '\t')) raw[--read] = 0;
  if (read != 36) return false;
  MultiByteToWideChar(CP_UTF8, 0, raw, -1, out, cap);
  return out[0] != 0;
}

static bool envNameIsSteamOverlayV219(const wchar_t* name) {
  if (!name) return false;
  return _wcsicmp(name, L"SteamGameId") == 0 ||
         _wcsicmp(name, L"SteamOverlayGameId") == 0 ||
         _wcsicmp(name, L"SteamAppId") == 0 ||
         _wcsicmp(name, L"SteamClientLaunch") == 0 ||
         _wcsicmp(name, L"SteamEnv") == 0 ||
         _wcsicmp(name, L"GAMEOVERLAYRENDERER_LOG") == 0 ||
         _wcsicmp(name, L"SteamOverlayUI") == 0;
}

static void appendEnvPairV219(std::wstring& block, const wchar_t* name, const wchar_t* value) {
  if (!name || !name[0] || !value) return;
  block.append(name);
  block.push_back(L'=');
  block.append(value);
  block.push_back(L'\0');
}

static std::wstring buildBridgeEnvBlockV219(const wchar_t* root, const wchar_t* trex, const wchar_t* guid, const wchar_t* version) {
  std::wstring block;
  LPWCH env = GetEnvironmentStringsW();
  if (env) {
    for (LPWCH cur = env; *cur; cur += wcslen(cur) + 1) {
      const wchar_t* eq = wcschr(cur, L'=');
      if (!eq || eq == cur) continue;
      wchar_t name[256] = {};
      size_t n = static_cast<size_t>(eq - cur);
      if (n >= _countof(name)) n = _countof(name) - 1;
      wcsncpy_s(name, cur, n);
      if (envNameIsSteamOverlayV219(name)) continue;
      block.append(cur);
      block.push_back(L'\0');
    }
    FreeEnvironmentStringsW(env);
  }

  wchar_t oldPath[32767] = {};
  GetEnvironmentVariableW(L"PATH", oldPath, 32767);
  wchar_t newPath[32767] = {};
  lstrcpynW(newPath, trex, 32767);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, root);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, oldPath);

  appendEnvPairV219(block, L"PATH", newPath);
  appendEnvPairV219(block, L"DXVK_REMIX_GAME_DIR", root);
  appendEnvPairV219(block, L"DXVK_REMIX_TREX_DIR", trex);
  appendEnvPairV219(block, L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");
  appendEnvPairV219(block, L"DX11_BRIDGE_GUID", guid);
  appendEnvPairV219(block, L"DX11_BRIDGE_FIXED_GUID", guid);
  appendEnvPairV219(block, L"DX11_BRIDGE_VERSION", version);
  appendEnvPairV219(block, L"DX11_BRIDGE_MODE", L"d3d11");
  appendEnvPairV219(block, L"SteamNoOverlayUIDrawing", L"1");
  appendEnvPairV219(block, L"SteamGameId", L"0");
  appendEnvPairV219(block, L"SteamOverlayGameId", L"0");
  appendEnvPairV219(block, L"SteamAppId", L"0");

  wchar_t plugins[MAX_PATH] = {};
  lstrcpynW(plugins, trex, MAX_PATH);
  lstrcatW(plugins, L"plugins");
  DWORD pluginAttr = GetFileAttributesW(plugins);
  if (pluginAttr != INVALID_FILE_ATTRIBUTES && (pluginAttr & FILE_ATTRIBUTE_DIRECTORY)) {
    appendEnvPairV219(block, L"PXR_PLUGINPATH_NAME", plugins);
  }

  block.push_back(L'\0');
  return block;
}

// DX11_V219_SINGLE_BRIDGE_SERVER_OWNER
static unsigned long long hashWidePathV219(const wchar_t* s) {
  unsigned long long h = 1469598103934665603ull;
  if (!s) return h;
  while (*s) {
    wchar_t c = *s++;
    if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    h ^= (unsigned long long)c;
    h *= 1099511628211ull;
  }
  return h;
}

static HANDLE acquireSingleBridgeServerOwnerV219(const wchar_t* root, const wchar_t* guid, bool* owns) {
  if (owns) *owns = false;
  wchar_t name[256] = {};
  unsigned long long h = hashWidePathV219(root);
  // DX11_V219_SINGLE_BRIDGE_PER_GAME_FOLDER
  // One bridge per game folder, not one bridge per GUID. Including GUID in
  // the mutex let every new launch create another bridge.
  swprintf_s(name, L"Local\\DX11RemixSingleBridge_%08X%08X",
    (unsigned)((h >> 32) & 0xffffffffu),
    (unsigned)(h & 0xffffffffu));

  HANDLE m = CreateMutexW(nullptr, TRUE, name);
  if (!m) {
    return nullptr;
  }

  DWORD err = GetLastError();
  if (err == ERROR_ALREADY_EXISTS) {
    DWORD waitNow = WaitForSingleObject(m, 0);
    if (waitNow == WAIT_OBJECT_0 || waitNow == WAIT_ABANDONED) {
      if (owns) *owns = true;
      return m;
    }

    appendLog(root, L"DX11_V219: another NvRemixBridge.exe owner is already active for this game folder; not starting a second bridge.");
    CloseHandle(m);
    return nullptr;
  }

  if (owns) *owns = true;
  appendLog(root, L"DX11_V219: acquired single bridge server owner mutex.");
  return m;
}

static void releaseSingleBridgeServerOwnerV219(HANDLE* ownerMutex) {
  if (ownerMutex && *ownerMutex) {
    ReleaseMutex(*ownerMutex);
    CloseHandle(*ownerMutex);
    *ownerMutex = nullptr;
  }
}

// DX11_V219_REAL_SERVER_PID_FOR_CLIENT_HANDLE
// DX11_V219_PID_RECORD_WITH_BRIDGE_PATH
static void clearBridgeServerPidFileV219(const wchar_t* root) {
  if (!root || !root[0]) return;
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L".trex\\dx11_bridge_server_pid.txt");
  DeleteFileW(path);
}

// Kept original V219 function name so existing call sites stay simple until version labels are applied.
static void writeBridgeServerPidFileV219(const wchar_t* root, DWORD pid, const wchar_t* bridgePath) {
  if (!root || !root[0] || !pid || !bridgePath || !bridgePath[0]) return;
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L".trex\\dx11_bridge_server_pid.txt");

  HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    appendLog(root, L"DX11_V219 WARNING: failed to write .trex\\dx11_bridge_server_pid.txt.");
    return;
  }

  // ASCII/UTF-8 record:
  //   <pid>|<full path to .trex\NvRemixBridge.exe>
  // The x86 client uses the path to validate it has the real server PID.
  char bridgeUtf8[MAX_PATH * 3] = {};
  WideCharToMultiByte(CP_UTF8, 0, bridgePath, -1, bridgeUtf8, sizeof(bridgeUtf8), nullptr, nullptr);

  char buf[4096] = {};
  sprintf_s(buf, "%lu|%s", pid, bridgeUtf8);

  DWORD bytes = 0;
  WriteFile(h, buf, static_cast<DWORD>(strlen(buf)), &bytes, nullptr);
  CloseHandle(h);
  appendLog(root, L"DX11_V219 wrote actual NvRemixBridge.exe PID+path to .trex\\dx11_bridge_server_pid.txt.");
}

// DX11_V219_LAUNCHER_DUPLICATES_REAL_CLIENT_HANDLE
static DWORD readRealGamePidForLauncherV219(const wchar_t* root) {
  if (!root || !root[0]) return 0;
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L".trex\\dx11_bridge_game_pid.txt");

  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    appendLog(root, L"DX11_V219 ERROR: missing .trex\\dx11_bridge_game_pid.txt.");
    return 0;
  }

  char raw[64] = {};
  DWORD got = 0;
  BOOL ok = ReadFile(h, raw, sizeof(raw) - 1, &got, nullptr);
  CloseHandle(h);
  if (!ok || got == 0) {
    appendLog(root, L"DX11_V219 ERROR: failed to read real game PID file.");
    return 0;
  }
  raw[got] = 0;

  char compact[64] = {};
  DWORD ci = 0;
  for (DWORD i = 0; i < got && ci + 1 < sizeof(compact); ++i) {
    if (raw[i] >= '0' && raw[i] <= '9') compact[ci++] = raw[i];
  }
  compact[ci] = 0;
  return static_cast<DWORD>(strtoul(compact, nullptr, 10));
}

static void writeLauncherDuplicatedClientHandleV219(const wchar_t* root, DWORD serverPid, DWORD gamePid, HANDLE remoteHandle) {
  if (!root || !root[0] || !serverPid || !gamePid || !remoteHandle) return;
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L".trex\\dx11_bridge_client_remote_handle.txt");

  HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    appendLog(root, L"DX11_V219 ERROR: failed to write .trex\\dx11_bridge_client_remote_handle.txt.");
    return;
  }

  char buf[256] = {};
  sprintf_s(buf, "%lu|%llu|%lu",
    serverPid,
    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(remoteHandle)),
    gamePid);

  DWORD bytes = 0;
  WriteFile(h, buf, static_cast<DWORD>(strlen(buf)), &bytes, nullptr);
  CloseHandle(h);
  appendLog(root, L"DX11_V219 wrote server-side real game process handle to .trex\\dx11_bridge_client_remote_handle.txt.");
}

static bool duplicateRealGameProcessHandleIntoBridgeServerV219(const wchar_t* root, HANDLE bridgeProcess, DWORD serverPid) {
  DWORD gamePid = readRealGamePidForLauncherV219(root);
  if (!gamePid) {
    appendLog(root, L"DX11_V219 ERROR: no real game PID available for server handle duplication.");
    return false;
  }

  HANDLE gameProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, gamePid);
  if (!gameProcess) {
    wchar_t msg[256] = {};
    swprintf_s(msg, L"DX11_V219 ERROR: launcher failed to open real game process pid=%lu err=%lu.", gamePid, GetLastError());
    appendLog(root, msg);
    return false;
  }

  HANDLE remoteHandle = nullptr;
  BOOL ok = DuplicateHandle(
    GetCurrentProcess(),
    gameProcess,
    bridgeProcess,
    &remoteHandle,
    0,
    FALSE,
    DUPLICATE_SAME_ACCESS);

  CloseHandle(gameProcess);

  if (!ok || !remoteHandle) {
    wchar_t msg[256] = {};
    swprintf_s(msg, L"DX11_V219 ERROR: launcher failed to duplicate real game handle into NvRemixBridge.exe pid=%lu err=%lu.", serverPid, GetLastError());
    appendLog(root, msg);
    return false;
  }

  writeLauncherDuplicatedClientHandleV219(root, serverPid, gamePid, remoteHandle);

  wchar_t msg[256] = {};
  swprintf_s(msg, L"DX11_V219 launcher duplicated real game process handle into NvRemixBridge.exe pid=%lu.", serverPid);
  appendLog(root, msg);
  return true;
}

// DX11_V219_REAL_GAME_TARGET_FOR_REMIX
static bool extractRealGameExeForServerV219(const wchar_t* gameCmd, wchar_t* out, DWORD cap) {
  if (!out || cap < 8) return false;
  out[0] = 0;
  if (!gameCmd || !gameCmd[0]) return false;

  const wchar_t* p = gameCmd;
  while (*p == L' ' || *p == L'\t') ++p;

  if (*p == L'"') {
    ++p;
    DWORD n = 0;
    while (*p && *p != L'"' && n + 1 < cap) {
      out[n++] = *p++;
    }
    out[n] = 0;
    return out[0] != 0;
  }

  const wchar_t* start = p;
  const wchar_t* exeEnd = nullptr;
  for (; *p; ++p) {
    if (_wcsnicmp(p, L".exe", 4) == 0) {
      exeEnd = p + 4;
      break;
    }
  }

  if (!exeEnd) {
    p = start;
    while (*p && *p != L' ' && *p != L'\t') ++p;
    exeEnd = p;
  }

  DWORD n = 0;
  for (const wchar_t* q = start; q < exeEnd && n + 1 < cap; ++q) {
    out[n++] = *q;
  }
  out[n] = 0;
  return out[0] != 0;
}

static void appendQuotedBridgeArgV219(wchar_t* cmd, DWORD cap, const wchar_t* arg) {
  if (!cmd || !arg || !arg[0]) return;
  DWORD used = static_cast<DWORD>(wcslen(cmd));
  if (used + 4 >= cap) return;
  cmd[used++] = L' ';
  cmd[used++] = L'"';
  while (*arg && used + 3 < cap) {
    if (*arg != L'"') {
      cmd[used++] = *arg;
    }
    ++arg;
  }
  cmd[used++] = L'"';
  cmd[used] = 0;
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  wchar_t root[MAX_PATH] = {};
  dirnameOf(self, root, MAX_PATH);

  appendLog(root, L"NvRemixLauncher32 DX11 launcher/client helper started. DX11_V219_DLL_LAUNCHER_BRIDGE_CHAIN");

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  bool launchBridge = false;
  wchar_t guid[128] = {};
  wchar_t version[128] = {};
  wchar_t gameCmd[4096] = {};
  wchar_t gameCmdFile[MAX_PATH] = {}; // DX11_V219_GAME_CMD_FILE_ANYWHERE

  if (argv) {
    for (int i = 1; i < argc; ++i) {
      if (_wcsicmp(argv[i], L"--dx11-launch-bridge") == 0) {
        launchBridge = true;
      } else if (_wcsicmp(argv[i], L"--guid") == 0 && i + 1 < argc) {
        lstrcpynW(guid, argv[++i], 128);
      } else if (_wcsicmp(argv[i], L"--version") == 0 && i + 1 < argc) {
        lstrcpynW(version, argv[++i], 128);
      } else if (_wcsicmp(argv[i], L"--game-root") == 0 && i + 1 < argc) {
        ++i;
      } else if (_wcsicmp(argv[i], L"--trex-root") == 0 && i + 1 < argc) {
        ++i;
      } else if (_wcsicmp(argv[i], L"--game-cmd-file") == 0 && i + 1 < argc) {
        lstrcpynW(gameCmdFile, argv[++i], MAX_PATH);
      } else if (_wcsicmp(argv[i], L"--game-cmd") == 0 && i + 1 < argc) {
        // Legacy fallback only. V219 uses --game-cmd-file so games can be anywhere.
        lstrcpynW(gameCmd, argv[++i], 4096);
      }
    }
  }

  if (!launchBridge) {
    appendLog(root, L"NvRemixLauncher32.exe was opened manually. It is started by the game-loaded d3d11.dll/dxgi.dll.");
    MessageBoxW(nullptr,
      L"Run the actual game normally.\n\nThe game loads the local d3d11.dll/dxgi.dll, and that DLL starts NvRemixLauncher32.exe.\nNvRemixLauncher32.exe then starts .trex\\NvRemixBridge.exe.",
      L"DX11 Remix Bridge",
      MB_OK | MB_ICONINFORMATION);
    if (argv) LocalFree(argv);
    return 0;
  }

  if (!guid[0]) {
    if (readLauncherGuidFileV219(root, guid, 128)) {
      appendLog(root, L"DX11_V219 launcher recovered shared DX9-style GUID from .trex\dx11_bridge_guid.txt.");
    }
  }

  if (!guid[0] || !version[0]) {
    appendLog(root, L"ERROR: DLL launched NvRemixLauncher32.exe without shared DX9-style --guid/--version.");
    if (argv) LocalFree(argv);
    return 10;
  }

  if (!gameCmd[0] && gameCmdFile[0]) {
    if (readLauncherTextFileV219(gameCmdFile, gameCmd, 4096)) {
      appendLog(root, L"DX11_V219 launcher loaded original game command line from --game-cmd-file.");
    } else {
      appendLog(root, L"DX11_V219 WARNING: failed to read --game-cmd-file.");
    }
  }
  if (!gameCmd[0]) {
    DWORD gotGameCmd = GetEnvironmentVariableW(L"DX11_BRIDGE_GAME_CMD", gameCmd, 4096);
    if (gotGameCmd > 0 && gotGameCmd < 4096) {
      appendLog(root, L"DX11_V219 launcher loaded original game command line from DX11_BRIDGE_GAME_CMD.");
    }
  }

  wchar_t realGameExeV219[MAX_PATH] = {};
  if (extractRealGameExeForServerV219(gameCmd, realGameExeV219, MAX_PATH)) {
    SetEnvironmentVariableW(L"DX11_BRIDGE_REAL_GAME_EXE", realGameExeV219);
    SetEnvironmentVariableW(L"DXVK_REMIX_REAL_GAME_EXE", realGameExeV219);
    SetEnvironmentVariableW(L"RTX_REMIX_TARGET_EXE", realGameExeV219);
    wchar_t targetMsg[1024] = {};
    swprintf_s(targetMsg, L"DX11_V219 real Remix target game exe: %s", realGameExeV219);
    appendLog(root, targetMsg);
  } else {
    appendLog(root, L"DX11_V219 WARNING: could not extract real game exe from original command line; bridge config may still see launcher target.");
  }

  wchar_t trex[MAX_PATH] = {};
  lstrcpynW(trex, root, MAX_PATH);
  lstrcatW(trex, L".trex\\");

  wchar_t bridge[MAX_PATH] = {};
  lstrcpynW(bridge, trex, MAX_PATH);
  lstrcatW(bridge, L"NvRemixBridge.exe");

  wchar_t rtD3D11[MAX_PATH] = {};
  wchar_t rtDxgi[MAX_PATH] = {};
  lstrcpynW(rtD3D11, trex, MAX_PATH); lstrcatW(rtD3D11, L"d3d11.dll");
  lstrcpynW(rtDxgi,  trex, MAX_PATH); lstrcatW(rtDxgi,  L"dxgi.dll");

  if (!existsFile(bridge)) {
    appendLog(root, L"ERROR: .trex\\NvRemixBridge.exe missing; launcher cannot start Remix server.");
    if (argv) LocalFree(argv);
    return 11;
  }
  if (!existsFile(rtD3D11) || !existsFile(rtDxgi)) {
    appendLog(root, L"ERROR: .trex\\d3d11.dll or .trex\\dxgi.dll missing; launcher cannot host Remix runtime.");
    if (argv) LocalFree(argv);
    return 12;
  }

  bool ownsBridgeServerV219 = false;
  HANDLE bridgeServerOwnerV219 = acquireSingleBridgeServerOwnerV219(root, guid, &ownsBridgeServerV219);
  if (!ownsBridgeServerV219) {
    appendLog(root, L"DX11_V219: duplicate launcher instance exiting after existing bridge owner finished; no second bridge was opened.");
    if (argv) LocalFree(argv);
    return 0;
  }

  wchar_t oldPath[32767] = {};
  GetEnvironmentVariableW(L"PATH", oldPath, 32767);
  wchar_t newPath[32767] = {};
  lstrcpynW(newPath, trex, 32767);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, root);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, oldPath);
  SetEnvironmentVariableW(L"PATH", newPath);
  SetEnvironmentVariableW(L"DXVK_REMIX_GAME_DIR", root);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", trex);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_GUID", guid);
  SetEnvironmentVariableW(L"DX11_BRIDGE_VERSION", version);
  SetEnvironmentVariableW(L"DX11_BRIDGE_MODE", L"d3d11");

  wchar_t plugins[MAX_PATH] = {};
  lstrcpynW(plugins, trex, MAX_PATH);
  lstrcatW(plugins, L"plugins");
  DWORD pluginAttr = GetFileAttributesW(plugins);
  if (pluginAttr != INVALID_FILE_ATTRIBUTES && (pluginAttr & FILE_ATTRIBUTE_DIRECTORY)) {
    SetEnvironmentVariableW(L"PXR_PLUGINPATH_NAME", plugins);
  }

  wchar_t cmd[8192] = {};
  // DX11_V219_REAL_GAME_TARGET_FOR_REMIX
  // NvRemixBridge.exe needs the real game exe target for Remix config/attachment.
  // Pass only the already-running game exe path, not the full game command line.
  // This gives Remix the correct target without relaunching the game.
  swprintf_s(cmd, L"\"%s\" %s %s", bridge, guid, version);
  if (gameCmd[0]) {
    SetEnvironmentVariableW(L"DX11_BRIDGE_GAME_CMD", gameCmd);
  }
  if (realGameExeV219[0]) {
    appendQuotedBridgeArgV219(cmd, 8192, realGameExeV219);
    appendLog(root, L"DX11_V219 appended real game exe target to NvRemixBridge.exe command line for Remix config lookup.");
  } else {
    appendLog(root, L"DX11_V219 WARNING: no real game exe target appended; Remix may resolve launcher as the app target.");
  }

  wchar_t msg[8192] = {};
  swprintf_s(msg, L"DX11_V219 launcher starting .trex bridge server with real game target. guid=%s cmd=%s cwd=%s", guid, cmd, trex);
  appendLog(root, msg);

  STARTUPINFOW si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  std::wstring bridgeEnvV219 = buildBridgeEnvBlockV219(root, trex, guid, version);
  SetEnvironmentVariableW(L"SteamNoOverlayUIDrawing", L"1");
  SetEnvironmentVariableW(L"SteamGameId", L"0");
  SetEnvironmentVariableW(L"SteamOverlayGameId", L"0");
  SetEnvironmentVariableW(L"SteamAppId", L"0");
  // DX11_V219_LAUNCHER_COMPILE_FIX
  LPVOID bridgeEnvBlockV219 = bridgeEnvV219.empty()
    ? nullptr
    : static_cast<LPVOID>(const_cast<wchar_t*>(bridgeEnvV219.c_str()));
  clearBridgeServerPidFileV219(root);
  DeleteFileW(L".trex\\dx11_bridge_client_remote_handle.txt");
  BOOL ok = CreateProcessW(
    bridge,
    cmd,
    nullptr,
    nullptr,
    FALSE,
    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
    bridgeEnvBlockV219,
    trex,
    &si,
    &pi);
  if (!ok) {
    wchar_t fail[512] = {};
    swprintf_s(fail, L"ERROR: launcher failed to start .trex\\NvRemixBridge.exe err=%lu", GetLastError());
    appendLog(root, fail);
    releaseSingleBridgeServerOwnerV219(&bridgeServerOwnerV219);
    if (argv) LocalFree(argv);
    return 13;
  }

  CloseHandle(pi.hThread);
  writeBridgeServerPidFileV219(root, pi.dwProcessId, bridge);
  duplicateRealGameProcessHandleIntoBridgeServerV219(root, pi.hProcess, pi.dwProcessId);
  appendLog(root, L"DX11_V219 launcher started .trex\\NvRemixBridge.exe, duplicated the real game handle into it, and is staying alive until the server exits.");
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  releaseSingleBridgeServerOwnerV219(&bridgeServerOwnerV219);

  wchar_t done[256] = {};
  swprintf_s(done, L"DX11_V219 .trex\\NvRemixBridge.exe exited with code %lu.", code);
  appendLog(root, done);

  if (argv) LocalFree(argv);
  return static_cast<int>(code);
}

