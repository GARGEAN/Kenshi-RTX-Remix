d3d11_res = wrc_generator.process('version.rc')

dxgi_common_src = [
  '../dxgi/dxgi_format.cpp',
  '../dxgi/dxgi_monitor.cpp',
  '../dxgi/dxgi_swapchain.cpp',
]

d3d11_src = [
  'd3d11_annotation.cpp',
  'd3d11_blend.cpp',
  'd3d11_buffer.cpp',
  'd3d11_class_linkage.cpp',
  'd3d11_cmdlist.cpp',
  'd3d11_context.cpp',
  'd3d11_context_def.cpp',
  'd3d11_context_ext.cpp',
  'd3d11_context_imm.cpp',
  'd3d11_cuda.cpp',
  'd3d11_depth_stencil.cpp',
  'd3d11_device.cpp',
  'd3d11_enums.cpp',
  'd3d11_fence.cpp',
  'd3d11_gdi.cpp',
  'd3d11_initializer.cpp',
  'd3d11_input_layout.cpp',
  'd3d11_kenshi_ogre_culling.cpp',
  'd3d11_interop.cpp',
  'd3d11_main.cpp',
  'd3d11_multithread.cpp',
  'd3d11_options.cpp',
  'd3d11_query.cpp',
  'd3d11_rasterizer.cpp',
  'd3d11_resource.cpp',
  'd3d11_rtx.cpp',
  'd3d11_sampler.cpp',
  'd3d11_shader.cpp',
  'd3d11_state.cpp',
  'd3d11_state_object.cpp',
  'd3d11_swapchain.cpp',
  'd3d11_texture.cpp',
  'd3d11_util.cpp',
  'd3d11_video.cpp',
  'd3d11_view_dsv.cpp',
  'd3d11_view_rtv.cpp',
  'd3d11_view_srv.cpp',
  'd3d11_view_uav.cpp',
]

d3d11_shaders = files([
  'shaders/d3d11_video_blit_frag.frag',
  'shaders/d3d11_video_blit_vert.vert',
])

# DX11_V580: d3d11_rtx.cpp reached the COFF section limit (fatal error C1128)
# and no longer accepts ANY addition without this. It is a container-format
# limit, not an optimisation switch: /bigobj only widens the section count
# field, and MSVC has emitted it for x64 objects since VS2005.
d3d11_extra_cpp_args = dxvk_is_msvc ? ['/bigobj'] : []

d3d11_rtx_include_path = include_directories('../dxvk', '../../public/include')
d3d11_nrc_include_path = include_directories('../../submodules/nrc/Include')

d3d11_dll = shared_library('d3d11'+dll_ext, dxgi_common_src + d3d11_src,
    glsl_generator.process(d3d11_shaders), d3d11_res,
  name_prefix         : '',
  cpp_args            : d3d11_extra_cpp_args,
  link_args           : [ usd_plugin_delay_load_args, remix_satellite_delay_load_args, '/DEPENDENTLOADFLAG:0x1100' ],
  dependencies        : [ lib_dxgi, dxbc_dep, dxvk_dep ],
  include_directories : [ dxvk_include_path, dxvk_shader_include_path, usd_include_paths, lssusd_include_paths, d3d11_rtx_include_path, d3d11_nrc_include_path ],
  install             : true,
  vs_module_defs      : 'd3d11'+def_spec_ext,
  override_options    : ['cpp_std='+dxvk_cpp_std])

d3d11_dep = declare_dependency(
  link_with           : [ d3d11_dll ],
  include_directories : [ dxvk_include_path ])
