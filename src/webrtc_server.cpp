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
#include <map>
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

    // gst_promise_new_with_change_func(Promise状态改变时的回调函数，传递给回调的自定义数据，用户数据的销毁回调)
    auto *promise = gst_promise_new_with_change_func([](GstPromise *p, gpointer data)
                                                     {
    // 将 void* 转换回原始的 shared_ptr 指针类型。
    auto output = *static_cast<decltype(result)*>(data);
    const GstStructure* reply = nullptr;
    // 判断 GstPromise 状态
    // 四种状态：Pending（等待中）、Interrupt（中断）、Expire（结果失效）、Replied（已收到响应）
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

    // 三种状态：ready（已就绪）、timeout（超时）、deferred（延迟）
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
      // 共享回调状态，确保超时与异步完成同时发生时数据仍然有效。
      // 表示调用者不再等待这个 Promise 的结果
      gst_promise_interrupt(promise);
      // 负责减少引用计数，当引用计数归零时销毁对象
      gst_promise_unref(promise);
      throw std::runtime_error("WebRTC operation timed out");
    }
    // 从 std::promise 的共享状态中获取 Structure 对象
    auto reply = future.get();
    gst_promise_unref(promise);
    // 判断 GstStructure 中是否存在指定名称的字段
    if (reply && gst_structure_has_field(reply.get(), "error"))
      throw std::runtime_error("WebRTC rejected SDP operation");
    return reply;
  }

  // 每个观看者仅持有 RTP 打包和 WebRTC 连接，共用服务端的 H.264 编码结果。
  struct Peer
  {
    GstElement *pipeline = nullptr;
    GstElement *source = nullptr;
    GstElement *rtc = nullptr;
    // 会话标识
    std::string id;
    // 会话创建时间
    Clock::time_point born = Clock::now();
    // negotiate：Peer 是否已经完成代码定义的 SDP 协商准备阶段
    // congested：标记当前 Peer 是否存在拥塞或数据积压
    std::atomic<bool> negotiated{false}, congested{false};
    // 标记当前 Peer 是否需要关键帧，供编码分发线程读写
    bool waiting_keyframe = true; // 仅由编码分发线程读写。
    // 多个 Peer 与共享编码器之间的关键帧请求机制
    std::shared_ptr<std::atomic<bool>> request_keyframe;

    /**
     * @brief 为一个观看者创建 H.264 到 WebRTC 的发送管线，不创建解码器和编码器。
     * @throws std::runtime_error 管线解析或启动失败。
     */
    explicit Peer(std::shared_ptr<std::atomic<bool>> request, const std::string &codec) : request_keyframe(std::move(request))
    {
      // 1. 生成会话标识，供网页断开时删除对应会话。

      // 生成随机 UUID，并将其转换为字符串形式，作为当前 Peer 的唯一标识
      gchar *uuid = g_uuid_string_random();
      id = uuid;
      g_free(uuid);
      GError *error = nullptr;
      // 2. 输入共享编码器的 H.264 结果，各会话独立打包、加密和发送。

      /**
       * appsrc 参数解析：
       * name=jpeg	为组件指定名称
       * is-live=true	声明这是实时数据源
       * format=time	使用时间格式描述流的 Segment
       * do-timestamp=true	为没有适当时间戳的输入数据自动生成时间戳
       * block=false	不因内部队列达到阈值而阻塞数据投递
       * max-bytes=2097152	设置内部队列的字节数阈值为 2 MiB
       *
       * H.264 Caps 参数解析:video/x-h264	H.264 编码视频
       * stream-format=byte-stream	Annex B 字节流格式
       * alignment=au	按 Access Unit 对齐
       * profile=constrained-baseline	H.264 受限基线配置
       * level=3.1	H.264 Level 3.1
       * width=1280	视频宽度
       * height=720	视频高度
       * framerate=30/1	视频帧率 30 FPS
       *
       * rtph264pay 参数解析
       * pt=96  将输出 RTP 包的 Payload Type 设置为 96
       * config-interval=-1 设置 SPS/PPS 配置间隔为 -1s，每个 IDR 帧发送 SPS/PPS
       * aggregate-mode=zero-latency 聚合 NAL Unit，直到包含 VCL NAL Unit
       *
       * RTP Caps
       * application/x-rtp	RTP 媒体数据
       * media=video	视频类型
       * encoding-name=H264	RTP 中封装的是 H.264
       * payload=96	RTP Payload Type
       * clock-rate=90000	RTP 时间戳时钟频率为 90kHz
       * packetization-mode=1	非交错封包模式
       *
       * webrtcbin 参数解析
       * name=rtc	为组件指定名称
       * bundle-policy=max-bundle	表示尽可能将多个媒体流复用到同一个 ICE/DTLS Transport 上
       */
      const std::string description = codec == "vp8" ?
          "appsrc name=jpeg is-live=true format=time do-timestamp=true block=false max-bytes=2097152 caps=video/x-vp8,width=1280,height=720,framerate=30/1 "
          "! rtpvp8pay pt=96 ! application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000 ! webrtcbin name=rtc bundle-policy=max-bundle" :
          "appsrc name=jpeg is-live=true format=time do-timestamp=true block=false max-bytes=2097152 "
          "caps=video/x-h264,stream-format=byte-stream,alignment=au,profile=constrained-baseline,level=3.1,width=1280,height=720,framerate=30/1 "
          "! rtph264pay pt=96 config-interval=-1 aggregate-mode=zero-latency ! application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000,packetization-mode=(string)1 "
          "! webrtcbin name=rtc bundle-policy=max-bundle";
      pipeline = gst_parse_launch(description.c_str(), &error);
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

      // 根据组件名称从 GstBin 中查找对应的 Element
      source = gst_bin_get_by_name(GST_BIN(pipeline), "jpeg");
      rtc = gst_bin_get_by_name(GST_BIN(pipeline), "rtc");

      // 将浏览器丢包后请求的关键帧转交共享编码器，回调状态独立持有。
      auto *pad = gst_element_get_static_pad(source, "src");
      auto *flag = new std::shared_ptr<std::atomic<bool>>(request_keyframe);

      // 	GST_PAD_PROBE_TYPE_EVENT_UPSTREAM：监听上游事件
      gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM, [](GstPad *, GstPadProbeInfo *info, gpointer data) -> GstPadProbeReturn
                        {
           // 获取事件并判断关键帧请求
          if (gst_video_event_is_force_key_unit(GST_PAD_PROBE_INFO_EVENT(info)))
            **static_cast<std::shared_ptr<std::atomic<bool>>*>(data) = true;
          return GST_PAD_PROBE_OK; }, flag, [](gpointer data)
                        { delete static_cast<std::shared_ptr<std::atomic<bool>> *>(data); });
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
    /** @brief 释放该观看者的打包、传输及连接资源。 */
    ~Peer() { close(); }

    /**
     * @brief 接收浏览器的 SDP 提议，生成含完整 ICE 候选地址的 SDP 应答。
     * @param offer 浏览器创建的视频接收提议，需包含 H.264 能力。
     * @return 可直接交给浏览器设置远端描述的 SDP 文本。
     * @throws std::runtime_error SDP 无效、异步操作失败或 ICE 收集超过 10 秒。
     */
    std::string answer(const std::string &offer)
    {
      // 1. 解析浏览器 SDP，并设置为当前连接的远端描述。
      GstSDPMessage *sdp = nullptr;
      // 分配并初始化的 SDP 消息对象
      gst_sdp_message_new(&sdp);
      // 将原始 SDP 文本解析后存储到 GstSDPMessage 对象中
      if (gst_sdp_message_parse_buffer(reinterpret_cast<const guint8 *>(offer.data()), offer.size(), sdp) != GST_SDP_OK)
      {
        gst_sdp_message_free(sdp);
        throw std::runtime_error("Invalid SDP");
      }
      // 创建WebRTC会话描述对象，类型为OFFER，包含解析后的SDP消息
      // GST_WEBRTC_SDP_TYPE_OFFER：发起协商提议
      Description remote(gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp),
                         gst_webrtc_session_description_free);
      // 设置远端描述，触发 ICE 候选收集
      action(rtc, "set-remote-description", remote.get());

      // 2. 根据双方媒体能力生成应答，并设置本地描述以启动 ICE 候选收集

      // 创建本地 SDP Answer
      auto reply = action(rtc, "create-answer");

      // 从 GstStructure 中提取 answer 字段
      GstWebRTCSessionDescription *local = nullptr;
      if (!reply || !gst_structure_get(reply.get(), "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &local, nullptr) || !local)
        throw std::runtime_error("Cannot create SDP answer (browser must offer H.264 video)");

      // 使用智能指针管理本地SDP
      Description local_description(local, gst_webrtc_session_description_free);

      // 检查视频通道是否协商成功
      bool accepted_video = false;
      for (guint i = 0; i < gst_sdp_message_medias_len(local->sdp); ++i)
      {
        const auto *media = gst_sdp_message_get_media(local->sdp, i);
        // 在SDP Offer/Answer协商中，对于普通的媒体描述，如果Answer中对应的media行的端口号为0，
        // 表示该媒体流被拒绝或不接受。反之，如果端口号不为0，则表示该媒体流被接受。
        if (std::string(gst_sdp_media_get_media(media)) == "video" && gst_sdp_media_get_port(media) != 0)
          accepted_video = true;
      }
      if (!accepted_video)
        throw std::runtime_error("浏览器未协商出可用的 H.264 视频通道");

      // 将前面生成的 SDP Answer 设置为 webrtcbin 的本地会话描述
      action(rtc, "set-local-description", local_description.get());

      // 3. 采用非增量 ICE 协商，每 20 毫秒查询一次，最多等待 10 秒。
      const auto deadline = Clock::now() + std::chrono::seconds(10);
      while (Clock::now() < deadline)
      {
        // 获取ICE候选地址收集状态
        GstWebRTCICEGatheringState state;
        g_object_get(rtc, "ice-gathering-state", &state, nullptr);
        // 三种状态：new（新建）、gathering（收集候选地址）、complete（收集完成）
        if (state == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE)
        {
          // 4. 重新读取包含候选地址的本地描述，序列化后允许主循环投递图像。
          g_object_get(rtc, "local-description", &local, nullptr);
          if (!local)
            throw std::runtime_error("Missing local SDP");

          // 将本地SDP序列化为文本格式，供浏览器设置远端描述
          gchar *text = gst_sdp_message_as_text(local->sdp);
          std::string answer = text;
          // 释放本地SDP和文本内存
          g_free(text);
          gst_webrtc_session_description_free(local);
          // 设置协商完成标志
          negotiated = true;
          // 返回已经包含 ICE Candidate 的 SDP Answer
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
      if (congested)
        return true;
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
     * @brief 向当前观看者发送共享的 H.264 帧，只复制缓冲元数据，不复制编码正文。
     * @param encoded 共享编码器输出，调用期间由分发线程持有。
     * @details 新会话等待关键帧；慢会话超过 2 MiB 积压后回收，避免丢参考帧导致花屏。
     */
    void push(GstBuffer *encoded)
    {
      if (!negotiated || congested)
        return;
      // 1. 新会话从完整关键帧开始，不能从其他帧的预测数据开始解码。
      if (waiting_keyframe)
      {
        if (GST_BUFFER_FLAG_IS_SET(encoded, GST_BUFFER_FLAG_DELTA_UNIT))
        {
          *request_keyframe = true;
          return;
        }
        waiting_keyframe = false;
      }
      // 2. 单个慢客户端不阻塞其他观看者；不任意丢弃已经编码的参考帧。
      guint64 queued = 0;
      g_object_get(source, "current-level-bytes", &queued, nullptr);
      if (queued > 2 * 1024 * 1024)
      {
        std::cerr << "Slow viewer disconnected: " << id << '\n';
        congested = true;
        return;
      }
      // 3. 正文内存通过引用共享，每个会话重新按自己的运行时钟打时间戳。
      auto *buffer = gst_buffer_copy(encoded);
      GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
      GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
      if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK)
        congested = true;
    }
  };

  /** @brief 同一 topic 的观看者共享一套 JPEG 解码和 H.264 编码管线，独立线程拉取编码帧。 */
  struct SharedEncoder
  {
    // 整个编码管线
    GstElement *pipeline = nullptr;
    // 输入JPEG图像
    GstElement *source = nullptr;
    // 获取H.264编码结果
    GstElement *sink = nullptr;
    // 硬件编码器组件，供请求关键帧使用
    GstElement *codec = nullptr;
    // 用于控制编码分发线程的运行与退出
    std::atomic<bool> running{true};
    std::thread worker;

    /**
     * @brief 创建共享 MPP 硬件编码管线，通过独立线程拉取并分发 H.264 访问单元。
     * @param request 浏览器或新会话发出的关键帧请求标志。
     * @param distribute 编码帧分发回调，缓冲仅在回调期间保持有效。
     * @param failed 管线错误标志，由主循环检查并退出服务。
     * @param bitrate 目标码率，单位为比特每秒。
     * @param gop 关键帧间隔，单位为帧。
     * @param selected_codec 选择的编码格式。
     * @throws std::runtime_error 插件、管线解析或启动失败。
     */
    SharedEncoder(std::shared_ptr<std::atomic<bool>> request,
                  std::function<void(GstBuffer *)> distribute, std::atomic<bool> &failed, int bitrate, int gop, const std::string &selected_codec)
    {
      // 1. 软件解码 JPEG 并转换为 NV12；MPP 编码固定为 720p 基线规格。
      GError *error = nullptr;

      /**
       * appsrc：接收 JPEG 图像
       * name=input	输入组件名称
       * is-live=true	实时数据源
       * format=time	使用时间格式描述数据流
       * do-timestamp=true	自动生成缺失的时间戳
       * block=false	不因内部队列达到阈值而阻塞投递线程
       * max-bytes=8388608	内部队列字节数阈值为 8 MiB
       * caps=image/jpeg	输入为 JPEG 压缩图像
       * framerate=30/1	声明输入帧率为 30 FPS
       *
       * queue：丢弃过期 JPEG 图像
       * max-size-buffers=1	队列最多缓存一个 Buffer
       * max-size-bytes=0	禁用字节数限制
       * max-size-time=0	禁用缓存时间限制
       * leaky=downstream	队列满时丢弃旧 Buffer
       *
       * jpegdec：JPEG 解码
       *
       * videoconvert：像素格式转换
       *
       * videoscale：调整分辨率
       *
       * mpph264enc：Rockchip 硬件编码
       * name=encoder	编码器组件名称
       * profile=baseline	使用 H.264 Baseline Profile
       * level=3.1	使用 H.264 Level 3.1
       * rc-mode=cbr	恒定码率控制模式
       * bps	目标编码码率
       * gop	图像组长度、关键帧间隔
       *
       * h264parse：解析 H.264 数据
       *
       * H.264 输出 Caps
       *
       * appsink：输出编码数据
       * name=output	输出组件名称
       * sync=false	不根据 Pipeline 时钟等待预定播放时间
       * max-buffers=8	内部队列最多缓存 8 个 Buffer
       * drop=false	队列满时不主动丢弃编码数据，而是产生背压
       */
      const std::string description = selected_codec == "vp8" ?
          "appsrc name=input is-live=true format=time do-timestamp=true block=false max-bytes=8388608 caps=image/jpeg,framerate=30/1 "
          "! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! jpegdec ! videoconvert ! videoscale "
          "! video/x-raw,format=I420,width=1280,height=720 ! vp8enc name=encoder deadline=1 cpu-used=8 target-bitrate=" +
          std::to_string(bitrate) + " keyframe-max-dist=" + std::to_string(gop) +
          " ! appsink name=output sync=false max-buffers=8 drop=false" :
          "appsrc name=input is-live=true format=time do-timestamp=true block=false max-bytes=8388608 "
          "caps=image/jpeg,framerate=30/1 ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream "
          "! jpegdec ! videoconvert ! videoscale ! video/x-raw,format=NV12,width=1280,height=720 "
          "! mpph264enc name=encoder profile=baseline level=3.1 rc-mode=cbr bps=" +
          std::to_string(bitrate) + " gop=" + std::to_string(gop) +
          " ! h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au,profile=constrained-baseline,level=3.1 "
          "! appsink name=output sync=false max-buffers=8 drop=false";

      // 创建并启动 Pipeline
      pipeline = gst_parse_launch(description.c_str(), &error);
      if (error || !pipeline)
      {
        std::string message = error ? error->message : "Cannot create shared encoder";
        if (error)
          g_error_free(error);
        if (pipeline)
          gst_object_unref(pipeline);
        throw std::runtime_error(message);
      }
      // 分别获取输入、输出及编码器组件的指针
      source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
      sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
      codec = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
      // 启动编码 Pipeline
      if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      {
        release();
        throw std::runtime_error("Cannot start shared encoder");
      }
      // 2. 编码帧不能随意丢弃；分发线程持续取出，背压时在 JPEG 输入端丢旧帧。
      worker = std::thread([this, request, distribute, &failed]
                           {
        // 获取 GStreamer Bus消息总线
        auto* bus = gst_element_get_bus(pipeline);
        while (running) {
          // 2.1 请求 IDR 关键帧，使新加入或丢包后的观看者恢复解码。
          if (request->exchange(false)) {
            // 获取编码器 src Pad
            auto* pad = gst_element_get_static_pad(codec, "src");
            // gst_video_event_new_upstream_force_key_unit：创建 ForceKeyUnit 事件，请求尽快生成关键帧，请求同时输出必要的编码头信息
            // gst_pad_send_event：将事件发送给编码器，请求编码器产生新的关键帧
            gst_pad_send_event(pad, gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0));
            gst_object_unref(pad);
          }
          // 2.2 每次取一个完整访问单元，解析器随 IDR 输出参数集。

          // 从 appsink 获取GstSample编码数据，GstSample包含媒体数据以及相关上下文信息封装
          auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
          if (sample) {
            distribute(gst_sample_get_buffer(sample));
            gst_sample_unref(sample);
          }
          // 2.3 硬件驱动或编码出错时通知主循环，不回退到软件编码。
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
        gst_object_unref(bus); });
    }

    /** @brief 停止媒体处理后释放组件引用；启动失败时也调用。 */
    void release()
    {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(source);
      gst_object_unref(sink);
      gst_object_unref(codec);
      gst_object_unref(pipeline);
    }
    /** @brief 先等待分发线程结束，再释放媒体管线，避免线程访问已释放的会话。 */
    ~SharedEncoder()
    {
      running = false;
      worker.join();
      release();
    }

    /** @brief 仅复制一次 JPEG 到共享编码器，输入积压时跳过当前帧。 */
    void push(const camframe::JpegView &jpeg)
    {
      guint64 queued = 0;
      g_object_get(source, "current-level-bytes", &queued, nullptr);
      if (queued)
        return;
      auto *buffer = gst_buffer_new_allocate(nullptr, jpeg.size, nullptr);
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
  // 每路独立保留编码器、关键帧请求与会话，避免不同相机的数据混流。
  struct Stream
  {
    std::vector<std::shared_ptr<Peer>> peers;
    std::atomic<bool> failed{false};
    std::shared_ptr<std::atomic<bool>> request_keyframe = std::make_shared<std::atomic<bool>>(true);
    std::unique_ptr<SharedEncoder> encoder;
    std::atomic<unsigned> encoder_instances{0};
    std::atomic<unsigned long long> encoded_frames{0};
  };
  std::map<std::string, std::unique_ptr<Stream>> streams;
  std::atomic<bool> listening{true};
  std::string page;
  int bitrate, gop;
  std::string codec;
  std::function<std::vector<WebStatus>()> status;

  /**
   * @brief 初始化媒体环境、注册网页接口，并启动事件线程与 HTTP 线程。
   * @param port HTTP 监听端口。
   * @param path 网页文件路径。
   * @param target_bitrate H.264 目标码率。
   * @param keyframe_interval 关键帧间隔。
   * @param topics 配置中的相机话题。
   * @param get_status 获取各话题接收计数及最近帧时间的回调。
   * @throws std::runtime_error 插件缺失、网页读取失败或端口绑定失败。
   */
  Impl(int port, const std::string &path, int target_bitrate, int keyframe_interval, const std::string &selected_codec, const std::vector<std::string> &topics, std::function<std::vector<WebStatus>()> get_status) : bitrate(target_bitrate), gop(keyframe_interval), codec(selected_codec), status(std::move(get_status))
  {
    for (const auto &topic : topics)
      streams.emplace(topic, std::make_unique<Stream>());
    // 1. 初始化 GStreamer，并检查媒体链路需要的运行时插件。
    gst_init(nullptr, nullptr);
    for (const char *name : {"appsrc", "appsink", "jpegdec", "videoconvert", "videoscale", codec == "vp8" ? "vp8enc" : "mpph264enc", codec == "vp8" ? "rtpvp8pay" : "h264parse", codec == "vp8" ? "rtpvp8pay" : "rtph264pay", "webrtcbin", "nicesrc"})
    {
      // 要查找Element工厂名称，返回对应GstElementFactory指针
      auto *factory = gst_element_factory_find(name);
      if (!factory)
        throw std::runtime_error(std::string("缺少 GStreamer 插件：") + name + "；请安装所选编码对应的插件及依赖");
      // 释放工厂引用，避免内存泄漏
      gst_object_unref(factory);
    }
    // 2. 启动时读取网页，配置请求大小及网络读写超时。
    std::ifstream file(path, std::ios::binary);
    if (!file)
      throw std::runtime_error("Cannot open webpage: " + path);
    // 将网页内容读取到字符串中，使用 istreambuf_iterator 迭代器从文件流中读取字符，直到文件结束
    page.assign(std::istreambuf_iterator<char>(file), {});
    // 设置 HTTP 请求的最大负载长度为 64KB，防止过大的请求导致服务器压力过大
    http.set_payload_max_length(64 * 1024);
    http.set_read_timeout(10, 0);
    http.set_write_timeout(10, 0);
    // 3. 注册网页和状态接口；状态计数来自 XBus，不代表浏览器已呈现帧数。
    http.Get("/", [this](const httplib::Request &, httplib::Response &res)
             { res.set_content(page, "text/html; charset=utf-8"); });
    http.Get("/api/status", [this](const httplib::Request &, httplib::Response &res)
             {
      const auto snapshots = status();
      Json cameras = Json::array();
      unsigned long long received = 0, invalid = 0;
      std::size_t peer_count = 0;
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& snapshot : snapshots) {
        const auto& stream = *streams.at(snapshot.topic);
        Json camera{{"topic", snapshot.topic}, {"received", snapshot.received}, {"invalid", snapshot.invalid},
          {"peers", stream.peers.size()}, {"source_alive", snapshot.frame_age_seconds >= 0 && snapshot.frame_age_seconds < 5},
          {"codec", codec == "vp8" ? "VP8" : "H264"}, {"encoder", codec == "vp8" ? "vp8enc" : "mpph264enc"}, {"encoder_failed", stream.failed.load()},
          {"encoder_instances", stream.encoder_instances.load()}, {"encoded_frames", stream.encoded_frames.load()}};
        camera["frame_age_seconds"] = snapshot.frame_age_seconds < 0 ? Json(nullptr) : Json(snapshot.frame_age_seconds);
        cameras.push_back(std::move(camera));
        received += snapshot.received; 
        invalid += snapshot.invalid; 
        peer_count += stream.peers.size();
      }
      res.set_content(Json{{"cameras", cameras}, {"received", received}, {"invalid", invalid}, {"peers", peer_count}}.dump(), "application/json"); });

    // 4. 注册协商接口：校验提议 → 分配会话 → 生成应答 → 返回 SDP 和会话标识。
    http.Post("/offer", [this](const httplib::Request &req, httplib::Response &res)
              {
      std::shared_ptr<Peer> peer;
      try {
        Json json = Json::parse(req.body);
        if (json.at("type") != "offer" || !json.at("sdp").is_string()) throw std::runtime_error("Expected SDP offer");
        {
          std::lock_guard<std::mutex> lock(mutex);
          const auto topic = json.at("topic").get<std::string>();
          const auto found = streams.find(topic);
          if (found == streams.end()) { res.status = 404; res.set_content("未配置该相机话题", "text/plain; charset=utf-8"); return; }
          auto& stream = *found->second;
          if (stream.failed) { res.status = 503; res.set_content("该相机编码异常，请检查服务日志", "text/plain; charset=utf-8"); return; }
          if (stream.peers.size() >= 4) { res.status = 503; res.set_content("Viewer limit reached", "text/plain"); return; }
          peer = std::make_shared<Peer>(stream.request_keyframe, codec);
          stream.peers.push_back(peer);
        }
        // ICE 候选收集可能阻塞，协商期间不持有会话集合的互斥锁。
        auto sdp = peer->answer(json.at("sdp").get<std::string>());
        res.set_content(Json{{"type", "answer"}, {"sdp", sdp}, {"id", peer->id}}.dump(), "application/json");
      } catch (const std::exception& error) {
        if (peer) remove(peer->id);
        std::cerr << "Offer failed: " << error.what() << '\n';
        res.status = 400; res.set_content("Cannot negotiate video; check server logs and H.264 support", "text/plain");
      } });
    // 5. 注册主动断开接口；移除集合引用后，由最后一个持有者释放媒体资源。
    http.Delete(R"(/session/([a-f0-9-]+))", [this](const httplib::Request &req, httplib::Response &res)
                { remove(req.matches[1].str()); res.status = 204; });
    // 6. 绑定端口，先启动媒体事件循环，再启动 HTTP 请求处理。
    if (!http.bind_to_port("0.0.0.0", port))
      throw std::runtime_error("Cannot bind HTTP port");
    // 创建GStreamer 的事件循环
    loop = g_main_loop_new(nullptr, FALSE);
    std::promise<void> running;
    auto started = running.get_future();

    // g_idle_add() 会创建一个 Idle Source，并将它注册到全局默认的 GMainContext 中
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
    for (auto &entry : streams)
    {
      auto &peers = entry.second->peers;
      peers.erase(std::remove_if(peers.begin(), peers.end(), [&](const auto &peer)
                                 { return peer->id == id; }),
                  peers.end());
    }
  }
  /** @brief 按 HTTP 请求、媒体会话、事件循环的顺序停止服务并释放资源。 */
  ~Impl()
  {
    // 先停止 HTTP 服务并等待请求处理结束，再销毁会话及事件循环。
    http.stop();
    http_thread.join();
    // 先停止所有分发线程，随后才能销毁会话与被线程访问的话题状态。
    for (auto &entry : streams)
      entry.second->encoder.reset();
    streams.clear();
    g_main_loop_quit(loop);
    loop_thread.join();
    g_main_loop_unref(loop);
  }
};
/** @brief 创建服务实现，完成网页加载、端口绑定和工作线程启动。 */
WebRtcServer::WebRtcServer(int port, const std::string &page, int bitrate, int gop, const std::string &codec, const std::vector<std::string> &topics, std::function<std::vector<WebStatus>()> status)
    : impl_(std::make_unique<Impl>(port, page, bitrate, gop, codec, topics, std::move(status))) {}
/** @brief 通过实现对象析构停止服务，等待请求及媒体线程结束。 */
WebRtcServer::~WebRtcServer() = default;
/** @brief 查询 HTTP 监听线程是否仍在运行，供主循环判断服务是否异常退出。 */
unsigned long long WebRtcServer::encodedFrames(const std::string &topic) const
{
  // 1. 话题映射启动后保持不变，累计计数通过原子读取。
  return impl_->streams.at(topic)->encoded_frames.load();
}
/** @brief 查询 HTTP 监听线程是否仍在运行。 */
bool WebRtcServer::healthy() const { return impl_->listening; }
/**
 * @brief 回收失效会话，并将当前图像分发给仍然有效的观看者。
 * @param topic 当前图像所属话题，必须已在配置中注册。
 * @param jpeg 最新原始消息，可包含 CJPG 头；允许为空，此时只执行会话回收。
 */
void WebRtcServer::push(const std::string &topic, const xbus::SharedBuffer &jpeg)
{
  // 1. 按话题定位独立媒体状态，并回收该路已经失效的会话。
  auto &stream = *impl_->streams.at(topic);
  std::vector<std::shared_ptr<Peer>> peers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto &all = stream.peers;
    all.erase(std::remove_if(all.begin(), all.end(), [](const auto &peer)
                             { return peer->expired(); }),
              all.end());
    if (stream.failed)
      all.clear();
    peers = all;
  }
  // 2. 某一路编码失败仅停止该路，其他相机继续提供画面和状态。
  if (stream.failed)
  {
    stream.encoder.reset();
    stream.encoder_instances = 0;
    return;
  }
  if (jpeg.empty() || std::none_of(peers.begin(), peers.end(), [](const auto &peer)
                                   { return peer->negotiated.load(); }))
    return;
  camframe::JpegView view;
  if (camframe::parse_jpeg(jpeg.data(), jpeg.size(), view))
    return;
  // 3. 每个 topic 首次有观看者时启动一套编码器，同一 topic 的所有页面复用编码结果。
  if (!stream.encoder)
  {
    try
    {
      auto *channel = &stream;
      stream.encoder = std::make_unique<SharedEncoder>(stream.request_keyframe, [this, channel](GstBuffer *encoded)
                                                       {
          ++channel->encoded_frames;
          std::vector<std::shared_ptr<Peer>> targets;
          {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            targets = channel->peers;
          }
          for (const auto& peer : targets) peer->push(encoded); }, stream.failed, impl_->bitrate, impl_->gop, impl_->codec);
      stream.encoder_instances = 1;
      std::cerr << "共享编码器启动：codec=" << impl_->codec << " topic=" << topic << '\n';
    }
    catch (const std::exception &error)
    {
      std::cerr << "编码启动失败：topic=" << topic << " reason=" << error.what() << '\n';
      stream.failed = true;
      return;
    }
  }
  // 4. 在持有原始 SharedBuffer 的期间，仅把解包后的 JPEG 复制到对应编码器。
  stream.encoder->push(view);
}
