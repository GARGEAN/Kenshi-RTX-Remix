dx11_client_inc = include_directories('.')
thread_dep = dependency('threads')

dx11_common_src = files(['dx11_bridge_client.cpp', 'dx11_bridge_client_globals.cpp'])

d3d11_dx11_bridge = shared_library('d3d11',
  files(['d3d11_dx11bridge.cpp', 'dx11_capture_remix.cpp']) + dx11_common_src,
  sources: [bridge_version],
  dependencies: [thread_dep, util_dep, lib_version, lib_commCtrl, tracy_dep],
  include_directories: [bridge_include_path, util_include_path, public_include_path, dx11_client_inc],
  cpp_args: ['/DREMIX_BRIDGE_CLIENT', '/DDX11_BRIDGE_CLIENT', '/DDX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER'],
  install: true,
  link_args: ['d3dcompiler.lib'],
  vs_module_defs: 'd3d11_dx11bridge.def')

dxgi_dx11_bridge = shared_library('dxgi',
  files(['dxgi_dx11bridge.cpp']) + dx11_common_src,
  sources: [bridge_version],
  dependencies: [thread_dep, util_dep, lib_version, lib_commCtrl, tracy_dep],
  include_directories: [bridge_include_path, util_include_path, public_include_path, dx11_client_inc],
  cpp_args: ['/DREMIX_BRIDGE_CLIENT', '/DDX11_BRIDGE_CLIENT', '/DDX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER'],
  install: true,
  vs_module_defs: 'dxgi_dx11bridge.def')