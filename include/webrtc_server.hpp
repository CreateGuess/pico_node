#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "xbus/api/node.hpp"

// 统计通过校验的 XBus 消息数，不代表解码成功或实际显示的帧数。
struct WebStatus
{
  std::string topic;
  unsigned long long received = 0, invalid = 0;
  double frame_age_seconds = -1; // 收到第一条通过校验的消息前为负值。
};

/**
 * @brief 管理网页服务、信令接口及每个观看者的 WebRTC 媒体管线。
 * @details 主循环按 topic 投递 JPEG，HTTP 工作线程负责协商；每路媒体线程解码编码一次，再分发给该路观看会话。
 */
class WebRtcServer
{
public:
  /**
   * @brief 加载网页、绑定端口并启动 HTTP 与媒体事件线程。
   * @param port HTTP 监听端口。
   * @param page 网页文件路径。
   * @param bitrate H.264 目标码率，单位为比特每秒。
   * @param gop 关键帧间隔，单位为帧。
   * @param topics 按配置顺序列出的相机话题。
   * @param status 各话题接收状态查询回调，在 HTTP 工作线程执行。
   * @throws std::runtime_error 媒体插件缺失、网页读取或服务启动失败。
   */
  WebRtcServer(int port, const std::string &page, int bitrate, int gop, const std::string &codec, const std::vector<std::string>& topics, std::function<std::vector<WebStatus>()> status);
  /** @brief 停止接收 HTTP 请求，释放媒体会话并等待工作线程结束。 */
  ~WebRtcServer();
  WebRtcServer(const WebRtcServer &) = delete;
  WebRtcServer &operator=(const WebRtcServer &) = delete;

  /**
   * @brief 回收失效会话，并向有效会话投递最新 JPEG。
   * @param topic 当前图像所属的已配置话题。
   * @param jpeg 已校验的原始消息，支持 CJPG 封装或原始 JPEG；为空时仅回收会话。
   * @details 由主循环调用，不得在 XBus 订阅回调中调用。
   */
  void push(const std::string& topic, const xbus::SharedBuffer &jpeg);
  /** @brief 查询 HTTP 监听线程是否仍在运行；停止时返回假。 */
  bool healthy() const;
  /** @brief 获取指定话题的累计编码帧数，供接收主循环计算编码帧率。 */
  unsigned long long encodedFrames(const std::string &topic) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
