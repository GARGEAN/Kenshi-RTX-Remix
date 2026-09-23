#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

namespace dx11_bridge_getgoing {

inline HMODULE g_module = nullptr;

inline void setModule(HMODULE m) { g_module = m; }

inline void getDllFolder(char* out, DWORD cap) {
  out[0] = 0;
  char path[MAX_PATH] = {};
  if (g_module) GetModuleFileNameA(g_module, path, MAX_PATH);
  char* slash = strrchr(path, '\\');
  if (slash) {
    slash[1] = 0;
    lstrcpynA(out, path, cap);
  }
}

inline void logLine(const char* tag, const char* msg) {
  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char path[MAX_PATH] = {};
  lstrcpynA(path, dir, MAX_PATH);
  lstrcatA(path, "dx11_bridge_getgoing.log");
  FILE* f = nullptr;
  fopen_s(&f, path, "ab");
  if (f) {
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s\r\n",
      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
      tag ? tag : "dx11", msg ? msg : "");
    fclose(f);
  }
  char dbg[1024] = {};
  wsprintfA(dbg, "[%s] %s\n", tag ? tag : "dx11", msg ? msg : "");
  OutputDebugStringA(dbg);
}

inline HMODULE loadSystemDll(const char* name) {
  char sys[MAX_PATH] = {};
  GetSystemDirectoryA(sys, MAX_PATH);
  lstrcatA(sys, "\\");
  lstrcatA(sys, name);
  HMODULE mod = LoadLibraryA(sys);
  if (!mod) {
    char msg[512] = {};
    wsprintfA(msg, "LoadLibrary failed for %s err=%lu", sys, GetLastError());
    logLine("loader", msg);
  }
  return mod;
}

inline bool fileExists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

inline void launchBridgeServerOnce() {
  static LONG launched = 0;
  if (InterlockedCompareExchange(&launched, 1, 0) != 0) return;

  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char server[MAX_PATH] = {};
  lstrcpynA(server, dir, MAX_PATH);
  lstrcatA(server, ".trex\\NvRemixBridge.exe");

  if (!fileExists(server)) {
    logLine("bridge", "No .trex\\NvRemixBridge.exe next to d3d11.dll/dxgi.dll; running DX11 passthrough only.");
    return;
  }

  char cmd[MAX_PATH + 64] = {};
  wsprintfA(cmd, "\"%s\" --dx11", server);
  STARTUPINFOA si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
  if (ok) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logLine("bridge", "Started .trex\\NvRemixBridge.exe --dx11");
  } else {
    char msg[256] = {};
    wsprintfA(msg, "Failed to start NvRemixBridge.exe err=%lu", GetLastError());
    logLine("bridge", msg);
  }
}

} // namespace
