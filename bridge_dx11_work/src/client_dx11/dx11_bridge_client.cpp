#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sstream>
#include <string>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "version.h"
#include "dx11_bridge_client.h"
#include "config/config.h"
#include "config/global_options.h"
#include "log/log.h"
#include "util_bridge_state.h"
#include "util_devicecommand.h"
#include "util_modulecommand.h"
#include "util_guid.h"
#include "util_process.h"
#include "util_semaphore.h"
#include "util_filesys.h"
#include "util_messagechannel.h"

using namespace bridge_util;

// DX11_V219_UNIFY_CLIENT_GUID_FOR_IPC
// The bridge IPC utilities and the launched x64 server must use the same global
// gUniqueIdentifier and gbBridgeRunning. Do not create a private namespace-static
// copy here, or the client will initialize queues under one GUID and launch the
// server under another GUID.
extern Guid gUniqueIdentifier;
extern bool gbBridgeRunning;

namespace dx11_bridge_client {

static HMODULE gModule = nullptr;
static std::mutex gAttachMutex;
static std::mutex gServerMutex;
static bool gAttached = false;
static std::string gRemixFolder;
static Process* gpServer = nullptr;
static bool gBridgeLaunchAttemptedV219 = false;
static NamedSemaphore* gpPresent = nullptr;
static std::chrono::steady_clock::time_point gTimeStart;

// DX11_V219_SINGLE_DLL_CLIENT_OWNER_NO_RELAUNCH
// d3d11.dll and dxgi.dll may both be loaded by the same game process. They are
// separate DLL modules with separate static state, so without a process-wide
// owner guard both can attach and launch their own x64 bridge server. The game
// should have exactly one bridge client owner per process.
static HANDLE gBridgeOwnerMutex = nullptr;
static bool gThisDllOwnsBridgeClient = false;

static std::string GetProcessBridgeOwnerMutexName() {
  char name[128] = {};
  sprintf_s(name, sizeof(name), "Local\\DX11RemixBridgeClientOwner_%lu", GetCurrentProcessId());
  return std::string(name);
}

static bool AcquireProcessBridgeClientOwnership() {
  if (gThisDllOwnsBridgeClient) return true;
  if (gBridgeOwnerMutex) return false;

  const std::string name = GetProcessBridgeOwnerMutexName();
  gBridgeOwnerMutex = CreateMutexA(nullptr, TRUE, name.c_str());
  const DWORD err = GetLastError();

  if (!gBridgeOwnerMutex) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "CreateMutexA bridge owner failed err=%lu; refusing duplicate client startup.", err);
    LogLine("bridge", msg);
    return false;
  }

  if (err == ERROR_ALREADY_EXISTS) {
    CloseHandle(gBridgeOwnerMutex);
    gBridgeOwnerMutex = nullptr;
    LogLine("bridge", "Another local DX11 bridge DLL already owns this process; this DLL will only forward system DX11/DXGI and will not launch another server.");
    return false;
  }

  gThisDllOwnsBridgeClient = true;
  LogLine("bridge", "This DLL acquired the single DX11 bridge client owner slot for the process.");
  return true;
}

static void ReleaseProcessBridgeClientOwnership() {
  if (gThisDllOwnsBridgeClient && gBridgeOwnerMutex) {
    ReleaseMutex(gBridgeOwnerMutex);
    CloseHandle(gBridgeOwnerMutex);
    gBridgeOwnerMutex = nullptr;
    gThisDllOwnsBridgeClient = false;
    LogLine("bridge", "Released single DX11 bridge client owner slot for the process.");
  } else if (gBridgeOwnerMutex) {
    CloseHandle(gBridgeOwnerMutex);
    gBridgeOwnerMutex = nullptr;
  }
}

static std::string GetFolderFromModule(HMODULE moduleHandle) {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(moduleHandle ? moduleHandle : gModule, path, MAX_PATH);
  char* slash = strrchr(path, '\\');
  if (!slash) slash = strrchr(path, '/');
  if (slash) slash[1] = 0;
  return std::string(path);
}

static void AppendClientLog(const char* text) {
  const std::string dir = gRemixFolder.empty() ? GetFolderFromModule(gModule) : gRemixFolder;
  const std::string path = dir + "dx11_bridge_client.log";
  FILE* f = nullptr;
  fopen_s(&f, path.c_str(), "ab");
  if (!f) return;
  SYSTEMTIME st; GetLocalTime(&st);
  fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
    text ? text : "");
  fclose(f);
}

void LogLine(const char* tag, const char* text) {
  char line[2048] = {};
  sprintf_s(line, sizeof(line), "[%s] %s", tag ? tag : "dx11-client", text ? text : "");
  AppendClientLog(line);
  OutputDebugStringA(line);
  OutputDebugStringA("\n");
  if (gAttached) {
    try { Logger::info(line); } catch (...) { }
  }
}

// DX11_V219_DEFINE_SERVER_MESSAGE_CHANNEL_FOR_DX11_ACK
// DX11 gets this helper from client/message_channels.h. The generated DX11
// bridge client is standalone, so it defines the same server-side message
// channel setup locally before using the DX11 Bridge_Ack sequence.
static std::unique_ptr<MessageChannelClient> gpServerMessageChannel;

static void initServerMessageChannel(const uint32_t serverThreadId) {
  gpServerMessageChannel = std::make_unique<MessageChannelClient>(static_cast<uint32_t>(serverThreadId));
  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "Server message channel initialized from Bridge_Ack pHandle/thread=%u.", serverThreadId);
  LogLine("bridge", msg);
}

// DX11_V219_SERVER_HOSTS_REMIX_RUNTIME
// The x64 bridge server must run as the host for the local .trex Remix runtime,
// not as a naked EXE launched from the game folder.  Set its working directory,
// DLL search path, and environment so it finds .trex\d3d11.dll, .trex\dxgi.dll,
// USD/plugin DLLs, and bridge files from the game-local package.
static void AddDllDirectoryCompatV219(const std::string& dir) {
  if (dir.empty()) return;
  SetDllDirectoryA(dir.c_str());
  HMODULE kernel = GetModuleHandleA("kernel32.dll");
  if (kernel) {
    using PFN_SetDefaultDllDirectories = BOOL (WINAPI*)(DWORD);
    using PFN_AddDllDirectory = DLL_DIRECTORY_COOKIE (WINAPI*)(PCWSTR);
    auto pSetDefaultDllDirectories = reinterpret_cast<PFN_SetDefaultDllDirectories>(GetProcAddress(kernel, "SetDefaultDllDirectories"));
    auto pAddDllDirectory = reinterpret_cast<PFN_AddDllDirectory>(GetProcAddress(kernel, "AddDllDirectory"));
    if (pSetDefaultDllDirectories && pAddDllDirectory) {
      pSetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
      wchar_t wdir[MAX_PATH] = {};
      MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, wdir, MAX_PATH);
      pAddDllDirectory(wdir);
    }
  }
}

static void PrependEnvPathV219(const char* name, const std::string& value) {
  if (!name || !name[0] || value.empty()) return;
  char oldValue[32767] = {};
  GetEnvironmentVariableA(name, oldValue, DWORD(sizeof(oldValue)));
  std::string merged = value;
  if (oldValue[0]) {
    merged += ";";
    merged += oldValue;
  }
  SetEnvironmentVariableA(name, merged.c_str());
}

static bool FileExistsV219(const std::string& path) {
  DWORD attr = GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirectoryExistsV219(const std::string& path) {
  DWORD attr = GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}


// DX11_V229_ATOMIC_GUID_VERSION_CONTRACT
static bool IsValidBridgeGuidV229(const std::string& guid) {
  if (guid.size() != 36) return false;
  for (size_t i = 0; i < guid.size(); ++i) {
    const char c = guid[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

static bool WriteBridgeTextFileAtomicallyV229(const std::string& path, const std::string& text) {
  const std::string temp = path + ".tmp";
  HANDLE h = CreateFileA(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL wrote = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  const BOOL flushed = wrote && written == text.size() && FlushFileBuffers(h);
  CloseHandle(h);
  if (!flushed) {
    DeleteFileA(temp.c_str());
    return false;
  }
  if (!MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileA(temp.c_str());
    return false;
  }
  return true;
}

static bool WriteSharedBridgeGuidV219(const std::string& gameRoot, const std::string& guid, const std::string& version) {
  if (gameRoot.empty() || !IsValidBridgeGuidV229(guid) || version.empty()) {
    LogLine("bridge", "DX11_V229 ERROR: refusing to launch bridge with an empty/invalid GUID or version.");
    return false;
  }
  SetEnvironmentVariableA("DX11_BRIDGE_GUID", guid.c_str());
  SetEnvironmentVariableA("DX11_BRIDGE_FIXED_GUID", guid.c_str());
  SetEnvironmentVariableA("DX11_BRIDGE_VERSION", version.c_str());

  const std::string guidPath = gameRoot + ".trex\\dx11_bridge_guid.txt";
  const std::string argsPath = gameRoot + ".trex\\dx11_bridge_args.txt";
  if (!WriteBridgeTextFileAtomicallyV229(guidPath, guid) ||
      !WriteBridgeTextFileAtomicallyV229(argsPath, guid + "\r\n" + version + "\r\n")) {
    LogLine("bridge", "DX11_V229 ERROR: failed to atomically publish bridge GUID/version files.");
    return false;
  }
  LogLine("bridge", "DX11_V229 atomically published one validated GUID/version contract for client, launcher, and server.");
  return true;
}

// DX11_V219_GAME_CMD_FILE_ANYWHERE
// Preserve the exact original game command line for games installed anywhere.
// The path may contain spaces, parentheses, Unicode, launcher parameters, etc.
// The launcher reads this file and passes the full content to .trex\NvRemixBridge.exe.
static std::string WriteSharedGameCommandLineV219(const std::string& gameRoot) {
  const std::string path = gameRoot + ".trex\\dx11_bridge_game_cmd.txt";
  const char* originalCmd = GetCommandLineA();
  HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(h, originalCmd, static_cast<DWORD>(strlen(originalCmd)), &written, nullptr);
    CloseHandle(h);
    LogLine("bridge", "DX11_V219 wrote original game command line to .trex\\dx11_bridge_game_cmd.txt.");
  } else {
    LogLine("bridge", "DX11_V219 WARNING: failed to write .trex\\dx11_bridge_game_cmd.txt.");
  }
  SetEnvironmentVariableA("DX11_BRIDGE_GAME_CMD_FILE", path.c_str());
  SetEnvironmentVariableA("DX11_BRIDGE_GAME_CMD", originalCmd);
  return path;
}

// DX11_V219_LAUNCHER_DUPLICATES_REAL_CLIENT_HANDLE
// The launcher owns the actual NvRemixBridge.exe process handle returned by
// CreateProcessW, so the launcher is the correct process to DuplicateHandle()
// the real game process handle into the server.  The game DLL only writes its
// PID and later reads the remote handle value prepared by the launcher.
static std::string WriteRealGamePidForLauncherV219(const std::string& gameRoot) {
  const std::string pidPath = gameRoot + ".trex\\dx11_bridge_game_pid.txt";
  const std::string remoteHandlePath = gameRoot + ".trex\\dx11_bridge_client_remote_handle.txt";
  DeleteFileA(remoteHandlePath.c_str());

  char pidText[64] = {};
  sprintf_s(pidText, "%lu", GetCurrentProcessId());

  HANDLE h = CreateFileA(pidPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(h, pidText, static_cast<DWORD>(strlen(pidText)), &written, nullptr);
    CloseHandle(h);
    LogLine("bridge", "DX11_V219 wrote real game PID to .trex\\dx11_bridge_game_pid.txt for launcher-side handle duplication.");
  } else {
    LogLine("bridge", "DX11_V219 WARNING: failed to write .trex\\dx11_bridge_game_pid.txt.");
  }

  SetEnvironmentVariableA("DX11_BRIDGE_GAME_PID", pidText);
  SetEnvironmentVariableA("DX11_BRIDGE_CLIENT_HANDLE_FILE", remoteHandlePath.c_str());
  return pidPath;
}

static bool ReadTextFileRawV219(const std::string& path, char* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD got = 0;
  BOOL ok = ReadFile(h, out, cap - 1, &got, nullptr);
  CloseHandle(h);
  if (!ok || got == 0) return false;
  out[got] = 0;
  while (got > 0 && (out[got - 1] == '\r' || out[got - 1] == '\n' || out[got - 1] == ' ' || out[got - 1] == '\t' || out[got - 1] == '\0')) {
    out[--got] = 0;
  }
  return out[0] != 0;
}

static uintptr_t WaitForLauncherDuplicatedClientHandleV219(const std::string& gameRoot) {
  const std::string path = gameRoot + ".trex\\dx11_bridge_client_remote_handle.txt";
  const DWORD selfPid = GetCurrentProcessId();

  for (int i = 0; i < 320; ++i) {
    char raw[512] = {};
    if (ReadTextFileRawV219(path, raw, sizeof(raw))) {
      // Format: serverPid|remoteHandle|gamePid
      char* first = strchr(raw, '|');
      if (!first) {
        Sleep(25);
        continue;
      }
      *first = 0;
      char* second = strchr(first + 1, '|');
      if (!second) {
        Sleep(25);
        continue;
      }
      *second = 0;

      DWORD serverPid = static_cast<DWORD>(strtoul(raw, nullptr, 10));
      uintptr_t remoteHandle = static_cast<uintptr_t>(_strtoui64(first + 1, nullptr, 0));
      DWORD gamePid = static_cast<DWORD>(strtoul(second + 1, nullptr, 10));

      if (serverPid != 0 && remoteHandle != 0 && gamePid == selfPid) {
        char msg[256] = {};
        sprintf_s(msg, sizeof(msg), "DX11_V219 read launcher-duplicated real game handle 0x%p for NvRemixBridge.exe pid=%lu.", reinterpret_cast<void*>(remoteHandle), serverPid);
        LogLine("bridge", msg);
        return remoteHandle;
      }

      char msg[256] = {};
      sprintf_s(msg, sizeof(msg), "DX11_V219 ignored stale remote-handle record serverPid=%lu handle=0x%p gamePid=%lu selfPid=%lu.", serverPid, reinterpret_cast<void*>(remoteHandle), gamePid, selfPid);
      LogLine("bridge", msg);
    }
    Sleep(25);
  }

  LogLine("bridge", "DX11_V219 ERROR: timed out waiting for launcher to duplicate real game handle into NvRemixBridge.exe.");
  return 0;
}


// DX11_V219_REAL_CLIENT_HANDLE_FOR_SERVER
// With DLL -> Launcher -> Bridge, the game DLL starts the launcher, and the
// launcher starts the real x64 server. The Bridge_Syn handle must be duplicated
// into the REAL NvRemixBridge.exe process, not into the launcher path.
static bool ReadTextFileSmallV219(const std::string& path, char* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD got = 0;
  BOOL ok = ReadFile(h, out, cap - 1, &got, nullptr);
  CloseHandle(h);
  if (!ok || got == 0) return false;
  out[got] = 0;

  // DX11_V219_PID_FILE_ASCII
  // Be robust if an older launcher wrote UTF-16LE digits: compact decimal
  // characters out of the buffer so 6\0 4\0 8\0 0\0 becomes 6480.
  char compact[64] = {};
  DWORD ci = 0;
  for (DWORD i = 0; i < got && ci + 1 < sizeof(compact); ++i) {
    if (out[i] >= '0' && out[i] <= '9') {
      compact[ci++] = out[i];
    }
  }
  compact[ci] = 0;
  if (ci > 0) {
    lstrcpynA(out, compact, cap);
    return true;
  }

  while (got > 0 && (out[got - 1] == '\r' || out[got - 1] == '\n' || out[got - 1] == ' ' || out[got - 1] == '\t' || out[got - 1] == '\0')) out[--got] = 0;
  return out[0] != 0;
}

// DX11_V219_PID_RECORD_WITH_BRIDGE_PATH
static bool NormalizePathLowerV219(char* s) {
  if (!s || !s[0]) return false;
  for (char* p = s; *p; ++p) {
    if (*p == '/') *p = '\\';
    if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');
  }
  return true;
}

static DWORD WaitForBridgeServerPidV219(const std::string& gameRoot) {
  const std::string path = gameRoot + ".trex\\dx11_bridge_server_pid.txt";
  char expected[MAX_PATH * 3] = {};
  lstrcpynA(expected, (gameRoot + ".trex\\NvRemixBridge.exe").c_str(), sizeof(expected));
  NormalizePathLowerV219(expected);

  for (int i = 0; i < 240; ++i) {
    char raw[4096] = {};
    if (ReadTextFileSmallV219(path, raw, sizeof(raw))) {
      char rawCopy[4096] = {};
      lstrcpynA(rawCopy, raw, sizeof(rawCopy));

      char* sep = strchr(rawCopy, '|');
      if (sep) {
        *sep = 0;
        DWORD pid = static_cast<DWORD>(strtoul(rawCopy, nullptr, 10));
        char* writtenPath = sep + 1;
        NormalizePathLowerV219(writtenPath);
        if (pid != 0 && strstr(writtenPath, expected) != nullptr) {
          return pid;
        }
        char msg[512] = {};
        sprintf_s(msg, sizeof(msg), "DX11_V219 ignored stale/wrong server PID record pid=%lu path=%s expected=%s.", pid, writtenPath, expected);
        LogLine("bridge", msg);
      } else {
        // Backward compatibility with V219 ASCII/UTF-16 digit-only files.
        DWORD pid = static_cast<DWORD>(strtoul(raw, nullptr, 10));
        if (pid != 0) return pid;
      }
    }
    Sleep(25);
  }
  return 0;
}


// DX11_V219_PID_FILE_ASCII
// DX11_V219_PID_RECORD_WITH_BRIDGE_PATH
static bool IsNvRemixBridgeProcessPidV219(DWORD pid) {
  if (!pid) return false;

  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (!h) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 could not query server PID %lu err=%lu.", pid, GetLastError());
    LogLine("bridge", msg);
    return false;
  }

  DWORD wait = WaitForSingleObject(h, 0);
  if (wait != WAIT_TIMEOUT) {
    CloseHandle(h);
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 rejected server PID %lu because the process is not running.", pid);
    LogLine("bridge", msg);
    return false;
  }

  char path[MAX_PATH] = {};
  DWORD size = MAX_PATH;
  BOOL ok = QueryFullProcessImageNameA(h, 0, path, &size);
  CloseHandle(h);

  if (!ok || !path[0]) {
    // On some x86->x64 cases, image query can fail even when OpenProcess works.
    // The PID record already includes the expected .trex path, so do not reject
    // solely on image-query failure.
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 could not query image for server PID %lu; accepting PID record from launcher.", pid);
    LogLine("bridge", msg);
    return true;
  }

  const char* slash = strrchr(path, '\\');
  const char* name = slash ? slash + 1 : path;
  if (_stricmp(name, "NvRemixBridge.exe") == 0) {
    return true;
  }

  char msg[512] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219 image-name check saw pid=%lu image=%s; accepting launcher PID record already matched .trex bridge path.", pid, path);
  LogLine("bridge", msg);
  return true;
}

static uintptr_t DuplicateCurrentGameProcessHandleIntoServerV219(DWORD serverPid) {
  if (!serverPid) return 0;
  if (!IsNvRemixBridgeProcessPidV219(serverPid)) {
    char pidMsg[256] = {};
    sprintf_s(pidMsg, sizeof(pidMsg), "DX11_V219 rejected server PID %lu because it was not a valid running .trex bridge server PID.", serverPid);
    LogLine("bridge", pidMsg);
    return 0;
  }
  HANDLE hServer = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, serverPid);
  if (!hServer) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 failed to open real NvRemixBridge.exe pid=%lu for handle duplication err=%lu.", serverPid, GetLastError());
    LogLine("bridge", msg);
    return 0;
  }
  HANDLE remoteClientHandle = nullptr;
  BOOL ok = DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), hServer, &remoteClientHandle, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0);
  CloseHandle(hServer);
  if (!ok || !remoteClientHandle) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 failed to duplicate real game process handle into bridge server pid=%lu err=%lu.", serverPid, GetLastError());
    LogLine("bridge", msg);
    return 0;
  }
  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219 duplicated real game process handle 0x%p into NvRemixBridge.exe pid=%lu.", remoteClientHandle, serverPid);
  LogLine("bridge", msg);
  return reinterpret_cast<uintptr_t>(remoteClientHandle);
}


static void PrepareRemixServerRuntimeEnvironmentV219(const std::string& gameRoot, const std::string& trexRoot) {
  AddDllDirectoryCompatV219(trexRoot);
  AddDllDirectoryCompatV219(gameRoot);

  PrependEnvPathV219("PATH", trexRoot + ";" + gameRoot);
  SetEnvironmentVariableA("DXVK_REMIX_GAME_DIR", gameRoot.c_str());
  SetEnvironmentVariableA("DXVK_REMIX_TREX_DIR", trexRoot.c_str());
  SetEnvironmentVariableA("DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", "1");

  if (DirectoryExistsV219(trexRoot + "plugins")) {
    SetEnvironmentVariableA("PXR_PLUGINPATH_NAME", (trexRoot + "plugins").c_str());
  }
  if (DirectoryExistsV219(trexRoot + "usd")) {
    SetEnvironmentVariableA("USD_ROOT", (trexRoot + "usd").c_str());
  }

  if (!FileExistsV219(trexRoot + "NvRemixBridge.exe")) {
    LogLine("bridge", "DX11_V219 ERROR: .trex\\NvRemixBridge.exe is missing; server cannot host Remix runtime.");
  }
  if (!FileExistsV219(trexRoot + "d3d11.dll")) {
    LogLine("bridge", "DX11_V219 WARNING: .trex\\d3d11.dll is missing; x64 Remix DX11 runtime may not load.");
  }
  if (!FileExistsV219(trexRoot + "dxgi.dll")) {
    LogLine("bridge", "DX11_V219 WARNING: .trex\\dxgi.dll is missing; x64 Remix DXGI runtime may not load.");
  }

  LogLine("bridge", "DX11_V219: prepared .trex as x64 Remix runtime/server host root.");
}


// DX11_V219_DLL_LAUNCHER_BRIDGE_CHAIN
// The game does not run NvRemixLauncher32.exe directly.  The game loads this
// root DLL; this DLL starts NvRemixLauncher32.exe from the same game folder as
// the required x86 launcher/client helper.  The launcher must not relaunch the
// game; it is a child/helper for the DLL-loaded client path.
static HANDLE gLauncherClientProcess = nullptr;

static bool IsProcessStillRunningV219(HANDLE h) {
  if (!h) return false;
  DWORD code = 0;
  if (!GetExitCodeProcess(h, &code)) return false;
  return code == STILL_ACTIVE;
}

static void EnsureLauncherClientBridgeStarterV219(const std::string& gameRoot) {
  if (IsProcessStillRunningV219(gLauncherClientProcess)) return;
  if (gLauncherClientProcess) {
    CloseHandle(gLauncherClientProcess);
    gLauncherClientProcess = nullptr;
  }

  const std::string launcherPath = gameRoot + "NvRemixLauncher32.exe";
  if (!FileExistsV219(launcherPath)) {
    LogLine("bridge", "DX11_V219 ERROR: NvRemixLauncher32.exe missing beside game exe; DLL cannot start required launcher/client helper.");
    return;
  }

  std::string cmd = "\"" + launcherPath + "\" --dx11-launch-bridge --game-root \"" + gameRoot + "\"";
  STARTUPINFOA si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);

  BOOL ok = CreateProcessA(
    launcherPath.c_str(),
    cmd.empty() ? nullptr : cmd.data(),
    nullptr,
    nullptr,
    FALSE,
    CREATE_NO_WINDOW,
    nullptr,
    gameRoot.c_str(),
    &si,
    &pi);

  if (!ok) {
    char msg[512] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 ERROR: failed to start NvRemixLauncher32.exe helper from DLL, err=%lu", GetLastError());
    LogLine("bridge", msg);
    return;
  }

  CloseHandle(pi.hThread);
  gLauncherClientProcess = pi.hProcess;
  LogLine("bridge", "DX11_V219: root DLL started NvRemixLauncher32.exe launcher/client helper.");
}
void SetModule(HMODULE moduleHandle) {
  gModule = moduleHandle;
  if (gRemixFolder.empty() && moduleHandle) gRemixFolder = GetFolderFromModule(moduleHandle);
}

HMODULE LoadSystemDll(const char* dllName) {
  char sys[MAX_PATH] = {};
  GetSystemDirectoryA(sys, MAX_PATH);
  strcat_s(sys, "\\");
  strcat_s(sys, dllName);
  HMODULE mod = LoadLibraryA(sys);
  if (!mod) {
    char msg[512] = {};
    sprintf_s(msg, sizeof(msg), "LoadLibraryA('%s') failed err=%lu", sys, GetLastError());
    LogLine("loader", msg);
  }
  return mod;
}

static void OnServerExited(Process const*) {
  // DX11_V219_D3D11_CLIENT_LIFETIME
  // Do not permanently disable the bridge client if the server host reloads/exits.
  // Keep the D3D11 client alive so the next D3D11 device call can reconnect.
  BridgeState::setServerState(BridgeState::ProcessState::Exited);
  if (gpServer) {
    delete gpServer;
    gpServer = nullptr;
  }
  gbBridgeRunning = true;
  LogLine("bridge", "DX11_V219 bridge/launcher exited; keeping D3D11 client alive but not auto-opening another bridge in this same game process.");
}

bool Attach() {
  std::lock_guard<std::mutex> lock(gAttachMutex);
  if (gAttached) return true;
  if (!gModule) GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCSTR>(&Attach), &gModule);
  gRemixFolder = GetFolderFromModule(gModule);
  gTimeStart = std::chrono::high_resolution_clock::now();

  if (!AcquireProcessBridgeClientOwnership()) {
    gAttached = false;
    return false;
  }

  try {
    Config::init(Config::App::Client, gModule);
    GlobalOptions::init();
    Logger::init();
    dxvk::util::RtxFileSys::init(gRemixFolder);
    Logger::info("==================\nDX11 RTX Remix Bridge Client\n==================");
    Logger::info(std::string("Version: ") + std::string(BRIDGE_VERSION));
    Logger::info(std::string("Loaded DX11 bridge client from ") + gRemixFolder);
    initModuleBridge();
    initDeviceBridge();
    gpPresent = new NamedSemaphore("Present", 0, GlobalOptions::getPresentSemaphoreMaxFrames());
    BridgeState::setClientState(BridgeState::ProcessState::Init);
  } catch (...) {
    LogLine("bridge", "Attach failed while initializing bridge config/logger/IPC.");
    ReleaseProcessBridgeClientOwnership();
    return false;
  }

  gAttached = true;
  LogLine("bridge", "DX11 bridge client attached and IPC queues initialized.");
  return true;
}

bool EnsureServer() {
  std::lock_guard<std::mutex> lock(gServerMutex);
  if (gpServer) return true;
  if (gBridgeLaunchAttemptedV219) {
    LogLine("bridge", "DX11_V219 bridge launch was already attempted in this game process; not opening another launcher/bridge.");
    return true;
  }
  if (!Attach()) return false;
  if (!gThisDllOwnsBridgeClient) return false;

  const std::string trexRoot = gRemixFolder + ".trex\\";
  const std::string serverPath = trexRoot + "NvRemixBridge.exe";
  const std::string launcherPath = gRemixFolder + "NvRemixLauncher32.exe";

  // DX11_V219_DLL_LAUNCHER_BRIDGE_CHAIN
  // Required chain:
  //   game -> root d3d11/dxgi DLL -> NvRemixLauncher32.exe -> .trex\NvRemixBridge.exe
  // The DLL must not directly start .trex\NvRemixBridge.exe.
  PrepareRemixServerRuntimeEnvironmentV219(gRemixFolder, trexRoot);

  if (GetFileAttributesA(launcherPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    LogLine("bridge", "Missing NvRemixLauncher32.exe beside root DLL; DX11 DLL cannot start launcher/client helper.");
    return false;
  }
  if (GetFileAttributesA(serverPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    LogLine("bridge", "Missing .trex\\NvRemixBridge.exe; launcher cannot start Remix server.");
    return false;
  }

  SetEnvironmentVariableA("DX11_BRIDGE_MODE", "d3d11");
  const std::string sharedGuidV219 = gUniqueIdentifier.toString();
  if (!WriteSharedBridgeGuidV219(gRemixFolder, sharedGuidV219, BRIDGE_VERSION)) {
    return false;
  }

  const std::string gameCmdFileV219 = WriteSharedGameCommandLineV219(gRemixFolder);
  WriteRealGamePidForLauncherV219(gRemixFolder);

  std::stringstream cmdSS;
  cmdSS << '"' << launcherPath << '"';
  cmdSS << " --dx11-launch-bridge";
  cmdSS << " --game-root " << '"' << gRemixFolder << '"';
  cmdSS << " --trex-root " << '"' << trexRoot << '"';
  cmdSS << " --guid " << sharedGuidV219;
  cmdSS << " --version " << BRIDGE_VERSION;
  cmdSS << " --game-cmd-file " << '"' << gameCmdFileV219 << '"';
  const std::string command = cmdSS.str();

  char launchLine[4096] = {};
  sprintf_s(launchLine, sizeof(launchLine), "DX11_V219 DLL launching NvRemixLauncher32.exe with shared DX11-style GUID %s and --game-cmd-file; launcher will start .trex bridge: %s", sharedGuidV219.c_str(), command.c_str());
  LogLine("bridge", launchLine);

  gBridgeLaunchAttemptedV219 = true;

  try {
    gpServer = new Process(command.c_str(), OnServerExited);
  } catch (...) {
    LogLine("bridge", "Process() failed to create NvRemixLauncher32.exe launcher/client helper.");
    gpServer = nullptr;
    return false;
  }

  // DX11_V319_LAUNCH_FAILURE_IS_VISIBLE: Process does NOT throw when
  // CreateProcess fails - it just stores INVALID_HANDLE_VALUE - so the catch
  // above can never fire, and a launcher that never started was indistinguishable
  // from one that did. The client went on to wait out the full handle-duplication
  // timeout and then reported "launcher did not provide a real game handle
  // duplicated into NvRemixBridge.exe", blaming the handshake for a process that
  // was never created. Field signature (Saints Row the Third): no
  // NvRemixLauncher32.log, no .trex\dx11_bridge_server_pid.txt, ~9s stall, then
  // that error. Ask the object whether the launch actually happened.
  if (gpServer == nullptr || !gpServer->isValid()) {
    LogLine("bridge",
      "DX11_V319 ERROR: NvRemixLauncher32.exe did not start - CreateProcess failed, so the .trex "
      "bridge server was never launched and Remix cannot load. The preceding 'CreateProcess FAILED' "
      "line carries the error code and the exact command line; check that NvRemixLauncher32.exe sits "
      "beside the game executable and is not blocked by security software.");
    delete gpServer;
    gpServer = nullptr;
    return false;
  }

  BridgeState::setServerState(BridgeState::ProcessState::Init);
  LogLine("bridge", "Sending Bridge_Syn and waiting for Bridge_Ack from .trex x64 server on DeviceBridge using DX11 DLL->Launcher->.trex Bridge_Ack sequence. DX11_V219_USE_DX11_BRIDGE_ACK_SEQUENCE");
  uintptr_t realClientHandleForServerV219 = WaitForLauncherDuplicatedClientHandleV219(gRemixFolder);
  if (!realClientHandleForServerV219) {
    LogLine("bridge", "DX11_V219 ERROR: launcher did not provide a real game handle duplicated into NvRemixBridge.exe; refusing legacy launcher-handle fallback.");
    return false;
  }
  {
    ClientMessage syn(Commands::Bridge_Syn, realClientHandleForServerV219);
  }
  BridgeState::setClientState(BridgeState::ProcessState::Handshaking);

  // DX11_V219_USE_DX11_BRIDGE_ACK_SEQUENCE
  // Match NVIDIA's working DX11 bridge handshake:
  //   Bridge_Syn -> DeviceBridge::waitForCommand(Bridge_Ack)
  //   DeviceBridge::pop_front()
  //   initServerMessageChannel(ackResponse.pHandle)
  //   Bridge_Continue
  const auto waitForAck = DeviceBridge::waitForCommand(Commands::Bridge_Ack, GlobalOptions::getStartupTimeout());
  if (waitForAck != Result::Success) {
    LogLine("bridge", "Timed out or failed waiting for Bridge_Ack from x64 server on DeviceBridge.");
    BridgeState::setServerState(BridgeState::ProcessState::DoneProcessing);
    gbBridgeRunning = false;
    return false;
  }

  const auto ackResponse = DeviceBridge::pop_front();
  initServerMessageChannel(ackResponse.pHandle);
  BridgeState::setServerState(BridgeState::ProcessState::Handshaking);
  LogLine("bridge", "Bridge_Ack received through DX11 DeviceBridge path using shared DX11-style bridge GUID; server message channel initialized; sending Bridge_Continue.");
  {
    ClientMessage cont(Commands::Bridge_Continue);
  }

  BridgeState::setClientState(BridgeState::ProcessState::Running);
  BridgeState::setServerState(BridgeState::ProcessState::Running);
  LogLine("bridge", "DX11 bridge client/server handshake completed.");
  return true;
}

void Detach() {
  std::lock_guard<std::mutex> lock(gServerMutex);
  if (gAttached) {
    BridgeState::setClientState(BridgeState::ProcessState::DoneProcessing);
    if (gpServer) {
      LogLine("bridge", "Sending Bridge_Terminate to x64 server.");
      gpServer->UnregisterExitCallback();
      { ClientMessage term(Commands::Bridge_Terminate); }
      DeviceBridge::waitForCommandAndDiscard(Commands::Bridge_Ack, GlobalOptions::getCommandTimeout());
      delete gpServer;
      gpServer = nullptr;
    }
    gpServerMessageChannel.reset();
    delete gpPresent;
    gpPresent = nullptr;
    BridgeState::setClientState(BridgeState::ProcessState::Exited);
    gAttached = false;
    ReleaseProcessBridgeClientOwnership();
  } else {
    ReleaseProcessBridgeClientOwnership();
  }
}

}