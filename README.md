# pico_node · 相机接收端

在运行 XBus 2.0.0 的 Linux 接收机上运行。此目录可以单独复制到该机。程序订阅导入话题，按秒打印累计收到的帧数和 JPEG 边界检查失败数，也支持限时保存一张图。接收端无需 OpenCV。

## 目录结构

```text
pico_node/
├── include/                 # 项目头文件
│   ├── cam_frame.hpp         # CJPG 协议及 JPEG 正文解析
│   └── webrtc_server.hpp
├── src/                     # 项目实现文件
│   ├── main.cpp
│   └── webrtc_server.cpp
├── third_party/             # 第三方头文件、许可证及 SDK 安装目录
│   ├── cpp-httplib/
│   ├── nlohmann-json/
│   ├── archives/             # GStreamer、libnice、Meson 原始源码包
│   ├── meson/                # 离线构建工具
│   ├── prefix/
│   └── CMakeLists.txt
├── config/pico_node.json    # 默认启动配置
├── web/index.html
└── CMakeLists.txt
```

## 构建

安装本机架构的 XBus 2.0.0 runtime、hub、tools、dev 四个 DEB，以及 CMake 和 C++17 编译器。WebRTC 为固定编译功能，默认从本地源码编译 GStreamer，需提前准备下文的基础开发依赖。进入本项目目录：

```bash
sudo apt update
sudo apt install -y libjpeg-dev libvpx-dev libsrtp2-dev

sudo apt install -y libsrtp2-dev libdrm-dev
sudo apt install libdrm-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
source /etc/profile.d/xbus.sh
```

## 连接相机机并接收

已有配对时用 `xbusctl link peers` 查看**接收机上**相机对端的导入前缀。首次配对时，相机机运行 `sudo xbusctl link pair --listen --restart`，接收机运行 `sudo xbusctl link pair <相机机IP> --import-prefix cam. --restart`（替换尖括号参数）。在相机机终端核对并输入请求码，两端服务将重启。然后在相机机启动 `camera_node 0` 或 `camera_node --test`。

```bash
xbusctl link status
xbusctl link capabilities --remote
# 配置 mode 为 once，output 为 ../frame.jpg，然后启动
./build/pico_node
file build/frame.jpg
# 配置 mode 为 receive，然后启动
./build/pico_node
```

目录中应看到来源名 `camera.jpeg`。若本机前缀是 `remote.`，订阅名应写 `remote.camera.jpeg`；多对端诊断命令需加 `--peer-id ID`。配置 `mode` 为 `once` 时等待最多 30 秒，收到一帧后写入指定路径（覆盖同名文件），失败返回非零。持续接收用 Ctrl+C 停止。

出现 `Subscribed` 但帧数持续为零时，依次核对导入前缀、远端目录、发布程序是否持续采集，以及两端 schema 是否一致。可用 `xbusctl link topics --watch` 检查流量和丢弃。修改代码前阅读 [AGENTS.md](AGENTS.md)。

## WebRTC 网页播放（纯 C++）

单个 `pico_node` 进程直接订阅 XBus，用 cpp-httplib 提供网页与 HTTP SDP 信令，使用 GStreamer `webrtcbin` 实现 WebRTC。无需 Python、OpenCV 或外部媒体子进程。

媒体链路：JPEG → jpegdec → videoconvert / videoscale（NV12）→ mpph264enc → h264parse → RTP → webrtcbin → 浏览器 video。网页位于 `web/index.html`，构建时复制到可执行文件旁的 `web/` 目录。

### 依赖与构建

默认直接使用 `third_party/archives/` 中的 GStreamer 1.24.12、libnice 0.1.22、Rockchip MPP 与 Rockchip GStreamer 插件源码，以及本地 Meson 工具。构建期间禁止联网下载，不再要求系统安装 GStreamer 开发包和插件包。httplib、JSON 也随项目提供。

目标机需提前具备 XBus 2.0 SDK、CMake 3.18+、C++17 编译器、Python 3.8+、Ninja、Flex、Bison 及基础开发库。在联网的 Ubuntu/Debian 准备机上可安装以下包；离线目标机需预装或使用匹配发行版与架构的完整离线包：

```bash
sudo apt install build-essential cmake python3 ninja-build pkg-config flex bison \
  libglib2.0-dev libssl-dev libjpeg-dev libsrtp2-dev libdrm-dev
```

在目标 Linux 机器本机编译：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
source /etc/profile.d/xbus.sh
./build/pico_node
```

CMake 先解压、校验并构建本地 GStreamer，安装到 `build/third_party/prefix/`，再编译本地 MPP 与 Rockchip 插件，最后配置编译应用，最终程序位于 `build/pico_node`。第三方库默认使用 2 个编译任务，可通过 `-DPICO_DEPENDENCY_JOBS=4` 调整。Python 仅用于 Meson 构建，运行服务仍为 C++ 单进程。

浏览器打开 `http://接收机IP:8080`，点击“连接视频”。程序通过相对 RPATH 加载媒体库，自行配置插件、扫描器和独立注册缓存，无需启动脚本。换架构或复制到另一台机器时，应复制整个源码目录后重新构建，不要复制旧的 build 目录。

若希望使用系统现有 GStreamer 1.20+：

```bash
cmake -S . -B build-system -DPICO_BUILD_GSTREAMER=OFF
cmake --build build-system -j2
./build-system/pico_node
```

也可指定 `-DPICO_GSTREAMER_PREFIX=/path/to/gstreamer`。统计、单帧保存和网页播放统一通过配置文件选择，所有构建均包含媒体依赖。第三方源码版本、基础依赖及离线边界见 [第三方依赖说明](third_party/README.md)。

### 配置与直接启动

默认读取可执行文件同目录的 `config/pico_node.json`，从任意工作目录启动均可。源码中的配置作为初次构建模板，已有构建目录的配置不会在重新编译时被覆盖。

```json
{
  "topics": ["cam.camera.jpeg", "rear.camera.jpeg"],
  "mode": "web",
  "output": "../frame.jpg",
  "web": { "port": 8080, "page": "../web/index.html" },
  "h264": { "bitrate": 2000000, "gop": 30 }
}
```

`topics` 按配置顺序填写相机话题，不能重复或为空。程序同时订阅全部话题，逐路解析相同的 CJPG/JPEG 消息；网页卡片名称直接使用 topic。旧版 `"topic": "cam.camera.jpeg"` 仍兼容，但新配置建议统一使用数组。

`mode` 为 `web`（网页）、`receive`（仅统计）或 `once`（保存一帧）；路径相对于配置文件目录。H.264 固定为 1280×720、标称 30 FPS、Baseline 编码并经解析器约束为 Constrained Baseline、Level 3.1；`bitrate` 单位为 bit/s，`gop` 为关键帧间隔。当前只使用 `mpph264enc`，缺少硬件插件直接报错，不自动使用 OpenH264 或 x264。

```bash
cd build
./pico_node
```

部署时保留 `pico_node`、`config/`、`web/`、`third_party/prefix/` 的相对位置，XBus 服务及系统基础库仍需安装。修改构建输出中的 `config/pico_node.json` 后重启生效。程序入口为 `int main()`，全部运行设置均以配置文件为准，启动命令统一为 `./pico_node`。

### NanoPC-T6 的硬件前提

已将以下固定源码打包在 `third_party/archives/`，CMake 离线编译，不依赖系统已有的 Rockchip 插件：

- Rockchip MPP：`9b56348cc61393c84662e6d6dd78feae67773a36`。
- Rockchip GStreamer 插件：`d33af1daedf36247782d57d6539bdd1162a1dd07`。

板载内核仍须提供兼容的 MPP 编码驱动和设备权限。用户提供的系统 GStreamer 1.20.3 列表只有 `openh264enc` 软件编码器；这不能证明硬件编码链路可用。当前电脑无法执行 ARM 编码，首次在目标板构建后需要验证内核驱动兼容性、关键帧、多人播放和实际 CPU 降幅。

构建完成可先独立检查本次构建的插件及驱动（这些环境变量仅用于独立诊断工具，程序直接启动不需要）：

```bash
export LD_LIBRARY_PATH="$PWD/build/third_party/prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GST_PLUGIN_SYSTEM_PATH_1_0="$PWD/build/third_party/prefix/lib/gstreamer-1.0"
export GST_PLUGIN_PATH_1_0="$GST_PLUGIN_SYSTEM_PATH_1_0"
export GST_PLUGIN_SCANNER_1_0="$PWD/build/third_party/prefix/libexec/gstreamer-1.0/gst-plugin-scanner"
./build/third_party/prefix/bin/gst-inspect-1.0 mpph264enc
./build/third_party/prefix/bin/gst-launch-1.0 -e videotestsrc num-buffers=90 \
  ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 \
  ! mpph264enc profile=baseline level=3.1 rc-mode=cbr bps=2000000 gop=30 \
  ! h264parse ! fakesink
ls -l /dev/mpp_service /dev/mpp-service /dev/dma_heap /dev/dri
```

设备节点名称因内核而异；诊断时报找不到某个节点，并不单独构成失败结论。以硬件测试管线是否正常结束及 MPP 错误日志判断。若使用 `PICO_BUILD_GSTREAMER=OFF`，必须自行准备与系统 GStreamer 匹配的 `mpph264enc`，不能把内置 1.24 插件直接放入系统 1.20 的插件目录。

### 运行行为

- 接收回调仅校验 JPEG 边界、统计并保留最新 SharedBuffer；主循环负责投递媒体数据。
- 每个 topic 最多 4 个观看会话，同一 topic 的会话共享一套 JPEG 解码和 H.264 硬件编码管线，各会话独立 RTP 打包和 WebRTC 发送，输出固定 1280×720 H.264，目标码率 2 Mbps。每约 33ms 投递最新图像；图源较慢时播放帧率随之降低，不补帧。
- 接收槽、appsrc 和媒体队列均限制积压，在 JPEG 输入端丢弃旧帧，编码帧不任意丢弃；慢会话积压超过限制后回收。`received` 为通过边界检查的 XBus 消息数，不代表已成功解码或浏览器呈现数。
- SDP 采用非 trickle ICE：双方收集完候选后交换 SDP。网页必须提供 H.264 video 接收能力。
- 网页主动断开会删除服务端会话；失败、断开、媒体管线报错和 35 秒内未连接的会话会被回收。媒体错误写到 stderr，该观看者需要重新连接。
- `/api/status` 的 `cameras` 数组按配置顺序返回每个 topic 的消息计数、会话数、最近消息时间和编码状态，顶层返回消息总数与会话总数；`source_alive` 表示最近 5 秒内有通过边界检查的消息。图源停更后画面可能停在最后一帧。

默认绑定所有网卡，仅适合可信局域网，无账号认证。需开放 TCP 网页端口和 ICE 使用的动态 UDP 端口。不配置外部 STUN/TURN；跨 NAT/公网访问需要另外配置 webrtcbin 和浏览器的 ICE 服务，以及 HTTPS、鉴权和网络访问控制。

### 验收与排查

先将配置中的 `mode` 设为 `once`、`output` 设为 `../frame.jpg`，运行程序保存图像，确认通过两个直连 Hub 收到的 JPEG 可以打开。再使用远端 `camera_node --test`，从另一台局域网机器打开网页，确认序号持续变化；验证断开重连、多个浏览器、关闭图源和 Ctrl+C 退出，最后连接真实相机。

缺少插件时启动会报告具体名称，可用 `gst-inspect-1.0 webrtcbin`、`gst-inspect-1.0 mpph264enc`、`gst-inspect-1.0 nicesrc` 排查。详细媒体日志可用：

```bash
GST_DEBUG=webrtcbin:5 ./build/pico_node
```

实现参考：[cpp-httplib](https://github.com/yhirose/cpp-httplib)、[GStreamer webrtcbin API](https://gstreamer.freedesktop.org/documentation/webrtc/)。

若 `received=0` 但 `invalid` 持续增加，说明已经触发接收回调，但消息未满足原始 JPEG 契约。程序只输出首条无效消息的长度、失败原因及首尾各最多 16 字节，日志格式为 `JPEG 校验失败：size=... reason=... head=[...] tail=[...]`。请与发送端消息格式对照，检查是否带自定义头、尾部填充、编码文本或分片；不能仅凭话题名认定正文就是完整 JPEG。

### 相机消息格式

当前相机话题为 `camera.jpeg`，接收端导入前缀为 `cam.` 时订阅 `cam.camera.jpeg`，schema 仍为 `camera.jpeg@v1`。实际前缀以接收机配置为准。

正文由 20 字节 CJPG 头和原始 JPEG 组成：

| 偏移 | 字段 | 格式 |
|---|---|---|
| 0–3 | 魔数 | ASCII `CJPG` |
| 4–7 | 帧序号 seq | u32 小端 |
| 8–15 | 采集到发布的板内耗时 age_ns | u64 小端 |
| 16–17 | width | u16 小端 |
| 18–19 | height | u16 小端 |
| 20 起 | JPEG 正文 | 以 FF D8 开头、FF D9 结尾 |

接收端按字节解析头部，整条消息上限为 8 MiB（包含头部）。仍兼容旧发送端无头的原始 JPEG。单帧保存不会写入 CJPG 头，WebRTC 解码器也只接收 JPEG 正文。头部字段已解析，但当前未用于丢帧统计或调整固定输出尺寸；age_ns 不用于跨板时钟相减。

```bash
cmake --build build -j2
# 配置 mode 为 once，output 为 ../frame.jpg，然后启动
./build/pico_node
file build/frame.jpg
# 配置 mode 改回 web 后启动
./build/pico_node
```

网页视频左上角显示播放帧率，每秒按浏览器呈现帧数增量计算。优先使用视频帧回调，旧浏览器回退到播放质量计数（总帧数减丢帧数）；没有新帧时显示 0.0 FPS，断开和重连时清零。该数值是浏览器播放帧率，不是 XBus 接收消息速率。

### 多观看者的 CPU 与共享编码

每个 topic 只有一个共享 H.264 硬件编码器，首个已协商会话收到图像时创建；无观看者时不投递 JPEG。第二个及后续观看者复用同一份编码数据，仅增加各自的 RTP 打包、SRTP 加密及网络发送开销。H.264 编码由 Rockchip MPP 调用硬件完成；JPEG 解码、像素转换、缩放、SRTP 和网络发送仍会消耗 CPU。

新观看者等待关键帧后才开始发送，浏览器的关键帧请求会转交共享编码器。慢会话积压超过 2 MiB 时回收，需要重新连接，避免任意丢弃编码参考帧。某路共享管线报错会报告 topic 并停止该路编码，其他相机继续播放；修复设备问题后重启服务恢复该路。

验收时依次连接 1、2、4 个页面，观察每页画面及 FPS、`top -H` 中的 CPU，验证中途加入、断开重连、丢包恢复和慢客户端隔离。可查看状态接口：

```bash
curl http://127.0.0.1:8080/api/status
```

`cameras` 数组中每一路的 `codec` 返回 `H264`，`encoder` 返回 `mpph264enc`。`encoder_instances` 在首次播放前为 0，启动后固定为 1；`encoded_frames` 是所有观看者共享的编码总帧数，不应乘以观看人数。各会话仍使用同一码率，不支持按观看者分别调整编码质量。

### 多相机页面与配置迁移

页面采用浅色卡片布局，支持手机单列、桌面多列。进入页面自动连接所有已配置相机；支持每路独立连接、断开、全屏，也支持全部连接、全部断开。每路显示 topic、播放 FPS、接收帧数、无效消息和服务端观看会话数。“图源在线”表示最近 5 秒内收到有效消息；“本页播放”表示本页已经开始播放的视频数量。

已有构建目录的配置不会自动覆盖，因此升级时需要手动修改程序旁的 `config/pico_node.json`，例如：

```json
"topics": ["cam.camera.jpeg", "cam2.camera.jpeg", "rear.camera.jpeg"]
```

删除原 `topic` 字段后添加 `topics`，重启程序并刷新网页。程序只按配置中的话题列表订阅，不接受命令行覆盖。`once` 模式为每路各保存一帧，多路输出使用 `frame.1.jpg`、`frame.2.jpg` 等配置顺序编号；某路 30 秒内没有图像仍保存其他已接收图像，并返回失败。

每路均使用独立的 1280×720 H.264 编码器；增加相机数量会增加硬件编码会话、JPEG 解码和网络开销，可用路数受板端硬件与驱动限制。同一相机增加观看页面仍复用编码器。验收时需同时发送内容不同的测试图，确认各卡片不串流，关闭其中一路后其他画面和计数持续更新，并验证未知 topic 被拒绝、逐路重连和全部断开。

### 配置 VP8

配置文件支持 `"video": { "codec": "vp8", "bitrate": 2000000, "gop": 30 }`，修改后重启程序。codec 可选 `h264`（默认，MPP 硬件编码）或 `vp8`（vp8enc 软件编码）。兼容旧 h264 配置块；存在 video 时优先读取 video。码率单位 bit/s，gop 为最大关键帧间隔。两种编码均使用 1280×720、标称 30 FPS。

离线构建 GStreamer 前需安装目标平台的 `libvpx-dev`（Debian/Ubuntu）；构建启用 vpx 插件。系统 GStreamer 模式需提供 vp8enc、rtpvp8pay。选择 VP8 时不检查 MPP 运行时插件，缺少所选编码插件会报错，不会自动切换编码。浏览器按状态接口返回的 codec 协商。
