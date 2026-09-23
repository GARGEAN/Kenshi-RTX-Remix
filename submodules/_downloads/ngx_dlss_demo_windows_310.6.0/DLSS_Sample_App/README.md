NGX DLSS Sample
=======
This is sample code showing how to implement NVIDIA Deep Learning Super Sampling (DLSS) for feature enhancement, anti-aliasing, and upscaling. It uses NVIDIA NGX for library linking and is written within the NVIDIA Donut framework.

Obtain the NGX DLSS SDK from https://github.com/NVIDIA/DLSS.

## Windows

**Prereqs:**
- Windows 10 RS5
- Visual Studio 2017(V141)+ with updates
- Cmake 3.12+
- Python 3.7 (64-Bit)

**Steps to run:**
1. Download the source package.
2. In `bin\ngx_dlss_demo` run `ngx_dlss_demo.exe`
3. To change Graphics APIs, use a command-line window and add the parameters: `-d3d11`, `-d3d12`, or `-vulkan` to the command.

**Steps to Build:**
1. Install CMake (https://cmake.org/)
2. Open CMake-GUI, set the source location as the package root, and choose an output location.
3. Ensure a 64-Bit compiler, by using the drop down.
4. Click "Configure"
5. Update `NGX_SDK_ROOT` to point to the root NGX SDK directory, then Configure again.
6. Click "Generate", then "Open Project"
7. Set `Donut DLSS Demo/ngx_dlss_demo` as startup project and Build.

**Troubleshooting Build Problems**
- Ensure you have an up-to-date version of the NGX SDK
- Certain newer versions of dxc will fail to compile the `donut_shaders` project due to being stricter on certain things. You may encounter problems
  building the `donut_shaders` project if the version of Visual Studio you are using is too new. You can work around this by setting `DXC_BIN_DXIL`
  to point to the same path as `DXC_BIN_SPIRV` in CMake.

**Running Code Built in Visual Studio:**
1. Set `Donut DLSS Demo/ngx_dlss_demo` as startup project and Run.

*OR*

1. Build the desired configuration
	- Available Configurations: Release, RelWithDbgInfo, MinSizeRel, Debug
2. Open a command-line window in the root output directory.
3. Run `ngx_dlss_demo\<Configuration>\ngx_dlss_demo.exe`
	- Replace `<Configuration>` with the configuration that was built (For example, for the Debug configuration: `ngxx_dlss_demo\Debug\ngx_dlss_demo.exe`)
4. To change graphics APIs, add the parameters: `-d3d11`, `-d3d12`, or `-vulkan` to the command.

**Troubleshooting Problems Running the NGX DLSS Sample**
- Ensure you have the required files in the media directory in the root of the repro. Some files in the media directory are tracked using LFS, and you will
  need to set up LFS to obtain them. You can set up LFS and obtain the binary files by running the following from the root of the repo:

  `git lfs install
  cd media
  git lfs pull`

 Then copy the contents of `<repo-root>\media` into `ngx_dlss_demo\<Configuration>\Media` replacing what was already there.

**Notes**
* Uses the NGX recommended scale factor.
* Supports d3d11, d3d12, and vulkan.
* The directory structure of the project cannot be changed, as relative paths are used in several places.


## Linux

**Steps to build:**
1. Update dependencies:
	- `cd <SAMPLE_APP_DIR>/donut/packman`
	- `sh update-dependencies.sh`
2. Run CMake:
	- `cd <SAMPLE_APP_DIR>`
	- `mkdir build && cd build`
	- `cmake -DNGX_SDK_ROOT=<YOUR_NGX_DLSS_SDK_DIR> ..`

**Steps to run:**
1. Start X. E.g:
	- `export DISPLAY=:0`
	- `X -retro -noreset & sleep 3; xset s off; xset dpms 0 0 0 -dpms`
2. Under the `build` directory from *Steps to build* section:
	- `cd ngx_dlss_demo`
	- `export DISPLAY=:0`
	- `./ngx_dlss_demo -w 1920 -h 1080`

**Notes**
* Uses the NGX recommended scale factor.
* Supports vulkan.
* The directory structure of the project cannot be changed, as relative paths are used in several places.
