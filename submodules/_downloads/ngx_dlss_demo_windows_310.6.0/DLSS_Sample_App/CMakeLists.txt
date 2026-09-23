cmake_minimum_required(VERSION 3.12)

project(ngx_dlss_demo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(DONUT_WITH_FEATURE_DEMO OFF)
option(DONUT_WITH_ASSIMP "" OFF)

set(CMAKE_INSTALL_PREFIX ${PROJECT_SOURCE_DIR}/DLSS_Sample_App/)
set(CMAKE_INSTALL_LIBDIR ${CMAKE_INSTALL_PREFIX}/donut/lib)
set(CMAKE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_PREFIX}/donut/include)
set(CMAKE_INSTALL_BINDIR ${CMAKE_INSTALL_PREFIX}bin/ngx_dlss_demo)

set(DEPENDENCIES_ROOT "${PROJECT_SOURCE_DIR}/donut/thirdparty")

install(DIRECTORY Media;PostInstall DESTINATION ".")
install(FILES CMakeLists.txt README.md DESTINATION ".")

add_subdirectory(donut)
add_subdirectory(ngx_dlss_demo)

# MUST BE LAST
add_subdirectory(PostInstall)
