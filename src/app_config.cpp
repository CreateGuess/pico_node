#include "app_config.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <set>

/** @brief 读取内核提供的程序位置；读取失败由主函数报告原因。 */
std::filesystem::path executableDirectory()
{
  // /proc/self/exe：一个符号链接，指向当前进程正在执行的可执行文件
  // std::filesystem::read_symlink(const std::filesystem::path& p)：读取符号链接的目标路径，返回std::filesystem::path
  return std::filesystem::read_symlink("/proc/self/exe").parent_path();
}

/**
 * @brief 将配置中的相对路径转换为相对于配置目录的绝对路径。
 * @param base 配置目录。
 * @param value 文件路径。
 * @return 规范化后的路径字符串。
 */
static std::string resourcePath(const std::filesystem::path &base, const std::string &value)
{
  return (base / value).lexically_normal().string();
}

/**
 * @brief 读取范围内的整数，拒绝浮点数、布尔值及溢出值。
 * @param value 配置字段。
 * @param minimum 最小值。
 * @param maximum 最大值。
 * @return 校验后的整数。
 */
static int integerField(const nlohmann::json &value, int minimum, int maximum)
{
  if (!value.is_number_integer() || value < minimum || value > maximum)
    throw std::runtime_error("端口、码率或关键帧间隔必须是允许范围内的整数");
  return value.get<int>();
}

/**
 * @brief 读取 JSON，检查运行模式、话题、端口与硬件编码参数。
 * @param path 配置文件绝对路径。
 * @return 可直接用于启动服务的配置。
 * @throws std::runtime_error 文件不可读或字段取值非法。
 */
AppConfig loadConfig(const std::filesystem::path &path)
{
  // 1. 读取完整配置，必填字段使用 at，避免错误配置被默认值掩盖。
  std::ifstream file(path);
  if (!file)
  {
    throw std::runtime_error("无法读取配置文件：" + path.string());
  }
  nlohmann::json json;
  file >> json;
  AppConfig config;
  // 解析topic
  if (json.contains("topics"))
  {
    config.topics = json.at("topics").get<std::vector<std::string>>();
  }
  std::set<std::string> unique;
  // topic列表为空，抛出异常
  if (config.topics.empty())
  {
    throw std::runtime_error("topics 至少填写一个话题");
  }
  // 检查topic是否为空或重复
  for (const auto &topic : config.topics)
  {
    if (topic.empty() || topic.find_first_not_of(" \t\r\n") == std::string::npos)
      throw std::runtime_error("topics 不能包含空话题");
    if (!unique.insert(topic).second)
      throw std::runtime_error("topics 包含重复话题：" + topic);
  }
  config.mode = json.at("mode").get<std::string>();
  config.port = integerField(json.at("web").at("port"), 1, 65535);
  config.page = resourcePath(path.parent_path(), json.at("web").at("page").get<std::string>());
  config.output = resourcePath(path.parent_path(), json.at("output").get<std::string>());
  const auto &video = json.contains("video") ? json.at("video") : json.at("h264");
  config.codec = video.value("codec", std::string("h264"));
  if (config.codec != "h264" && config.codec != "vp8")
    throw std::runtime_error("video.codec 必须为 h264 或 vp8");
  config.bitrate = integerField(video.at("bitrate"), 64000, 100000000);
  config.gop = integerField(video.at("gop"), 1, 300);
  // 2. 在连接 Hub 前检查配置，避免非法端口或编码参数延迟到播放时失败。
  if (config.mode != "web" && config.mode != "receive" && config.mode != "once")
    throw std::runtime_error("mode 必须为 web、receive 或 once");
  return config;
}
