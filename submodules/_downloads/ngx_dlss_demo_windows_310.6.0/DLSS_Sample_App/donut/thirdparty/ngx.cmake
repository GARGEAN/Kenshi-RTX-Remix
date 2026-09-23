set(NGX_SDK_ROOT NGX-SDK-ROOT-NOTFOUND CACHE STRING "NGX SDK Root Directory")

if ("${NGX_SDK_ROOT}" STREQUAL "NGX-SDK-ROOT-NOTFOUND")
  message(FATAL_ERROR "NGX_SDK_ROOT not set - please set it and rerun CMAKE Configure")
endif()

if (WIN32)
  set(NGX_USE_STATIC_MSVCRT OFF CACHE BOOL "[Deprecated?]Use NGX libs with static VC runtime (/MT), otherwise dynamic (/MD)")

  add_library(ngx IMPORTED SHARED GLOBAL)

  set_property(TARGET ngx APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
  set_property(TARGET ngx APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)

  if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
  if(NGX_USE_STATIC_MSVCRT)
    set_target_properties(ngx PROPERTIES IMPORTED_IMPLIB_DEBUG ${NGX_SDK_ROOT}/lib/Windows_x86_64/x86_64/nvsdk_ngx_s_dbg.lib)
    set_target_properties(ngx PROPERTIES IMPORTED_IMPLIB_RELEASE ${NGX_SDK_ROOT}/lib/Windows_x86_64/x86_64/nvsdk_ngx_s.lib)
  else()
    set_target_properties(ngx PROPERTIES IMPORTED_IMPLIB_DEBUG ${NGX_SDK_ROOT}/lib/Windows_x86_64/x86_64/nvsdk_ngx_d_dbg.lib)
    set_target_properties(ngx PROPERTIES IMPORTED_IMPLIB_RELEASE ${NGX_SDK_ROOT}/lib/Windows_x86_64/x86_64/nvsdk_ngx_d.lib)
  endif()
  else()
    if(NGX_USE_STATIC_MSVCRT)
      set_target_properties(ngx PROPERTIES IMPORTED_IMPLIB_DEBUG ${NGX_SDK_ROOT}/lib/${CMAKE_GENERATOR_PLATFORM}/nvsdk_ngx_s_dbg.lib)
      set_target_properties(ngx PROPERTIES IMPORTED_IMPLIB_RELEASE ${NGX_SDK_ROOT}/lib/${CMAKE_GENERATOR_PLATFORM}/nvsdk_ngx_s.lib)
    else()
      set_target_properties(ngx PROPERTIES IMPORTED_IMPLIB_DEBUG ${NGX_SDK_ROOT}/lib/${CMAKE_GENERATOR_PLATFORM}/nvsdk_ngx_d_dbg.lib)
      set_target_properties(ngx PROPERTIES IMPORTED_IMPLIB_RELEASE ${NGX_SDK_ROOT}/lib/${CMAKE_GENERATOR_PLATFORM}/nvsdk_ngx_d.lib)
    endif()
  endif()
  set_target_properties(ngx PROPERTIES
    MAP_IMPORTED_CONFIG_MINSIZEREL Release
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
  )

  # TODO: It seems this component should be configured as STATIC rather than SHARED
  #       and IMPORTED_LOCATION point to the actual SDK static library that applications
  #       will link against
  if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
  set_target_properties(ngx PROPERTIES IMPORTED_LOCATION "${NGX_SDK_ROOT}/lib/Windows_x86_64/$<IF:$<CONFIG:Debug>,dev,rel>/nvngx_dlss.dll")
  else()
    set_target_properties(ngx PROPERTIES IMPORTED_LOCATION "${NGX_SDK_ROOT}/lib/Windows_${CMAKE_GENERATOR_PLATFORM}/$<IF:$<CONFIG:Debug>,dev,rel>/default/nvngx_dlss.dll")
  endif()    
else ()
  add_library(ngx IMPORTED STATIC GLOBAL)

  set_property(TARGET ngx APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
  set_property(TARGET ngx APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
  set_target_properties(ngx PROPERTIES IMPORTED_LOCATION ${NGX_SDK_ROOT}/lib/Linux_x86_64/libnvsdk_ngx.a)
  # TODO: Need debug static library
  # set_target_properties(ngx PROPERTIES IMPORTED_LOCATION ${NGX_SDK_ROOT}/lib/Linux_x86_64/libnvsdk_ngx.a)

  # set the list of DLLs that need copying to target folder of application
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(GLOB __NGX_DLLS_LIST "${NGX_SDK_ROOT}/lib/Linux_x86_64/dev/libnvidia-ngx-*.so.*")
  else()
    file(GLOB __NGX_DLLS_LIST "${NGX_SDK_ROOT}/lib/Linux_x86_64/rel/libnvidia-ngx-*.so.*")
  endif()
endif()

set_property(TARGET ngx APPEND PROPERTY EXTRA_DLLS "${__NGX_DLLS_LIST}")

target_include_directories(ngx INTERFACE ${NGX_SDK_ROOT}/include)
