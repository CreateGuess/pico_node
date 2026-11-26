# 从本地源码包构建 GStreamer，构建过程禁止下载任何依赖。
include(ExternalProject)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
find_program(PICO_NINJA_EXECUTABLE NAMES ninja ninja-build REQUIRED)
find_program(PICO_FLEX_EXECUTABLE NAMES flex REQUIRED)
find_program(PICO_BISON_EXECUTABLE NAMES bison REQUIRED)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "内置 GStreamer 构建目前支持 Linux 本机编译；请在目标架构机器上构建，或使用 -DPICO_BUILD_GSTREAMER=OFF 和目标平台 SDK")
endif()

# 提前报告基础开发依赖缺失；这些依赖可以由目标系统的离线软件包安装。
pkg_check_modules(PICO_GST_PREREQUISITES REQUIRED
  glib-2.0>=2.64 gobject-2.0 gio-2.0 gmodule-2.0 gthread-2.0
  openssl libcrypto libjpeg vpx>=1.7.0 libsrtp2>=2.1.0)
set(_pico_archives "${PROJECT_SOURCE_DIR}/third_party/archives")
include("${PROJECT_SOURCE_DIR}/third_party/SourceVersions.cmake")
foreach(_pico_archive IN ITEMS gstreamer-1.24.12.tar.gz libnice-0.1.22.tar.gz)
  if(NOT EXISTS "${_pico_archives}/${_pico_archive}")
    message(FATAL_ERROR "缺少离线源码包：${_pico_archives}/${_pico_archive}")
  endif()
endforeach()
set(_pico_meson "${PROJECT_SOURCE_DIR}/third_party/meson/meson.py")
set(_pico_gst_prefix "${PROJECT_BINARY_DIR}/third_party/prefix")
set(_pico_gst_build "${PROJECT_BINARY_DIR}/third_party/gstreamer-build")
set(_pico_gst_source "${PROJECT_BINARY_DIR}/third_party/gstreamer-source")
set(_pico_gst_options
  --prefix=${_pico_gst_prefix} --libdir=lib --buildtype=release
  --default-library=shared --wrap-mode=nodownload -Dauto_features=disabled
  -Dbase=enabled -Dgood=enabled -Dbad=enabled -Dwebrtc=enabled
  -Dlibnice=enabled -Dlibnice:crypto-library=openssl
  -Dbuild-tools-source=system -Dorc=disabled -Dintrospection=disabled
  -Dtests=disabled -Dexamples=disabled -Ddoc=disabled -Dtools=enabled
  -Dgst-plugins-base:app=enabled -Dgst-plugins-base:videoconvertscale=enabled
  -Dgst-plugins-base:typefind=enabled
  -Dgst-plugins-good:jpeg=enabled -Dgst-plugins-good:vpx=enabled
  -Dgst-plugins-good:rtp=enabled -Dgst-plugins-good:rtpmanager=enabled
  -Dgst-plugins-bad:webrtc=enabled -Dgst-plugins-bad:dtls=enabled
  -Dgst-plugins-bad:srtp=enabled -Dgst-plugins-bad:sctp=enabled
  -Dgst-plugins-bad:sctp-internal-usrsctp=enabled)
set(PICO_DEPENDENCY_JOBS 2 CACHE STRING "第三方库并行编译任务数")
ExternalProject_Add(pico_gstreamer
  URL "${_pico_archives}/gstreamer-1.24.12.tar.gz"
  URL_HASH "SHA256=${PICO_GSTREAMER_SHA256}"
  SOURCE_DIR "${_pico_gst_source}"
  BINARY_DIR "${_pico_gst_build}"
  UPDATE_COMMAND ""
  PATCH_COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=<SOURCE_DIR>" "-DARCHIVE=${_pico_archives}/libnice-0.1.22.tar.gz"
    "-DEXPECTED_HASH=${PICO_LIBNICE_SHA256}"
    -P "${PROJECT_SOURCE_DIR}/cmake/PrepareLibnice.cmake"
  CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
    "${Python3_EXECUTABLE}" -B "${_pico_meson}" setup <BINARY_DIR> <SOURCE_DIR> ${_pico_gst_options}
  BUILD_COMMAND "${PICO_NINJA_EXECUTABLE}" -C <BINARY_DIR> -j "${PICO_DEPENDENCY_JOBS}"
  INSTALL_COMMAND "${Python3_EXECUTABLE}" -B "${_pico_meson}" install -C <BINARY_DIR> --no-rebuild
)

# GStreamer 安装后再配置应用，届时 pkg-config 才能读取生成的 .pc 文件。
set(_pico_child_args
  -DPICO_BUILD_GSTREAMER=OFF
  "-DPICO_DEPENDENCY_PREFIX=${PICO_DEPENDENCY_PREFIX}"
  "-DPICO_GSTREAMER_PREFIX=${_pico_gst_prefix}"
  "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
  "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
  "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${PROJECT_BINARY_DIR}"
  "-DCMAKE_BUILD_RPATH=${_pico_gst_prefix}/lib"
  "-Dxbus-dev_DIR=${xbus-dev_DIR}")
ExternalProject_Add(pico_application
  DEPENDS pico_gstreamer
  SOURCE_DIR "${PROJECT_SOURCE_DIR}"
  BINARY_DIR "${PROJECT_BINARY_DIR}/application"
  DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
  CMAKE_ARGS ${_pico_child_args}
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel "${PICO_DEPENDENCY_JOBS}"
  BUILD_ALWAYS TRUE
  INSTALL_COMMAND ""
)
# 用启动脚本设置插件和动态库路径，避免误加载机器上其他版本的插件。
configure_file("${PROJECT_SOURCE_DIR}/cmake/run_pico_node.sh.in"
  "${PROJECT_BINARY_DIR}/run_pico_node.sh" @ONLY)
message(STATUS "离线构建：先编译本地 GStreamer 源码，再构建 pico_node")
