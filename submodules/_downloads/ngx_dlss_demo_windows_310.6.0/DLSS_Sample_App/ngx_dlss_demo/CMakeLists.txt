add_executable(ngx_dlss_demo WIN32 DemoMain.cpp NGXWrapper.h NGXWrapper.cpp glue/RenderTargets.hpp glue/UIRenderer.hpp)
target_link_libraries(ngx_dlss_demo donut_core donut_engine donut_render donut_app)
target_include_directories(ngx_dlss_demo PUBLIC . ./glue)

if(DONUT_WITH_DX11 OR DONUT_WITH_DX12)
    target_link_libraries(ngx_dlss_demo dxgi dxguid)
endif()

if (NOT DONUT_WITH_NGX)
    message(FATAL_ERROR "Donut must be built with NGX")
endif()

set(__ngx_dlss_demo_libs "")
if(DONUT_WITH_ASSIMP)
    set(__ngx_dlss_demo_libs ${__ngx_dlss_demo_libs} assimp)
endif()
if(DONUT_WITH_HBAO_PLUS)
    if(DONUT_WITH_DX11)
        set(__ngx_dlss_demo_libs ${__ngx_dlss_demo_libs} hbao_plus_d3d11)
    endif()
    if(DONUT_WITH_DX12)
        set(__ngx_dlss_demo_libs ${__ngx_dlss_demo_libs} hbao_plus_d3d12)
    endif()
endif()
if(DONUT_WITH_VOLUME_LIGHTING)
    if(DONUT_WITH_DX11)
        set(__ngx_dlss_demo_libs ${__ngx_dlss_demo_libs} volumelighting_d3d11)
    endif()
    if(DONUT_WITH_DX12)
        set(__ngx_dlss_demo_libs ${__ngx_dlss_demo_libs} volumelighting_d3d12)
    endif()
endif()
if(DONUT_WITH_NGX)
    set(__ngx_dlss_demo_libs ${__ngx_dlss_demo_libs} ngx)
endif()

# xxxnsubtil: haven't been able to abstract this into a function or macro
# due to the $<TARGET_FILE_DIR:> generator expression
foreach(lib ${__ngx_dlss_demo_libs})
    get_target_property(lib_type ${lib} TYPE)
    if ("${lib_type}" STREQUAL "SHARED_LIBRARY")
        get_property(dll_file TARGET ${lib} PROPERTY IMPORTED_LOCATION)

        add_custom_command(TARGET ngx_dlss_demo
                           COMMENT "Dependant DLLs copy ${dll_file} $<TARGET_FILE_DIR:ngx_dlss_demo>"
                           COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll_file}" "$<TARGET_FILE_DIR:ngx_dlss_demo>")

        STRING(REGEX REPLACE "\\\\" "/" dll_file ${dll_file})
        install(FILES ${dll_file} DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif ()
endforeach()

if(DONUT_WITH_NGX)
    # Need to copy all libraries NGX needs
    get_property(__NGX_DLLS_LIST TARGET ngx PROPERTY EXTRA_DLLS)
    foreach(dll_file ${__NGX_DLLS_LIST})
        add_custom_command(TARGET ngx_dlss_demo
                            COMMENT "NGX DLL copy ${dll_file} $<TARGET_FILE_DIR:ngx_dlss_demo>\n"
                            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll_file}" "$<TARGET_FILE_DIR:ngx_dlss_demo>")
        STRING(REGEX REPLACE "\\\\" "/" dll_file ${dll_file})
        install(FILES ${dll_file} DESTINATION ${CMAKE_INSTALL_BINDIR})
    endforeach()
endif()

add_custom_command(TARGET ngx_dlss_demo
                   COMMENT "MEDIA copy Media  $<TARGET_FILE_DIR:ngx_dlss_demo>\n"
                   COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/media" "$<TARGET_FILE_DIR:ngx_dlss_demo>/media")

set_target_properties(ngx_dlss_demo PROPERTIES FOLDER "Donut DLSS Demo")

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /MP /we4838 /Zc:ternary")
endif()


install(TARGETS ngx_dlss_demo RUNTIME DESTINATION bin/ngx_dlss_demo)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" DESTINATION ".")
