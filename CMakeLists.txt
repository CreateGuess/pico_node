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
add_executable(pico_node src/main.cpp src/app_config.cpp src/media_environment.cpp)
option(PICO_BUNDLED_MEDIA "使用随程序部署的媒体 SDK" OFF)
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
  PICO_BUNDLED_MEDIA=$<BOOL:${PICO_BUNDLED_MEDIA}>)
target_link_libraries(pico_node PRIVATE PkgConfig::GST pico_third_party::httplib pico_third_party::json)
# 网页、配置和动态库均相对于程序部署，不依赖启动时所在目录。
set(_pico_runtime "${CMAKE_CURRENT_BINARY_DIR}")
if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(_pico_runtime "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()
configure_file(web/index.html "${_pico_runtime}/web/index.html" COPYONLY)
# 已有运行配置可能由部署人员修改，重新配置工程时不覆盖。
if(NOT EXISTS "${_pico_runtime}/config/pico_node.json")
  configure_file(config/pico_node.json "${_pico_runtime}/config/pico_node.json" COPYONLY)
endif()
if(PICO_BUNDLED_MEDIA)
  # 使用可传递的 DT_RPATH，同时解决插件间接依赖；路径随部署目录移动。
  set_target_properties(pico_node PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/third_party/prefix/lib")
  target_link_options(pico_node PRIVATE "LINKER:--disable-new-dtags")
endif()
