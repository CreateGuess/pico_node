#pragma once
#include <filesystem>
#include <string>
#include <vector>

/** @brief 多相机启动配置；topics 顺序决定网页排列，路径相对于配置文件目录解析。 */
struct AppConfig
{
  std::vector<std::string> topics;
  std::string mode;
  std::string output;
  std::string page;
  int port = 8080;
  std::string codec = "h264";
  int bitrate = 2000000;
  int gop = 30;
};

/** @brief 通过 Linux 可执行文件路径定位资源，不依赖当前工作目录。 */
std::filesystem::path executableDirectory();
/** @brief 读取并检查配置；字段缺失、类型错误或取值非法时抛出异常。 */
AppConfig loadConfig(const std::filesystem::path& path);
/** @brief 在媒体初始化前设置本次构建的插件、扫描器和独立缓存路径。 */
void prepareMediaEnvironment(const std::filesystem::path& directory);
