# 将本地 libnice 源码放入 GStreamer 子项目目录，阻止 Meson 回退到 Git 下载。
file(SHA256 "${ARCHIVE}" _pico_hash)
if(NOT _pico_hash STREQUAL EXPECTED_HASH)
  message(FATAL_ERROR "libnice 源码校验失败：${ARCHIVE}")
endif()
if(NOT EXISTS "${SOURCE_DIR}/subprojects/libnice/meson.build")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${ARCHIVE}"
    WORKING_DIRECTORY "${SOURCE_DIR}/subprojects" RESULT_VARIABLE _pico_extract_result)
  if(NOT _pico_extract_result EQUAL 0)
    message(FATAL_ERROR "无法解压本地 libnice 源码包")
  endif()
  file(RENAME "${SOURCE_DIR}/subprojects/libnice-0.1.22" "${SOURCE_DIR}/subprojects/libnice")
endif()
