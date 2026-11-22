cmake_minimum_required(VERSION 3.20)
project(TeacherBox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 仅面向 Windows。WebView2 SDK 放在 _deps/webview2（NuGet 解包后的目录）。
# 解包方法见 README：tools/fetch_webview2.ps1
# ---------------------------------------------------------------------------
if(NOT WIN32)
  message(FATAL_ERROR "TeacherBox 只支持 Windows。")
endif()

set(TB_WEBVIEW2_ROOT "${CMAKE_SOURCE_DIR}/_deps/webview2" CACHE PATH "WebView2 SDK 根目录")
set(TB_WEBVIEW2_INCLUDE "${TB_WEBVIEW2_ROOT}/build/native/include")

if(NOT EXISTS "${TB_WEBVIEW2_INCLUDE}/WebView2.h")
  message(FATAL_ERROR "找不到 WebView2 SDK：${TB_WEBVIEW2_INCLUDE}/WebView2.h\n请先运行 tools/fetch_webview2.ps1")
endif()

if(MSVC)
  enable_language(RC)
  add_compile_options(/utf-8 /W3 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# ---------------------------------------------------------------------------
# 公共静态库
# ---------------------------------------------------------------------------
add_library(tb_common STATIC
  src/common/util.cpp
  src/common/json.cpp
  src/common/ipc.cpp
)
target_include_directories(tb_common PUBLIC src/common)
target_link_libraries(tb_common PUBLIC
  user32 shell32 advapi32 ole32 oleaut32 shlwapi version winhttp
)

# ---------------------------------------------------------------------------
# main.exe —— 调度器（无控制台）
# ---------------------------------------------------------------------------
add_executable(tb_main WIN32 src/main/main.cpp resources/app.rc)
target_link_libraries(tb_main PRIVATE tb_common)
target_include_directories(tb_main PRIVATE "${CMAKE_SOURCE_DIR}/resources")
set_target_properties(tb_main PROPERTIES OUTPUT_NAME "main" WIN32_EXECUTABLE TRUE)

# ---------------------------------------------------------------------------
# update.exe —— 升级器（无控制台，自带进度窗口）
# ---------------------------------------------------------------------------
add_executable(tb_update WIN32 src/update/main.cpp resources/app.rc)
target_link_libraries(tb_update PRIVATE tb_common)
target_include_directories(tb_update PRIVATE "${CMAKE_SOURCE_DIR}/resources")
set_target_properties(tb_update PROPERTIES OUTPUT_NAME "update" WIN32_EXECUTABLE TRUE)

# ---------------------------------------------------------------------------
# gui.exe —— WebView2 外壳 + 托盘（无控制台）
# ---------------------------------------------------------------------------
add_executable(tb_gui WIN32
  src/gui/gui.cpp
  src/gui/tools.cpp
  resources/app.rc
)
target_include_directories(tb_gui PRIVATE "${TB_WEBVIEW2_INCLUDE}" src/gui "${CMAKE_SOURCE_DIR}/resources")
target_link_libraries(tb_gui PRIVATE tb_common)
set_target_properties(tb_gui PROPERTIES OUTPUT_NAME "gui" WIN32_EXECUTABLE TRUE)

# 所有产物统一输出到 build/bin，方便装配
set_target_properties(tb_main tb_update tb_gui PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
)

# 清单已由 resources/app.rc 以 RT_MANIFEST 嵌入，禁用 link.exe 默认清单，避免资源重复
if(MSVC)
  target_link_options(tb_main PRIVATE /MANIFEST:NO)
  target_link_options(tb_update PRIVATE /MANIFEST:NO)
  target_link_options(tb_gui PRIVATE /MANIFEST:NO)
endif()

# WebView2Loader.dll 复制到输出目录（我们运行时动态加载它）
if(EXISTS "${TB_WEBVIEW2_ROOT}/build/native/x64/WebView2Loader.dll")
  add_custom_command(TARGET tb_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${TB_WEBVIEW2_ROOT}/build/native/x64/WebView2Loader.dll"
      "$<TARGET_FILE_DIR:tb_gui>/WebView2Loader.dll")
endif()
