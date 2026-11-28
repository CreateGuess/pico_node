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
#include <vector>
#include <algorithm>
#include <functional>
#include "xbus/api/node.hpp"
#include "cam_frame.hpp"
#include "app_config.hpp"
#include "webrtc_server.hpp"

namespace
{
  // 信号处理函数只设置退出标志，资源清理由主循环完成。
  volatile std::sig_atomic_t stopping = 0;
  /** @brief 响应终止信号，通知主循环退出；不在信号上下文释放资源。 */
  void stop(int) { stopping = 1; }
  // 保存某一路收到的最新图像和接收统计，每个Topic一个
  struct Receiver
  {
    std::atomic<unsigned long long> frames{0}, invalid{0};
    std::atomic<long long> last_frame_ms{-1};
    std::mutex mutex;
    // 保存最新的有效 JPEG，或首条无效消息的原始字节；允许为空。
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
  std::string describeInvalidJpeg(const xbus::SharedBuffer &bytes)
  {
    // 1. 使用与接收回调相同的解包规则，报告封装头或 JPEG 正文错误。
    const auto n = bytes.size();
    camframe::JpegView view;
    const auto *reason = camframe::parse_jpeg(bytes.data(), n, view);
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
 * @brief 从配置文件读取运行参数，连接 XBus 并启动接收、保存或网页播放。
 * @return 正常结束返回 0，连接、订阅、服务或文件操作失败返回 1，配置读取或校验失败返回 2。
 */
int main()
{
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  // 1. 从程序旁的配置文件读取全部运行设置，并准备媒体库环境。
  AppConfig config;
  try
  {
    // 获取build目录的绝对路径
    const auto directory = executableDirectory();
    // 配置文件路径为 build/config/pico_node.json，加载配置文件
    config = loadConfig(directory / "config/pico_node.json");
    // 由于GStreamer大部分插件依赖于动态库，而这些动态库是在构建时由CMake安装到第三方目录下的，
    // 因此需要在运行时设置环境变量GST_PLUGIN_PATH_1_0和GST_PLUGIN_SYSTEM_PATH_1_0来指定插件路径，
    // 同时设置GST_PLUGIN_SCANNER_1_0来指定扫描器路径，确保GStreamer能够正确加载插件和扫描器。
    prepareMediaEnvironment(directory);
  }
  catch (const std::exception &error)
  {
    std::cerr << "启动配置错误：" << error.what() << '\n';
    return 2;
  }
  const bool web = config.mode == "web";
  const bool save = config.mode == "once";
  const int port = config.port;
  // 2. 安装退出信号处理函数，使用固定节点名连接本机 Hub。
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  xbus::NodeOptions options;
  options.name = "pico_node";
  auto node = xbus::Node::connect(options);
  if (!node)
    return 1;
  // 3. 按配置顺序为各话题创建独立接收状态，保留全部订阅句柄。
  // subscribe(topic名称，回调函数，接收数据协议)，返回值为订阅句柄
  using SubscriptionHandle = decltype(node->subscribe(config.topics.front(), std::function<void(xbus::Event)>{},
                                                      xbus::SubscriptionOptions{xbus::schema::hash("camera.jpeg@v1")}));
  std::vector<SubscriptionHandle> subscriptions;
  std::vector<std::shared_ptr<Receiver>> states;
  bool failed = false;
  for (const auto &topic : config.topics)
  {
    auto state = std::make_shared<Receiver>();
    auto sub = node->subscribe(topic, [state, save, web](xbus::Event event)
                               {
      const auto& bytes = event.payload;
      // 3.1 识别 CJPG 封装，按小端解析 20 字节头，再校验 JPEG 正文。
      camframe::JpegView view;
      // 解析 JPEG 数据
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
    // subscribe end

    // 等待订阅就绪，最多阻塞 5 秒；若失败则不启动网页服务。
    const bool ready = sub && sub->wait_ready(5000);
    if (sub)
      subscriptions.push_back(std::move(sub));
    states.push_back(state);
    if (!ready)
    {
      std::cerr << "本机订阅未就绪：" << topic << '\n';
      failed = true;
      break;
    }
    std::cerr << "已订阅：" << topic << '\n';
  }
  // 4. 所有话题共用一个 HTTP 服务，状态按配置顺序逐路返回。
  std::unique_ptr<WebRtcServer> server;
  if (web && !failed)
  {
    try
    {
      server = std::make_unique<WebRtcServer>(port, config.page, config.bitrate, config.gop, config.codec, config.topics, [states, topics = config.topics]
                                              {
        std::vector<WebStatus> result;
        for (std::size_t i = 0; i < states.size(); ++i) {
          WebStatus status;
          status.topic = topics[i];
          status.received = states[i]->frames.load();
          status.invalid = states[i]->invalid.load();
          const auto last = states[i]->last_frame_ms.load();
          // 计算最近一帧的时间差，单位为秒；若尚未收到有效帧则为负值。
          if (last >= 0) {status.frame_age_seconds = (std::chrono::duration_cast<std::chrono::milliseconds>(
              Clock::now().time_since_epoch()).count() - last) / 1000.0;}
          result.push_back(std::move(status));
        }
        return result; });
      // end

      std::cerr << "网页地址：http://<接收机IP>:" << port << '\n';
    }
    catch (const std::exception &error)
    {
      std::cerr << "网页服务启动失败：" << error.what() << '\n';
      failed = true;
    }
  }
  // 5. 驱动接收循环：每秒报告统计，网页模式约每 33 毫秒取走最新帧。
  // 单帧模式等待每路首帧，共享 30 秒期限；其余模式等待退出信号。
  const auto deadline = Clock::now() + 30s;
  auto last_report = Clock::now();
  std::vector<unsigned long long> previous_frames;
  previous_frames.reserve(states.size());
  for (const auto &state : states)
    previous_frames.push_back(state->frames.load());
  std::vector<unsigned long long> previous_encoded(states.size(), 0);
  auto report = last_report + 1s;
  while (!failed && !stopping && (!save || (Clock::now() < deadline && std::any_of(states.begin(), states.end(), [](const auto &state)
                                                                                   { return state->frames.load() == 0; }))))
  {
    const auto now = Clock::now();
    const bool report_now = now >= report;
    const double elapsed = std::chrono::duration<double>(now - last_report).count();
    for (std::size_t i = 0; i < states.size(); ++i)
    {
      const auto &state = states[i];
      const auto &topic = config.topics[i];
      // 首条无效样本只输出一次；即使数据为空，也通过独立标志触发诊断。
      xbus::SharedBuffer invalid_sample;
      bool report_invalid = false;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        report_invalid = state->invalid_sample_pending;
        if (report_invalid)
        {
          invalid_sample = std::move(state->invalid_sample);
          state->invalid_sample = xbus::SharedBuffer{};
          state->invalid_sample_pending = false;
        }
      }
      if (report_invalid)
        std::cerr << "topic=" << topic << " " << describeInvalidJpeg(invalid_sample) << '\n';
      if (report_now)
      {
        // 按实际经过时间计算各路有效接收帧率，避免循环调度延迟造成偏差。
        const auto frames = state->frames.load();
        const double fps = (frames - previous_frames[i]) / elapsed;
        previous_frames[i] = frames;
        const auto encoded = server ? server->encodedFrames(topic) : 0;
        const double encoded_fps = (encoded - previous_encoded[i]) / elapsed;
        previous_encoded[i] = encoded;
        std::cerr << "topic=" << topic << " received=" << frames << " invalid=" << state->invalid
                  << " fps=" << fps << " encoded_frames=" << encoded << " encoded_fps=" << encoded_fps << std::endl;
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
        server->push(topic, image);
        if (!server->healthy())
        {
          std::cerr << "HTTP server stopped\n";
          failed = true;
          break;
        }
      }
    }
    if (report_now)
    {
      last_report = now;
      report = now + 1s;
    }
    std::this_thread::sleep_for(web ? 33ms : 50ms);
  }
  // 6. 取消订阅并等待回调结束，再关闭节点，避免清理过程与回调并发。
  bool drained = true;
  for (auto &sub : subscriptions)
  {
    if (!sub->cancel_and_wait(3000))
      drained = false;
  }
  node->shutdown();
  if (!drained || failed)
    return 1;
  // 7. 单帧模式将保留的原始 JPEG 写入文件，超时未收到图像则报告失败。
  if (save)
  {
    for (std::size_t i = 0; i < states.size(); ++i)
    {
      const auto &state = states[i];
      xbus::SharedBuffer image;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        image = std::move(state->snapshot);
      }
      if (image.empty())
      {
        std::cerr << "未收到 JPEG：" << config.topics[i] << '\n';
        failed = true;
        continue;
      }
      // 多话题保存时以配置序号区分文件，避免话题包含路径字符或不同话题互相覆盖。
      std::filesystem::path output(config.output);
      if (states.size() > 1)
        output = output.parent_path() / (output.stem().string() + "." + std::to_string(i + 1) + output.extension().string());
      std::ofstream out(output, std::ios::binary);
      camframe::JpegView view;
      camframe::parse_jpeg(image.data(), image.size(), view);
      // 原始消息仍由 image 持有，写盘只使用解包后的 JPEG 正文。
      out.write(reinterpret_cast<const char *>(view.data), view.size);
      out.close();
      if (!out)
      {
        std::cerr << "Cannot write " << output << '\n';
        failed = true;
        continue;
      }
      std::cout << "Saved " << view.size << " bytes to " << output << '\n';
    }
  }
  return failed ? 1 : 0;
}
