#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace dx11_bridge_client {
void SetModule(HMODULE moduleHandle);
void LogLine(const char* tag, const char* text);
bool Attach();
bool EnsureServer();
void Detach();
HMODULE LoadSystemDll(const char* dllName);
}