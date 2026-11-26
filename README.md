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
├── web/index.html
└── CMakeLists.txt
```

## 构建

安装本机架构的 XBus 2.0.0 runtime、hub、tools、dev 四个 DEB，以及 CMake 和 C++17 编译器。WebRTC 为固定编译功能，默认从本地源码编译 GStreamer，需提前准备下文的基础开发依赖。进入本项目目录：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
source /etc/profile.d/xbus.sh
```

## 连接相机机并接收

已有配对时用 `xbusctl link peers` 查看**接收机上**相机对端的导入前缀。首次配对时，相机机运行 `sudo xbusctl link pair --listen --restart`，接收机运行 `sudo xbusctl link pair <相机机IP> --import-prefix cam. --restart`（替换尖括号参数）。在相机机终端核对并输入请求码，两端服务将重启。然后在相机机启动 `camera_node 0` 或 `camera_node --test`。

```bash
xbusctl link status
xbusctl link capabilities --remote
./build/pico_node cam.camera.jpeg --once frame.jpg
file frame.jpg
./build/pico_node cam.camera.jpeg
```

目录中应看到来源名 `camera.jpeg`。若本机前缀是 `remote.`，订阅名应写 `remote.camera.jpeg`；多对端诊断命令需加 `--peer-id ID`。参数 `--once` 等待最多 30 秒，收到一帧后写入指定路径（覆盖同名文件），失败返回非零。持续接收用 Ctrl+C 停止。

出现 `Subscribed` 但帧数持续为零时，依次核对导入前缀、远端目录、发布程序是否持续采集，以及两端 schema 是否一致。可用 `xbusctl link topics --watch` 检查流量和丢弃。修改代码前阅读 [AGENTS.md](AGENTS.md)。

## WebRTC 网页播放（纯 C++）

单个 `pico_node` 进程直接订阅 XBus，用 cpp-httplib 提供网页与 HTTP SDP 信令，使用 GStreamer `webrtcbin` 实现 WebRTC。无需 Python、OpenCV 或外部媒体子进程。

媒体链路：JPEG → jpegdec → videoconvert / videoscale → VP8 → RTP → webrtcbin → 浏览器 video。网页位于 `web/index.html`，构建时复制到应用构建目录；运行时保留完整构建目录。

### 依赖与构建

默认直接使用 `third_party/archives/` 中的 GStreamer 1.24.12、libnice 0.1.22 源码，以及本地 Meson 工具。构建期间禁止联网下载，不再要求系统安装 GStreamer 开发包和插件包。httplib、JSON 也随项目提供。

目标机需提前具备 XBus 2.0 SDK、CMake 3.18+、C++17 编译器、Python 3.8+、Ninja、Flex、Bison 及基础开发库。在联网的 Ubuntu/Debian 准备机上可安装以下包；离线目标机需预装或使用匹配发行版与架构的完整离线包：

```bash
sudo apt install build-essential cmake python3 ninja-build pkg-config flex bison \
  libglib2.0-dev libssl-dev libjpeg-dev libvpx-dev libsrtp2-dev
```

在目标 Linux 机器本机编译：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
source /etc/profile.d/xbus.sh
sh build/run_pico_node.sh cam.camera.jpeg --web 8080
```

CMake 先解压、校验并构建本地 GStreamer，安装到 `build/third_party/prefix/`，再配置编译应用，最终程序位于 `build/pico_node`。第三方库默认使用 2 个编译任务，可通过 `-DPICO_DEPENDENCY_JOBS=4` 调整。Python 仅用于 Meson 构建，运行服务仍为 C++ 单进程。

浏览器打开 `http://接收机IP:8080`，点击“连接视频”。启动脚本为本次构建设置动态库和插件路径。换架构或复制到另一台机器时，应复制整个源码目录后重新构建，不要复制旧的 build 目录。

若希望使用系统现有 GStreamer 1.20+：

```bash
cmake -S . -B build-system -DPICO_BUILD_GSTREAMER=OFF
cmake --build build-system -j2
./build-system/pico_node cam.camera.jpeg --web 8080
```

也可指定 `-DPICO_GSTREAMER_PREFIX=/path/to/gstreamer`。统计、单帧保存和网页播放仍通过运行参数选择，所有构建均包含媒体依赖。第三方源码版本、基础依赖及离线边界见 [第三方依赖说明](third_party/README.md)。

### 运行行为

- 接收回调仅校验 JPEG 边界、统计并保留最新 SharedBuffer；主循环负责投递媒体数据。
- 最多 4 个观看会话，所有会话共享一套 JPEG 解码和 VP8 编码管线，各会话独立 RTP 打包和 WebRTC 发送，输出固定 1280×720 VP8，目标码率 2 Mbps。每约 33ms 投递最新图像；图源较慢时播放帧率随之降低，不补帧。
- 接收槽、appsrc 和媒体队列均限制积压，慢客户端会丢弃旧帧。`received` 为通过边界检查的 XBus 消息数，不代表已成功解码或浏览器呈现数。
- SDP 采用非 trickle ICE：双方收集完候选后交换 SDP。网页必须提供 VP8 video 接收能力。
- 网页主动断开会删除服务端会话；失败、断开、媒体管线报错和 35 秒内未连接的会话会被回收。媒体错误写到 stderr，该观看者需要重新连接。
- `/api/status` 返回消息计数、会话数、最近消息时间；`source_alive` 表示最近 5 秒内有通过边界检查的消息。图源停更后画面可能停在最后一帧。

默认绑定所有网卡，仅适合可信局域网，无账号认证。需开放 TCP 网页端口和 ICE 使用的动态 UDP 端口。不配置外部 STUN/TURN；跨 NAT/公网访问需要另外配置 webrtcbin 和浏览器的 ICE 服务，以及 HTTPS、鉴权和网络访问控制。

### 验收与排查

先运行原有 `--once frame.jpg`，确认通过两个直连 Hub 收到的 JPEG 可以打开。再使用远端 `camera_node --test`，从另一台局域网机器打开网页，确认序号持续变化；验证断开重连、多个浏览器、关闭图源和 Ctrl+C 退出，最后连接真实相机。

缺少插件时启动会报告具体名称，可用 `gst-inspect-1.0 webrtcbin`、`gst-inspect-1.0 vp8enc`、`gst-inspect-1.0 nicesrc` 排查。详细媒体日志可用：

```bash
GST_DEBUG=webrtcbin:5 sh build/run_pico_node.sh cam.camera.jpeg --web 8080
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
sh build/run_pico_node.sh cam.camera.jpeg --once frame.jpg
file frame.jpg
sh build/run_pico_node.sh cam.camera.jpeg --web 8080
```

网页视频左上角显示播放帧率，每秒按浏览器呈现帧数增量计算。优先使用视频帧回调，旧浏览器回退到播放质量计数（总帧数减丢帧数）；没有新帧时显示 0.0 FPS，断开和重连时清零。该数值是浏览器播放帧率，不是 XBus 接收消息速率。

### 多观看者的 CPU 与共享编码

只有一个共享 VP8 编码器，首个已协商会话收到图像时创建；无观看者时不投递 JPEG。第二个及后续观看者复用同一份编码数据，仅增加各自的 RTP 打包、SRTP 加密及网络发送开销。单路 720p 软件编码的 CPU 成本仍然存在。

新观看者等待关键帧后才开始发送，浏览器的关键帧请求会转交共享编码器。慢会话积压超过 2 MiB 时回收，需要重新连接，避免任意丢弃编码参考帧。共享管线报错会报告错误并停止服务。

验收时依次连接 1、2、4 个页面，观察每页画面及 FPS、`top -H` 中的 CPU，验证中途加入、断开重连、丢包恢复和慢客户端隔离。可查看状态接口：

```bash
curl http://127.0.0.1:8080/api/status
```

`encoder_instances` 在首次播放前为 0，启动后固定为 1；`encoded_frames` 是所有观看者共享的编码总帧数，不应乘以观看人数。各会话仍使用同一码率，不支持按观看者分别调整编码质量。
