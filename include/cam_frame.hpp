#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace camframe
{
  inline constexpr char kMagic[4] = {'C', 'J', 'P', 'G'};
  inline constexpr std::size_t kHeaderBytes = 20;
  inline constexpr std::size_t kMaxMessageBytes = 8 * 1024 * 1024;

  // 消息头字段按小端逐字节读取，不能用结构体强转代替协议解析。
  struct Header
  {
    std::uint32_t seq = 0;
    std::uint64_t age_ns = 0; // 相机板内采集到发布的耗时，不能作为跨板绝对时间。
    std::uint16_t width = 0;
    std::uint16_t height = 0;
  };

  // 只引用消息内的 JPEG；调用者必须在使用期间持有原始消息缓冲区。
  struct JpegView
  {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    Header header;
    bool wrapped = false;
  };

  /**
   * @brief 读取 CJPG 的 20 字节消息头，不依赖主机字节序或结构体对齐。
   * @param p 消息起始地址。
   * @param n 消息字节数；为零时允许 p 为空。
   * @param out 成功时写入帧序号、板内耗时和图像宽高。
   * @return 长度不足或魔数不匹配时返回假。
   */
  inline bool read_header(const std::uint8_t* p, std::size_t n, Header& out)
  {
    // 1. 确认消息头完整且包含 CJPG 魔数。
    if (n < kHeaderBytes || std::memcmp(p, kMagic, 4) != 0) return false;
    // 2. 按协议偏移和小端字节序读取各字段。
    out = Header{};
    for (unsigned i = 0; i < 4; ++i) out.seq |= std::uint32_t(p[4 + i]) << (8 * i);
    for (unsigned i = 0; i < 8; ++i) out.age_ns |= std::uint64_t(p[8 + i]) << (8 * i);
    out.width = std::uint16_t(p[16]) | (std::uint16_t(p[17]) << 8);
    out.height = std::uint16_t(p[18]) | (std::uint16_t(p[19]) << 8);
    return true;
  }

  /**
   * @brief 从 CJPG 帧消息或兼容的原始 JPEG 中取得图像数据，不复制正文。
   * @param p 原始消息地址。
   * @param n 原始消息长度，包含可能存在的 20 字节封装头。
   * @param out 成功时返回 JPEG 视图及已解析的封装元数据。
   * @return 成功返回空指针，失败返回中文原因；不验证 JPEG 是否能完整解码。
   */
  inline const char* parse_jpeg(const std::uint8_t* p, std::size_t n, JpegView& out)
  {
    // 1. 校验整条消息上限，再通过魔数识别是否带封装头。
    out = JpegView{};
    if (n > kMaxMessageBytes) return "消息超过8MiB";
    if (n < 4) return "消息不足4字节";
    out.wrapped = std::memcmp(p, kMagic, 4) == 0;
    std::size_t offset = 0;
    if (out.wrapped) {
      if (!read_header(p, n, out.header)) return "CJPG头不足20字节";
      offset = kHeaderBytes;
    }
    // 2. 仅检查 JPEG 正文的首尾标记，不将封装头交给解码器或写入图片文件。
    const auto size = n - offset;
    if (size < 4) return "JPEG正文不足4字节";
    if (p[offset] != 0xff || p[offset + 1] != 0xd8) return "JPEG正文缺少起始FFD8";
    if (p[n - 2] != 0xff || p[n - 1] != 0xd9) return "JPEG正文缺少结束FFD9";
    out.data = p + offset;
    out.size = size;
    return nullptr;
  }
}
