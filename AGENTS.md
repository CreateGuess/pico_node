# XBus 2.0 节点开发约定

先读同目录 README.md 获取本项目的构建、设备依赖和验收命令。两个程序分别连接所在机器的 Hub；HubLink 配对和对端 TCP 端口由 Hub 管理。客户端从登录环境取得本机控制端点，系统服务部署通常使用 `ipc:///run/xbus/control.sock`。修改项目只需本目录和已安装的 SDK。

## 编译和接口

- 使用 C++17、`find_package(xbus-dev 2.0 CONFIG REQUIRED COMPONENTS client)` 和 `xbus::client` CMake target。引入安装的公开头，如 `xbus/api/node.hpp` 和 `xbus/simple/simple_publisher.hpp`。
- 设置 `NodeOptions.name` 并调用 `Node::connect(options)`。返回非空后仍需确认注册或订阅就绪。跨机发布通过 `TopicOptions.visibility = xbus::Visibility::HubLink` 开放；缺省仅本机。发送方保持来源名，接收方使用本端配置的对端前缀。
- 通过 `SimplePublisher::advertise(deadline)` 注册后再 `publish()`。示例用 `SharedBuffer::copy_of(jpeg)` 复制编码结果，因此下一帧可以复用 OpenCV 向量。检查返回值，发布被本地接纳后，远端仍可能因订阅尚未传播、队列满或断线错过该帧。
- 订阅用 `node->subscribe(topic, callback, SubscriptionOptions{schema})`。保留订阅句柄并检查 `wait_ready(timeout_ms)`。回调只做有界工作；保存缓冲使用 SharedBuffer 的所有权，避免仅保留 data() 指针。把磁盘写入或图像处理放到回调之外。退出时取消订阅并等待回调结束，然后关闭 Node。

## 这两个项目共享的消息契约

| 项目 | 约定 |
|---|---|
| 语义 | PubSub 实时图像流 |
| 来源名 | 当前发送端为 `camera.jpeg`；兼容旧话题 `camera.frames`，以实际发布配置为准 |
| Schema | 两端均用 `xbus::schema::hash("camera.jpeg@v1")` |
| 正文 | 当前发送端为 20 字节 CJPG 小端头 + 完整 JPEG；同时兼容无头的原始 JPEG |
| 上限 | 整条消息最多 8 MiB，包含 CJPG 头；超限消息拒收 |
| 接收 | 持续模式只计数；单帧模式最多保留一帧并写出原始 JPEG；网页模式保留最新帧并在回调外交给 GStreamer |

若要新增时间戳、序号或其他图像编码，先定义两端一致的字段、长度、字节序和 schema 版本，然后同步修改两端。当前测试画面上的序号是像素内容。不能将 C++ 对象、指针或 cv::Mat 的内存布局直接发给对端。

## 修改后的验证

先用测试图源，经两个直连 Hub 保存一张能被图像工具识别的 JPEG；再用真实设备验收。检查时区分发布接纳、目录同步、订阅就绪和真正收到图像。业务若需要可靠传输、重放、时延或丢帧率，需单独实现并验证相应契约。

## 网页模式

项目统一使用 C++17。HTTP 使用 cpp-httplib，WebRTC 使用 GStreamer webrtcbin，禁止引入 Python 运行网关。WebRTC 是固定编译功能，不设置可选功能宏；程序只读取可执行文件旁的 config/pico_node.json，直接执行 ./pico_node 启动；入口使用 int main()，不得解析或使用命令行参数覆盖配置。媒体处理、编码和 HTTP 操作不得放进 XBus 回调；保持缓冲有界。修改后按 README 在 Linux 接收机验收 WebRTC 播放、断开重连、会话回收和退出。

代码和构建脚本的注释统一使用中文，函数采用 Doxygen 格式说明用途、参数、返回值和必要的失败条件，函数内以编号行注释说明主要执行步骤；API、库名和协议名称保留原名。

## 目录与依赖

项目头文件放在 include/，实现放在 src/。第三方库集中在 third_party/，其原始文件和许可证保持上游内容；中文注释约定适用于项目自有代码。新增依赖通过目标级 CMake 配置接入，不使用全局头文件路径或库路径。

默认通过本地源码包离线构建 GStreamer 与 libnice；不得在 CMake 或 Meson 配置中启用联网下载。生成文件仅写入构建目录。基础开发库仍需目标机预装，详见 third_party/README.md。

CJPG 头定义见 include/cam_frame.hpp：魔数 4 字节、seq u32 LE、age_ns u64 LE、width u16 LE、height u16 LE。age_ns 是相机板内采集至发布的耗时，禁止解释为跨板绝对时间。保存和媒体解码仅使用第 20 字节之后的 JPEG 正文，持有原始 SharedBuffer 直到正文复制或写盘完成。

同一 topic 的多观看者共用一个 SharedEncoder，不同 topic 独立订阅、缓冲、统计和编码，禁止在 Peer 中重新创建 JPEG 解码器或 H.264 编码器。新增或恢复会话必须等待关键帧，编码帧不能任意丢弃；分发线程先于会话和事件循环停止。

硬件编码固定使用 Rockchip MPP 的 mpph264enc，不允许静默回退软件编码。新第三方源码必须固定提交与 SHA256，保留原始许可证；所有源码解压和安装仍在构建目录完成。程序直接启动，不依赖启动脚本；资源基于可执行文件目录定位，已有部署配置不得在重新构建时覆盖。

多相机配置使用 topics 数组，保留旧单 topic 配置兼容。网页按配置顺序显示话题名称，topic 必须通过文本节点显示；SDP 请求必须指定已配置 topic。单路媒体故障不应停止其他相机。
