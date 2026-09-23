@echo off
cd /d "C:\Users\icetr\Downloads\dxvk-remix-DX11\bridge_dx11_work\_Comp64Release"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "PATH=C:\Users\icetr\Downloads\dxvk-remix-DX11\.build_deps\python\bin;%PATH%"
ninja "src/server/NvRemixBridge.exe" 2>&1
echo EXITCODE=%ERRORLEVEL%
