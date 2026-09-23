@echo off
cd /d "C:\Users\icetr\Downloads\dxvk-remix-DX11\bridge_dx11_work\_Comp64Release"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "NINJA=C:\Users\icetr\Downloads\dxvk-remix-DX11\.build_deps\python\bin\ninja.exe"
"%NINJA%" "src/server/NvRemixBridge.exe.p/main.cpp.obj"
