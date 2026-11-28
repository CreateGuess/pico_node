#include "app_config.hpp"
#include <cstdlib>
#include <functional>
#include <unistd.h>

/** @brief 内置 SDK 模式隔离插件版本；系统 SDK 模式沿用系统环境。 */
void prepareMediaEnvironment(const std::filesystem::path &directory)
{
  if (!PICO_BUNDLED_MEDIA)
    return;
  // 1. 动态库由链接器的相对 RPATH 定位；插件与扫描器在初始化前指定。
  const auto prefix = directory / "third_party/prefix";
  const auto plugins = (prefix / "lib/gstreamer-1.0").string();
  const auto scanner = (prefix / "libexec/gstreamer-1.0/gst-plugin-scanner").string();
  setenv("GST_PLUGIN_PATH_1_0", plugins.c_str(), 1);
  setenv("GST_PLUGIN_SYSTEM_PATH_1_0", plugins.c_str(), 1);
  setenv("GST_PLUGIN_SCANNER_1_0", scanner.c_str(), 1);
  // 2. 扫描器作为独立进程也需要 SDK 动态库路径。
  std::string libraries = (prefix / "lib").string();
  if (const auto *previous = std::getenv("LD_LIBRARY_PATH"))
    libraries += std::string(":") + previous;
  setenv("LD_LIBRARY_PATH", libraries.c_str(), 1);
  // 3. 为每个构建和用户隔离缓存，允许程序目录只读。
  setenv("GST_REGISTRY_1_0", (std::filesystem::temp_directory_path() / ("pico-gst-" + std::to_string(std::hash<std::string>{}(directory.string())) + "-" + std::to_string(getuid()) + ".bin")).c_str(), 1);
}
