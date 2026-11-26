#include "webrtc_server.hpp"
#include "cam_frame.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video-event.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
  using Clock = std::chrono::steady_clock;
  using Json = nlohmann::json;
  using Structure = std::unique_ptr<GstStructure, decltype(&gst_structure_free)>;
  using Description = std::unique_ptr<GstWebRTCSessionDescription, decltype(&gst_webrtc_session_description_free)>;

  /**
   * @brief 在 GStreamer webrtcbin 上执行异步操作并等待结果。
   * @param rtc webrtcbin 元素。
   * @param name set-remote-description、create-answer 或 set-local-description。
   * @param desc 设置描述时传入 SDP；创建应答时传入空指针。
   * @return 独立持有的响应结构；无响应字段的设置操作返回空指针。
   * @throws std::runtime_error 操作超过 5 秒或响应包含错误。
   */
  Structure action(GstElement *rtc, const char *name, GstWebRTCSessionDescription *desc = nullptr)
  {
    // 1. 将 GStreamer 完成回调桥接到当前 HTTP 工作线程的等待结果。
    auto result = std::make_shared<std::promise<Structure>>();
    auto future = result->get_future();
    auto *holder = new decltype(result)(result);
    // 创建GStreamer的完成通知对象
    auto *promise = gst_promise_new_with_change_func([](GstPromise *p, gpointer data)
                                                     {
    auto output = *static_cast<decltype(result)*>(data);
    const GstStructure* reply = nullptr;
    if (gst_promise_wait(p) == GST_PROMISE_RESULT_REPLIED) reply = gst_promise_get_reply(p);
    // 释放 GStreamer 异步结果对象前，复制其持有的响应数据。
    output->set_value(Structure(reply ? gst_structure_copy(reply) : nullptr, gst_structure_free)); }, holder, [](gpointer data)
                                                     { delete static_cast<decltype(result) *>(data); });
    // 2. 发起指定操作；创建应答的第一个参数为可选配置，其余操作传入 SDP。
    if (desc)
      g_signal_emit_by_name(rtc, name, desc, promise);
    else
      g_signal_emit_by_name(rtc, name, nullptr, promise);
    // 3. 最多等待 5 秒，超时中断等待；成功后检查响应中的错误字段。
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
      // 共享回调状态，确保超时与异步完成同时发生时数据仍然有效。
      gst_promise_interrupt(promise);
      gst_promise_unref(promise);
      throw std::runtime_error("WebRTC operation timed out");
    }
    auto reply = future.get();
    gst_promise_unref(promise);
    if (reply && gst_structure_has_field(reply.get(), "error"))
      throw std::runtime_error("WebRTC rejected SDP operation");
    return reply;
  }

  // 每个观看者仅持有 RTP 打包和 WebRTC 连接，共用服务端的 VP8 编码结果。
  struct Peer
  {
    GstElement *pipeline = nullptr;
    GstElement *source = nullptr;
    GstElement *rtc = nullptr;
    std::string id;
    Clock::time_point born = Clock::now();
    std::atomic<bool> negotiated{false}, congested{false};
    bool waiting_keyframe = true; // 仅由编码分发线程读写。
    std::shared_ptr<std::atomic<bool>> request_keyframe;

    /**
     * @brief 为一个观看者创建 VP8 到 WebRTC 的发送管线，不创建解码器和编码器。
     * @throws std::runtime_error 管线解析或启动失败。
     */
    explicit Peer(std::shared_ptr<std::atomic<bool>> request) : request_keyframe(std::move(request))
    {
      // 1. 生成会话标识，供网页断开时删除对应会话。
      gchar *uuid = g_uuid_string_random();
      id = uuid;
      g_free(uuid);
      GError *error = nullptr;
      // 2. 输入共享编码器的 VP8 结果，各会话独立打包、加密和发送。
      pipeline = gst_parse_launch(
          "appsrc name=jpeg is-live=true format=time do-timestamp=true block=false max-bytes=2097152 "
          "caps=video/x-vp8,width=1280,height=720,framerate=30/1 "
          "! rtpvp8pay pt=96 ! application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000 "
          "! webrtcbin name=rtc bundle-policy=max-bundle",
          &error);
      // 3. 检查解析结果，如果失败则释放资源并抛出异常。
      if (error || !pipeline)
      {
        std::string message = error ? error->message : "Cannot create media pipeline";
        if (error)
          g_error_free(error);
        if (pipeline)
          gst_object_unref(pipeline);
        pipeline = nullptr;
        throw std::runtime_error(message);
      }
      // 4. 保留输入组件及连接组件的引用，启动管线等待图像与浏览器协商。
      source = gst_bin_get_by_name(GST_BIN(pipeline), "jpeg");
      rtc = gst_bin_get_by_name(GST_BIN(pipeline), "rtc");
      // 将浏览器丢包后请求的关键帧转交共享编码器，回调状态独立持有。
      auto* pad = gst_element_get_static_pad(source, "src");
      auto* flag = new std::shared_ptr<std::atomic<bool>>(request_keyframe);
      gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
        [](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
          if (gst_video_event_is_force_key_unit(GST_PAD_PROBE_INFO_EVENT(info)))
            **static_cast<std::shared_ptr<std::atomic<bool>>*>(data) = true;
          return GST_PAD_PROBE_OK;
        }, flag, [](gpointer data) { delete static_cast<std::shared_ptr<std::atomic<bool>>*>(data); });
      gst_object_unref(pad);
      if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      {
        close();
        throw std::runtime_error("Cannot start media pipeline");
      }
    }
    /**
     * @brief 停止媒体管线并释放组件引用，供启动失败和析构时共用。
     */
    void close()
    {
      // 先停止流处理任务，再释放各媒体组件的引用。
      if (pipeline)
        gst_element_set_state(pipeline, GST_STATE_NULL);
      if (source)
        gst_object_unref(source);
      if (rtc)
        gst_object_unref(rtc);
      if (pipeline)
        gst_object_unref(pipeline);
      pipeline = source = rtc = nullptr;
    }
    /** @brief 释放该观看者的编码、传输及连接资源。 */
    ~Peer() { close(); }

    /**
     * @brief 接收浏览器的 SDP 提议，生成含完整 ICE 候选地址的 SDP 应答。
     * @param offer 浏览器创建的视频接收提议，需包含 VP8 能力。
     * @return 可直接交给浏览器设置远端描述的 SDP 文本。
     * @throws std::runtime_error SDP 无效、异步操作失败或 ICE 收集超过 10 秒。
     */
    std::string answer(const std::string &offer)
    {
      // 1. 解析浏览器 SDP，并设置为当前连接的远端描述。
      GstSDPMessage *sdp = nullptr;
      gst_sdp_message_new(&sdp);
      if (gst_sdp_message_parse_buffer(reinterpret_cast<const guint8 *>(offer.data()), offer.size(), sdp) != GST_SDP_OK)
      {
        gst_sdp_message_free(sdp);
        throw std::runtime_error("Invalid SDP");
      }
      Description remote(gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp),
                         gst_webrtc_session_description_free);
      action(rtc, "set-remote-description", remote.get());

      // 2. 根据双方媒体能力生成应答，并设置本地描述以启动 ICE 候选收集。
      auto reply = action(rtc, "create-answer");
      GstWebRTCSessionDescription *local = nullptr;
      if (!reply || !gst_structure_get(reply.get(), "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &local, nullptr) || !local)
        throw std::runtime_error("Cannot create SDP answer (browser must offer VP8 video)");
      Description local_description(local, gst_webrtc_session_description_free);
      action(rtc, "set-local-description", local_description.get());

      // 3. 采用非增量 ICE 协商，每 20 毫秒查询一次，最多等待 10 秒。
      const auto deadline = Clock::now() + std::chrono::seconds(10);
      while (Clock::now() < deadline)
      {
        GstWebRTCICEGatheringState state;
        g_object_get(rtc, "ice-gathering-state", &state, nullptr);
        if (state == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE)
        {
          // 4. 重新读取包含候选地址的本地描述，序列化后允许主循环投递图像。
          g_object_get(rtc, "local-description", &local, nullptr);
          if (!local)
            throw std::runtime_error("Missing local SDP");
          gchar *text = gst_sdp_message_as_text(local->sdp);
          std::string answer = text;
          g_free(text);
          gst_webrtc_session_description_free(local);
          negotiated = true;
          return answer;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      throw std::runtime_error("ICE gathering timed out");
    }

    /**
     * @brief 判断会话是否需要从服务端集合中移除，并输出媒体错误。
     * @return 连接断开、失败、关闭、35 秒未连通或管线报错时返回真。
     */
    bool expired() const
    {
      // 1. 检查连接状态、积压错误及建立连接的时间限制。
      if (congested) return true;
      GstWebRTCPeerConnectionState state;
      g_object_get(rtc, "connection-state", &state, nullptr);
      if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED || state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED ||
          state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED)
        return true;
      if (state != GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED && Clock::now() - born > std::chrono::seconds(35))
        return true;
      // 2. 检查媒体总线；解码或编码错误可能早于连接状态变化。
      auto *bus = gst_element_get_bus(pipeline);
      auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
      gst_object_unref(bus);
      if (!message)
        return false;
      GError *error = nullptr;
      gchar *debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      std::cerr << "Media session failed: " << error->message << '\n';
      g_error_free(error);
      g_free(debug);
      gst_message_unref(message);
      return true;
    }

    /**
     * @brief 向当前观看者发送共享的 VP8 帧，只复制缓冲元数据，不复制编码正文。
     * @param encoded 共享编码器输出，调用期间由分发线程持有。
     * @details 新会话等待关键帧；慢会话超过 2 MiB 积压后回收，避免丢参考帧导致花屏。
     */
    void push(GstBuffer* encoded)
    {
      if (!negotiated || congested) return;
      // 1. 新会话从完整关键帧开始，不能从其他帧的预测数据开始解码。
      if (waiting_keyframe) {
        if (GST_BUFFER_FLAG_IS_SET(encoded, GST_BUFFER_FLAG_DELTA_UNIT)) {
          *request_keyframe = true;
          return;
        }
        waiting_keyframe = false;
      }
      // 2. 单个慢客户端不阻塞其他观看者；不任意丢弃已经编码的参考帧。
      guint64 queued = 0;
      g_object_get(source, "current-level-bytes", &queued, nullptr);
      if (queued > 2 * 1024 * 1024) {
        std::cerr << "Slow viewer disconnected: " << id << '\n';
        congested = true;
        return;
      }
      // 3. 正文内存通过引用共享，每个会话重新按自己的运行时钟打时间戳。
      auto* buffer = gst_buffer_copy(encoded);
      GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
      GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
      if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK)
        congested = true;
    }
  };

  /** @brief 全部观看者共享的 JPEG 解码和 VP8 编码管线，单独线程拉取编码帧。 */
  struct SharedEncoder
  {
    GstElement* pipeline = nullptr;
    GstElement* source = nullptr;
    GstElement* sink = nullptr;
    GstElement* codec = nullptr;
    std::atomic<bool> running{true};
    std::thread worker;

    /** @brief 创建唯一编码器，异步输出交给 distribute；错误通过 failed 回报。 */
    SharedEncoder(std::shared_ptr<std::atomic<bool>> request,
                  std::function<void(GstBuffer*)> distribute, std::atomic<bool>& failed)
    {
      GError* error = nullptr;
      pipeline = gst_parse_launch(
        "appsrc name=input is-live=true format=time do-timestamp=true block=false max-bytes=8388608 "
        "caps=image/jpeg,framerate=30/1 ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream "
        "! jpegdec ! videoconvert ! videoscale ! video/x-raw,format=I420,width=1280,height=720 "
        "! vp8enc name=encoder deadline=1 cpu-used=8 threads=2 target-bitrate=2000000 keyframe-max-dist=30 "
        "! appsink name=output sync=false max-buffers=8 drop=false", &error);
      if (error || !pipeline) {
        std::string message = error ? error->message : "Cannot create shared encoder";
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        throw std::runtime_error(message);
      }
      source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
      sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
      codec = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
      if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        release();
        throw std::runtime_error("Cannot start shared encoder");
      }
      // 1. 编码帧不能随意丢弃；分发线程持续取出，背压时在 JPEG 输入端丢旧帧。
      worker = std::thread([this, request, distribute, &failed] {
        auto* bus = gst_element_get_bus(pipeline);
        while (running) {
          if (request->exchange(false)) {
            auto* pad = gst_element_get_static_pad(codec, "src");
            gst_pad_send_event(pad, gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0));
            gst_object_unref(pad);
          }
          auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
          if (sample) {
            distribute(gst_sample_get_buffer(sample));
            gst_sample_unref(sample);
          }
          auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
          if (message) {
            GError* err = nullptr; gchar* debug = nullptr;
            gst_message_parse_error(message, &err, &debug);
            std::cerr << "Shared encoder failed: " << err->message << '\n';
            g_error_free(err); g_free(debug); gst_message_unref(message);
            failed = true;
            break;
          }
        }
        gst_object_unref(bus);
      });
    }

    /** @brief 停止媒体处理后释放组件引用；启动失败时也调用。 */
    void release()
    {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(source); gst_object_unref(sink); gst_object_unref(codec);
      gst_object_unref(pipeline);
    }
    /** @brief 先等待分发线程结束，再释放媒体管线，避免线程访问已释放的会话。 */
    ~SharedEncoder() { running = false; worker.join(); release(); }

    /** @brief 仅复制一次 JPEG 到共享编码器，输入积压时跳过当前帧。 */
    void push(const camframe::JpegView& jpeg)
    {
      guint64 queued = 0;
      g_object_get(source, "current-level-bytes", &queued, nullptr);
      if (queued) return;
      auto* buffer = gst_buffer_new_allocate(nullptr, jpeg.size, nullptr);
      gst_buffer_fill(buffer, 0, jpeg.data, jpeg.size);
      gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
    }

  };
}

struct WebRtcServer::Impl
{
  httplib::Server http;
  std::thread http_thread, loop_thread;
  GMainLoop *loop = nullptr;
  std::mutex mutex;
  std::vector<std::shared_ptr<Peer>> peers;
  std::atomic<bool> listening{true}, encoder_failed{false};
  std::shared_ptr<std::atomic<bool>> request_keyframe = std::make_shared<std::atomic<bool>>(true);
  std::unique_ptr<SharedEncoder> encoder;
  std::atomic<unsigned> encoder_instances{0};
  std::atomic<unsigned long long> encoded_frames{0};
  std::string page;
  std::function<WebStatus()> status;

  /**
   * @brief 初始化媒体环境、注册网页接口，并启动事件线程与 HTTP 线程。
   * @param port HTTP 监听端口。
   * @param path 网页文件路径。
   * @param get_status 获取接收计数及最近帧时间的回调。
   * @throws std::runtime_error 插件缺失、网页读取失败或端口绑定失败。
   */
  Impl(int port, const std::string &path, std::function<WebStatus()> get_status) : status(std::move(get_status))
  {
    // 1. 初始化 GStreamer，并检查媒体链路需要的运行时插件。
    gst_init(nullptr, nullptr);
    for (const char *name : {"appsrc", "appsink", "jpegdec", "videoconvert", "videoscale", "vp8enc", "rtpvp8pay", "webrtcbin", "nicesrc"})
    {
      auto *factory = gst_element_factory_find(name);
      if (!factory)
        throw std::runtime_error(std::string("Missing GStreamer plugin: ") + name);
      gst_object_unref(factory);
    }
    // 2. 启动时读取网页，配置请求大小及网络读写超时。
    std::ifstream file(path, std::ios::binary);
    if (!file)
      throw std::runtime_error("Cannot open webpage: " + path);
    page.assign(std::istreambuf_iterator<char>(file), {});
    http.set_payload_max_length(64 * 1024);
    http.set_read_timeout(10, 0);
    http.set_write_timeout(10, 0);
    // 3. 注册网页和状态接口；状态计数来自 XBus，不代表浏览器已呈现帧数。
    http.Get("/", [this](const httplib::Request &, httplib::Response &res)
             { res.set_content(page, "text/html; charset=utf-8"); });
    http.Get("/api/status", [this](const httplib::Request &, httplib::Response &res)
             {
      auto s = status();
      std::lock_guard<std::mutex> lock(mutex);
      Json j{{"received", s.received}, {"invalid", s.invalid}, {"peers", peers.size()}, {"source_alive", s.frame_age_seconds >= 0 && s.frame_age_seconds < 5}};
      j["encoder_instances"] = encoder_instances.load();
      j["encoded_frames"] = encoded_frames.load();
      j["frame_age_seconds"] = s.frame_age_seconds < 0 ? Json(nullptr) : Json(s.frame_age_seconds);
      res.set_content(j.dump(), "application/json"); });
    // 4. 注册协商接口：校验提议 → 分配会话 → 生成应答 → 返回 SDP 和会话标识。
    http.Post("/offer", [this](const httplib::Request &req, httplib::Response &res)
              {
      std::shared_ptr<Peer> peer;
      try {
        Json json = Json::parse(req.body);
        if (json.at("type") != "offer" || !json.at("sdp").is_string()) throw std::runtime_error("Expected SDP offer");
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (peers.size() >= 4) { res.status = 503; res.set_content("Viewer limit reached", "text/plain"); return; }
          peer = std::make_shared<Peer>(request_keyframe);
          peers.push_back(peer);
        }
        // ICE 候选收集可能阻塞，协商期间不持有会话集合的互斥锁。
        auto sdp = peer->answer(json.at("sdp").get<std::string>());
        res.set_content(Json{{"type", "answer"}, {"sdp", sdp}, {"id", peer->id}}.dump(), "application/json");
      } catch (const std::exception& error) {
        if (peer) remove(peer->id);
        std::cerr << "Offer failed: " << error.what() << '\n';
        res.status = 400; res.set_content("Cannot negotiate video; check server logs and VP8 support", "text/plain");
      } });
    // 5. 注册主动断开接口；移除集合引用后，由最后一个持有者释放媒体资源。
    http.Delete(R"(/session/([a-f0-9-]+))", [this](const httplib::Request &req, httplib::Response &res)
                { remove(req.matches[1].str()); res.status = 204; });
    // 6. 绑定端口，先启动媒体事件循环，再启动 HTTP 请求处理。
    if (!http.bind_to_port("0.0.0.0", port))
      throw std::runtime_error("Cannot bind HTTP port");
    loop = g_main_loop_new(nullptr, FALSE);
    std::promise<void> running;
    auto started = running.get_future();
    g_idle_add([](gpointer data) -> gboolean
               {
      static_cast<std::promise<void>*>(data)->set_value();
      return G_SOURCE_REMOVE; }, &running);
    loop_thread = std::thread([this]
                              { g_main_loop_run(loop); });
    // 确认事件循环已启动，避免退出请求早于循环启动而导致等待不结束。
    started.wait();
    http_thread = std::thread([this]
                              { http.listen_after_bind(); listening = false; });
  }
  /**
   * @brief 按会话标识移除观看者；不存在的标识不产生操作。
   * @param id 协商接口返回的会话标识。
   */
  void remove(const std::string &id)
  {
    std::lock_guard<std::mutex> lock(mutex);
    peers.erase(std::remove_if(peers.begin(), peers.end(), [&](const auto &p)
                               { return p->id == id; }),
                peers.end());
  }
  /** @brief 按 HTTP 请求、媒体会话、事件循环的顺序停止服务并释放资源。 */
  ~Impl()
  {
    // 先停止 HTTP 服务并等待请求处理结束，再销毁会话及事件循环。
    http.stop();
    http_thread.join();
    encoder.reset(); // 先停止分发，随后才能销毁被分发线程访问的会话。
    peers.clear();
    g_main_loop_quit(loop);
    loop_thread.join();
    g_main_loop_unref(loop);
  }
};
/** @brief 创建服务实现，完成网页加载、端口绑定和工作线程启动。 */
WebRtcServer::WebRtcServer(int port, const std::string &page, std::function<WebStatus()> status)
    : impl_(std::make_unique<Impl>(port, page, std::move(status))) {}
/** @brief 通过实现对象析构停止服务，等待请求及媒体线程结束。 */
WebRtcServer::~WebRtcServer() = default;
/** @brief 查询 HTTP 监听线程是否仍在运行，供主循环判断服务是否异常退出。 */
bool WebRtcServer::healthy() const { return impl_->listening && !impl_->encoder_failed; }
/**
 * @brief 回收失效会话，并将当前图像分发给仍然有效的观看者。
 * @param jpeg 最新原始消息，可包含 CJPG 头；允许为空，此时只执行会话回收。
 */
void WebRtcServer::push(const xbus::SharedBuffer &jpeg)
{
  // 1. 在集合锁内移除失效会话，再复制有效会话引用。
  std::vector<std::shared_ptr<Peer>> peers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto &all = impl_->peers;
    all.erase(std::remove_if(all.begin(), all.end(), [](const auto &p)
                             { return p->expired(); }),
              all.end());
    // 复制会话引用，在集合锁之外发送时仍保持会话有效。
    peers = all;
  }
  // 2. 无新图像时结束；其余情况在集合锁外逐个投递，避免阻塞协商接口。
  if (jpeg.empty() || std::none_of(peers.begin(), peers.end(), [](const auto& p) { return p->negotiated.load(); }))
    return;
  // 在持有原始 SharedBuffer 期间取得正文视图，跳过 CJPG 头再交给媒体管线。
  camframe::JpegView view;
  if (camframe::parse_jpeg(jpeg.data(), jpeg.size(), view))
    return;
  // 3. 首个观看者到来时创建共享编码器，此后不再按观看人数创建编码器。
  if (!impl_->encoder) {
    try {
      impl_->encoder = std::make_unique<SharedEncoder>(impl_->request_keyframe,
        [this](GstBuffer* encoded) {
          ++impl_->encoded_frames;
          std::vector<std::shared_ptr<Peer>> targets;
          {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            targets = impl_->peers;
          }
          for (const auto& peer : targets) peer->push(encoded);
        }, impl_->encoder_failed);
      impl_->encoder_instances = 1;
      std::cerr << "Shared VP8 encoder started (one encoder for all viewers)\n";
    } catch (const std::exception& error) {
      std::cerr << "Shared encoder startup failed: " << error.what() << '\n';
      impl_->encoder_failed = true;
      return;
    }
  }
  impl_->encoder->push(view);
}
