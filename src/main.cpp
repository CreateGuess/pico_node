#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include "xbus/api/node.hpp"
#include "cam_frame.hpp"
#include "webrtc_server.hpp"

namespace
{
  // 信号处理函数只设置退出标志，资源清理由主循环完成。
  volatile std::sig_atomic_t stopping = 0;
  /** @brief 响应终止信号，通知主循环退出；不在信号上下文释放资源。 */
  void stop(int) { stopping = 1; }
  // 订阅回调与主循环共享的接收状态；图像由互斥锁保护，计数独立原子更新。
  struct Receiver
  {
    std::atomic<unsigned long long> frames{0}, invalid{0};
    std::atomic<long long> last_frame_ms{-1};
    std::mutex mutex;
    xbus::SharedBuffer snapshot;
    xbus::SharedBuffer invalid_sample;
    bool invalid_sample_pending = false;

  };

  /**
   * @brief 描述首条无效消息，帮助核对发布端是否发送完整的原始 JPEG。
   * @param bytes 首条未通过消息校验的数据，允许为空。
   * @return 消息长度、失败原因及首尾最多 16 字节的十六进制表示。
   * @details 仅由主循环调用，不在订阅回调内执行日志格式化或输出。
   */
  std::string describeInvalidJpeg(const xbus::SharedBuffer& bytes)
  {
    // 1. 使用与接收回调相同的解包规则，报告封装头或 JPEG 正文错误。
    const auto n = bytes.size();
    camframe::JpegView view;
    const auto* reason = camframe::parse_jpeg(bytes.data(), n, view);
    std::ostringstream out;
    out << "JPEG 校验失败：size=" << n << " reason=" << (reason ? reason : "通过校验");
    // 2. 只打印有限的首尾数据，避免大图像造成日志刷屏。
    const auto count = n < 16 ? n : 16;
    out << std::hex << std::setfill('0') << " head=[";
    for (std::size_t i = 0; i < count; ++i)
      out << (i ? " " : "") << std::setw(2) << static_cast<unsigned int>(bytes.data()[i]);
    out << "] tail=[";
    for (std::size_t i = n - count; i < n; ++i)
      out << (i == n - count ? "" : " ") << std::setw(2) << static_cast<unsigned int>(bytes.data()[i]);
    out << "]";
    return out.str();
  }
}

/**
 * @brief 连接 XBus 并按启动参数执行统计、单帧保存或网页播放。
 * @param argc 参数数量。
 * @param argv 话题名称，以及可选的 --once 文件路径或 --web 端口。
 * @return 正常结束返回 0，连接、订阅、服务或文件操作失败返回 1，参数错误返回 2。
 */
int main(int argc, char **argv)
{
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  // 1. 解析运行模式与端口，确认参数完整且端口有效。
  const bool web = (argc == 3 || argc == 4) && std::string(argv[2]) == "--web";
  int port = 8080;
  if (web && argc == 4)
  {
    try
    {
      std::size_t used = 0;
      port = std::stoi(argv[3], &used);
      if (used != std::string(argv[3]).size() || port < 1 || port > 65535)
        throw std::invalid_argument("port");
    }
    catch (...)
    {
      std::cerr << "Invalid HTTP port\n";
      return 2;
    }
  }
  const bool save = argc == 4 && std::string(argv[2]) == "--once";
  if ((argc != 2 && !save && !web) || !*argv[1])
  {
    std::cerr << "Usage: pico_node TOPIC [--once OUTPUT.jpg | --web [PORT]]\n";
    return 2;
  }
  // 2. 安装退出信号处理函数，使用固定节点名连接本机 Hub。
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  xbus::NodeOptions options;
  options.name = "pico_node";
  auto node = xbus::Node::connect(options);
  if (!node)
    return 1;
  auto state = std::make_shared<Receiver>();
  // 3. 订阅 JPEG 话题；回调只校验、保存缓冲引用及更新统计，不解码或写盘。
  auto sub = node->subscribe(argv[1], [state, save, web](xbus::Event event)
                             {
    const auto& bytes = event.payload;
    // 3.1 识别 CJPG 封装，按小端解析 20 字节头，再校验 JPEG 正文。
    camframe::JpegView view;
    if (camframe::parse_jpeg(bytes.data(), bytes.size(), view)) {
      // 仅保留首条失败样本，日志输出交给主循环，避免阻塞接收回调。
      if (state->invalid.fetch_add(1) == 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->invalid_sample = bytes;
        state->invalid_sample_pending = true;
      }
      return;
    }
    // 3.2 保存模式只取首帧，网页模式覆盖旧帧；统计模式不持有图像。
    // 通过 SharedBuffer 保持数据所有权，确保回调返回后数据仍然有效。
    if (save || web) {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (web || state->snapshot.empty()) state->snapshot = bytes;
    }
    // 3.3 记录单调时钟时间及有效消息数，供主循环和网页状态接口读取。
    state->last_frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
    ++state->frames; }, xbus::SubscriptionOptions{xbus::schema::hash("camera.jpeg@v1")});
  // 本地订阅就绪不代表已经收到远端图像。
  if (!sub || !sub->wait_ready(5000))
  {
    std::cerr << "Subscription not ready on local Hub\n";
    return 1;
  }
  // 4. 网页模式启动 HTTP 和媒体服务，状态回调按需计算最近帧距今秒数。
  std::unique_ptr<WebRtcServer> server;
  if (web)
  {
    try
    {
      server = std::make_unique<WebRtcServer>(port, PICO_WEB_PAGE, [state]
                                              {
        WebStatus status;
        status.received = state->frames.load(); status.invalid = state->invalid.load();
        const auto last = state->last_frame_ms.load();
        if (last >= 0) status.frame_age_seconds = (std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count() - last) / 1000.0;
        return status; });
    }
    catch (const std::exception &error)
    {
      std::cerr << "WebRTC startup failed: " << error.what() << '\n';
      sub->cancel_and_wait(3000);
      node->shutdown();
      return 1;
    }
    std::cerr << "Web player: http://<receiver-ip>:" << port << "\n";
  }
  std::cerr << "Subscribed to " << argv[1] << std::endl;
  // 5. 驱动接收循环：每秒报告统计，网页模式约每 33 毫秒取走最新帧。
  // 单帧模式收到首帧或等待 30 秒后结束，其余模式等待退出信号。
  const auto deadline = Clock::now() + 30s;
  auto report = Clock::now() + 1s;
  bool failed = false;
  while (!stopping && (!save || (!state->frames.load() && Clock::now() < deadline)))
  {
    // 首条无效样本只输出一次；即使数据为空，也通过独立标志触发诊断。
    xbus::SharedBuffer invalid_sample;
    bool report_invalid = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      report_invalid = state->invalid_sample_pending;
      if (report_invalid) {
        invalid_sample = std::move(state->invalid_sample);
        state->invalid_sample = xbus::SharedBuffer{};
        state->invalid_sample_pending = false;
      }
    }
    if (report_invalid) std::cerr << describeInvalidJpeg(invalid_sample) << '\n';
    if (Clock::now() >= report)
    {
      std::cerr << "received=" << state->frames << " invalid=" << state->invalid << std::endl;
      report = Clock::now() + 1s;
    }
    if (server)
    {
      xbus::SharedBuffer image;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        image = std::move(state->snapshot);
        state->snapshot = xbus::SharedBuffer{};
      }
      // 媒体处理在回调之外执行，期间不持有帧缓冲互斥锁。
      // 即使没有新图像，也需要回收已经断开的会话。
      server->push(image);
      if (!server->healthy())
      {
        std::cerr << "HTTP server stopped\n";
        failed = true;
        break;
      }
    }
    std::this_thread::sleep_for(web ? 33ms : 50ms);
  }
  // 6. 取消订阅并等待回调结束，再关闭节点，避免清理过程与回调并发。
  const bool drained = sub->cancel_and_wait(3000);
  node->shutdown();
  if (!drained || failed)
    return 1;
  // 7. 单帧模式将保留的原始 JPEG 写入文件，超时未收到图像则报告失败。
  if (save)
  {
    xbus::SharedBuffer image;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      image = std::move(state->snapshot);
    }
    if (image.empty())
    {
      std::cerr << "No JPEG received\n";
      return 1;
    }
    std::ofstream out(argv[3], std::ios::binary);
    camframe::JpegView view;
    camframe::parse_jpeg(image.data(), image.size(), view);
    // 原始消息仍由 image 持有，写盘只使用解包后的 JPEG 正文。
    out.write(reinterpret_cast<const char *>(view.data), view.size);
    out.close();
    if (!out)
    {
      std::cerr << "Cannot write " << argv[3] << '\n';
      return 1;
    }
    std::cout << "Saved " << view.size << " bytes to " << argv[3] << '\n';
  }
  return 0;
}
