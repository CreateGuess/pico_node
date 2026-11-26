# 单头文件库直接随项目分发，配置及编译阶段不联网下载。
foreach(_pico_header IN ITEMS
    "cpp-httplib/httplib.h"
    "nlohmann-json/include/nlohmann/json.hpp")
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_pico_header}")
    message(FATAL_ERROR "缺少第三方头文件：third_party/${_pico_header}，请按 third_party/README.md 补齐")
  endif()
endforeach()

add_library(pico_third_party_httplib INTERFACE)
add_library(pico_third_party::httplib ALIAS pico_third_party_httplib)
target_include_directories(pico_third_party_httplib SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/cpp-httplib")
target_link_libraries(pico_third_party_httplib INTERFACE Threads::Threads)
if(WIN32)
  target_link_libraries(pico_third_party_httplib INTERFACE ws2_32)
endif()

add_library(pico_third_party_json INTERFACE)
add_library(pico_third_party::json ALIAS pico_third_party_json)
target_include_directories(pico_third_party_json SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann-json/include")
