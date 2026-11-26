cmake_minimum_required(VERSION 3.18)
project(pico_node LANGUAGES C CXX)

option(PICO_BUILD_GSTREAMER "从本地源码离线构建 GStreamer" ON)
# 可将目标平台的 SDK 安装到项目内，也可指定外部安装目录。
set(PICO_DEPENDENCY_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/third_party/prefix" CACHE PATH "第三方 SDK 安装前缀")
list(PREPEND CMAKE_PREFIX_PATH "${PICO_DEPENDENCY_PREFIX}")

find_package(xbus-dev 2.0 CONFIG REQUIRED COMPONENTS client)
find_package(Threads REQUIRED)
if(PICO_BUILD_GSTREAMER)
  include(cmake/BundledGStreamer.cmake)
  return()
endif()
add_executable(pico_node src/main.cpp)
target_compile_features(pico_node PRIVATE cxx_std_17)
target_include_directories(pico_node PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pico_node PRIVATE xbus::client Threads::Threads)

add_subdirectory(third_party)
find_package(PkgConfig REQUIRED)
# 优先查找本地 SDK 的元数据，同时保留系统搜索路径。
set(PICO_GSTREAMER_PREFIX "${PICO_DEPENDENCY_PREFIX}" CACHE PATH "已安装的 GStreamer SDK 前缀")
set(_pico_pkgconfig_dirs
  "${PICO_GSTREAMER_PREFIX}/lib/pkgconfig"
  "${PICO_GSTREAMER_PREFIX}/lib64/pkgconfig"
  "${PICO_GSTREAMER_PREFIX}/share/pkgconfig"
  "${PICO_DEPENDENCY_PREFIX}/lib/pkgconfig"
  "${PICO_DEPENDENCY_PREFIX}/lib64/pkgconfig"
  "${PICO_DEPENDENCY_PREFIX}/share/pkgconfig")
if(CMAKE_LIBRARY_ARCHITECTURE)
  list(APPEND _pico_pkgconfig_dirs "${PICO_DEPENDENCY_PREFIX}/lib/${CMAKE_LIBRARY_ARCHITECTURE}/pkgconfig")
endif()
if(CMAKE_HOST_WIN32)
  set(_pico_path_separator ";")
else()
  set(_pico_path_separator ":")
endif()
string(JOIN "${_pico_path_separator}" _pico_pkgconfig_path ${_pico_pkgconfig_dirs})
set(ENV{PKG_CONFIG_PATH} "${_pico_pkgconfig_path}${_pico_path_separator}$ENV{PKG_CONFIG_PATH}")
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0>=1.20 gstreamer-app-1.0>=1.20
  gstreamer-webrtc-1.0>=1.20 gstreamer-sdp-1.0>=1.20
  gstreamer-video-1.0>=1.20)
target_sources(pico_node PRIVATE src/webrtc_server.cpp)
target_compile_definitions(pico_node PRIVATE GST_USE_UNSTABLE_API
  PICO_WEB_PAGE="${CMAKE_CURRENT_BINARY_DIR}/web/index.html")
target_link_libraries(pico_node PRIVATE PkgConfig::GST pico_third_party::httplib pico_third_party::json)
configure_file(web/index.html web/index.html COPYONLY)
