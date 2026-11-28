# 第三方依赖

## 已随项目提供

| 依赖 | 版本 | 文件 | 许可证 |
|---|---|---|---|
| cpp-httplib | 0.18.3 | `cpp-httplib/httplib.h` | `cpp-httplib/LICENSE` |
| nlohmann/json | 3.11.3 | `nlohmann-json/include/nlohmann/json.hpp` | `nlohmann-json/LICENSE.MIT` |

直接通过本目录的 CMake 接口目标使用，编译时不下载，不再要求系统安装这两个库。第三方原始文件保持上游内容，包括版权、许可证和原始注释；项目自己编写的代码统一采用中文注释。

官方来源：

- https://github.com/yhirose/cpp-httplib/tree/v0.18.3
- https://github.com/nlohmann/json/tree/v3.11.3

## 离线 GStreamer 源码

已提供以下本地源码，不使用 Git 子模块，也不会在编译时下载：

| 组件 | 版本 | 本地文件 |
|---|---|---|
| GStreamer 完整单仓库源码 | 1.24.12 | `archives/gstreamer-1.24.12.tar.gz` |
| libnice ICE 库源码 | 0.1.22 | `archives/libnice-0.1.22.tar.gz` |
| Rockchip MPP | `9b56348cc61393c84662e6d6dd78feae67773a36` | `archives/rockchip-mpp.tar.gz` |
| Rockchip GStreamer 插件 | `d33af1daedf36247782d57d6539bdd1162a1dd07` | `archives/gstreamer-rockchip.tar.gz` |
| Meson 构建工具源码 | 1.5.2 | `meson/`，原始包保存在 `archives/meson-1.5.2.tar.gz` |

GStreamer 源码包含核心、基础插件、优质插件及扩展插件，保留完整上游目录和许可证。源码包在构建目录解压，避免在 Windows 上解压符号链接破坏 Linux 源码。libnice 在配置前放入 GStreamer 的子项目目录；Meson 设置 `--wrap-mode=nodownload`，缺少依赖时直接报错，不联网回退。

默认 `PICO_BUILD_GSTREAMER=ON`，CMake 先构建安装 GStreamer 和 libnice，再配置编译应用。生成目录全部位于 build：

```text
build/
├── third_party/gstreamer-source/
├── third_party/gstreamer-build/
├── third_party/prefix/
├── application/
├── pico_node
├── config/pico_node.json
└── web/index.html
```

使用 `./build/pico_node` 启动，从程序旁 `config/` 读取配置；程序自行设置插件和扫描器，动态库使用相对 RPATH，避免混用系统其他版本。构建目录不能跨架构复用；不同机器分别从同一套源码本机编译。当前未提供交叉编译工具链。

### 必须提前准备的基础环境

离线构建不等于空系统可以直接编译。以下基础工具和开发库仍由目标 Linux 系统提供：

- C/C++ 编译器、CMake 3.18+、Python 3.8+、Ninja、pkg-config、Flex、Bison。
- GLib/GObject/GIO 2.64+、OpenSSL、libjpeg、libsrtp2 2.1+、libdrm 的开发包及传递依赖。
- 与机器架构匹配的 XBus 2.0 SDK 和运行服务。

Ubuntu/Debian 包名示例：

```bash
sudo apt install build-essential cmake python3 ninja-build pkg-config flex bison \
  libglib2.0-dev libssl-dev libjpeg-dev libsrtp2-dev libdrm-dev
```

离线机器请事先通过系统镜像或同发行版、同架构的完整离线软件包安装这些基础依赖；上述命令仅说明所需包名，不是 CMake 自动执行的步骤。本项目不要求系统安装 GStreamer 开发包或插件包，也不附带上述基础开发库的源码及 DEB 文件。

### 使用已有 SDK

设置 `-DPICO_BUILD_GSTREAMER=OFF` 可使用已安装的 GStreamer 1.20+。`prefix/` 仍可存放目标平台的 XBus / GStreamer SDK，或用 `-DPICO_DEPENDENCY_PREFIX=/path/to/sdk` 指定其他目录；可单独用 `-DPICO_GSTREAMER_PREFIX=/path/to/gstreamer` 指定媒体 SDK。

更新任何源码包或单头文件库时，同步更新版本、许可证和 `SHA256SUMS`；GStreamer、libnice 的校验值还需更新 `SourceVersions.cmake`，再验证编译及播放。

源码来源：

- https://github.com/GStreamer/gstreamer/tree/1.24.12
- https://libnice.freedesktop.org/releases/libnice-0.1.22.tar.gz
- https://github.com/mesonbuild/meson/releases/tag/1.5.2

## Rockchip 硬件编码

MPP 源码来自 https://github.com/rockchip-linux/mpp/tree/9b56348cc61393c84662e6d6dd78feae67773a36 ，保留归档内 `LICENSES/`（Apache-2.0、MIT）及逐文件版权声明。插件来自 https://github.com/JeffyCN/mirrors/tree/d33af1daedf36247782d57d6539bdd1162a1dd07 ，保留 `COPYING` 和各文件许可证。摘要同时记录在 `SourceVersions.cmake` 与 `SHA256SUMS`。

默认构建次序为 GStreamer → MPP → Rockchip 插件 → 应用。插件仅启用 MPP；RGA、GPU、显示输出及 VPX Alpha 功能关闭。CPU 将 JPEG 转换为 NV12，再由 MPP 调用硬件编码 H.264。基础系统无需 libvpx；新增 h264parse 由本地 GStreamer 的 videoparsers 插件提供。

用户空间源码不能代替板级内核驱动。目标板需提供与 MPP 兼容的编码设备及访问权限；如厂商内核与该固定版本不兼容，应使用经厂商验证的 MPP 源码包更新版本和校验值，重新构建，不能混入另一套 GStreamer 插件。当前环境未完成 ARM 编译与硬件出流验收。
