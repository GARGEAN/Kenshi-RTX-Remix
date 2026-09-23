dx11_bridge_inc = include_directories('.')

d3d11_dx11bridge_dll = shared_library('d3d11',
  files('d3d11_dx11bridge.cpp'),
  files('d3d11_dx11bridge.def'),
  include_directories: [util_include_path, dx11_bridge_inc],
  cpp_args: ['/EHsc'],
  dependencies: [lib_version],
  install: false
)

dxgi_dx11bridge_dll = shared_library('dxgi',
  files('dxgi_dx11bridge.cpp'),
  files('dxgi_dx11bridge.def'),
  include_directories: [util_include_path, dx11_bridge_inc],
  cpp_args: ['/EHsc'],
  dependencies: [lib_version],
  install: false
)

run_target('copy_dx11_bridge_client',
  depends: [d3d11_dx11bridge_dll, dxgi_dx11bridge_dll],
  command: [copy_script_path, meson.current_build_dir().replace('\\', '/'), output_dir, 'd3d11*'])

run_target('copy_dxgi_bridge_client',
  depends: [dxgi_dx11bridge_dll],
  command: [copy_script_path, meson.current_build_dir().replace('\\', '/'), output_dir, 'dxgi*'])
