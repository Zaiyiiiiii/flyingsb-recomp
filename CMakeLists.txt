cmake_minimum_required(VERSION 3.20)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
project(flyingsb_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 重编译输入：用户自己拥有的原版 FlyingSB.exe。
# 生成出的 C 代码只包含翻译后的指令逻辑，不包含任何原版数据/资源；
# 运行时再从用户的游戏目录读取 exe 的数据段与全部资源。
# ---------------------------------------------------------------------------
set(FLYINGSB_ORIGINAL_EXE "${CMAKE_SOURCE_DIR}/game/FlyingSB.exe" CACHE FILEPATH "原版 FlyingSB.exe（繁体中文版）")
if(NOT EXISTS "${FLYINGSB_ORIGINAL_EXE}")
  message(FATAL_ERROR "找不到原版可执行文件: ${FLYINGSB_ORIGINAL_EXE}\n请用 -DFLYINGSB_ORIGINAL_EXE=<路径> 指定。")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen" CACHE PATH "生成的 C 代码目录（可指向别的平台已生成的目录以复用）")
file(GLOB RECOMP_PY "${CMAKE_SOURCE_DIR}/recomp/*.py" "${CMAKE_SOURCE_DIR}/recomp/*.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_PY} "${FLYINGSB_ORIGINAL_EXE}")
if(NOT EXISTS "${GEN_DIR}/gen_table.c" OR "${FLYINGSB_ORIGINAL_EXE}" IS_NEWER_THAN "${GEN_DIR}/gen_table.c")
  set(NEED_GEN TRUE)
endif()
foreach(f ${RECOMP_PY})
  if("${f}" IS_NEWER_THAN "${GEN_DIR}/gen_table.c")
    set(NEED_GEN TRUE)
  endif()
endforeach()
if(NEED_GEN)
  message(STATUS "正在静态重编译 ${FLYINGSB_ORIGINAL_EXE} ...")
  execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/recomp/main.py" "${FLYINGSB_ORIGINAL_EXE}" "${GEN_DIR}"
            --extra-roots "${CMAKE_SOURCE_DIR}/recomp/extra_roots.txt"
            --hooks "${CMAKE_SOURCE_DIR}/recomp/hooks.txt"
    RESULT_VARIABLE GEN_RESULT)
  if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "重编译失败 (需要 Python3 + capstone + pefile: pip install capstone pefile)")
  endif()
endif()
file(GLOB GEN_SOURCES "${GEN_DIR}/gen_*.c")

# ---------------------------------------------------------------------------
# SDL3（窗口/渲染/输入/手柄/音频的跨平台后端）
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
  # Web：游戏跑在 pthread 工作线程里（原版是阻塞式主循环），SDL 的窗口/事件由主线程代理
  add_compile_options(-pthread)
  add_link_options(-pthread)
endif()

include(FetchContent)
if(ANDROID)
  # Android：SDL 的 Java 层（SDLActivity）按名字加载 libSDL3.so 和 libmain.so，所以 SDL 必须是动态库
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  # Android 15+ 的设备页大小可能是 16KB：.so 的 LOAD 段按 16KB 对齐（同时也满足 4KB 对齐），否则会提示应用不兼容
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384")
else()
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
endif()
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.18
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

# FreeType：渲染文字；字体有内嵌点阵（宋体/新細明體的 12、16 像素）时直接取用点阵字形
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
  GIT_REPOSITORY https://github.com/freetype/freetype.git
  GIT_TAG VER-2-13-3
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(freetype)

file(GLOB RUNTIME_SOURCES "${CMAKE_SOURCE_DIR}/runtime/*.c")
file(GLOB PLATFORM_SOURCES "${CMAKE_SOURCE_DIR}/platform/*.c")

if(ANDROID)
  add_library(flyingsb SHARED ${RUNTIME_SOURCES} ${PLATFORM_SOURCES} ${GEN_SOURCES})
  set_target_properties(flyingsb PROPERTIES OUTPUT_NAME main)   # libmain.so：SDL 在其中找 SDL_main
  target_link_libraries(flyingsb PRIVATE log android)
else()
  add_executable(flyingsb ${RUNTIME_SOURCES} ${PLATFORM_SOURCES} ${GEN_SOURCES})
endif()
target_include_directories(flyingsb PRIVATE "${CMAKE_SOURCE_DIR}/runtime" "${CMAKE_SOURCE_DIR}/platform" "${GEN_DIR}")
target_link_libraries(flyingsb PRIVATE SDL3::SDL3 freetype)
if(NOT WIN32)
  target_link_libraries(flyingsb PRIVATE m)
endif()

if(MSVC)
  target_compile_options(flyingsb PRIVATE /W3 /wd4244 /wd4267 /wd4101 /wd4102)
  set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;/O2")
else()
  target_compile_options(flyingsb PRIVATE -fno-strict-aliasing -Wall -Wno-unused-label -Wno-unused-variable)
  set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-O1;-fno-strict-aliasing")
endif()
if(WIN32)
  target_compile_definitions(flyingsb PRIVATE _CRT_SECURE_NO_WARNINGS)
  # exe 图标：重编译步骤从原版 exe 的资源里提取到 GEN_DIR
  if(EXISTS "${GEN_DIR}/flyingsb.rc")
    enable_language(RC)
    target_sources(flyingsb PRIVATE "${GEN_DIR}/flyingsb.rc")
  endif()
endif()
# 把项目 fonts/ 目录放到可执行文件旁，运行时优先使用其中的 simsun.ttc
if(EXISTS "${CMAKE_SOURCE_DIR}/fonts" AND NOT EMSCRIPTEN AND NOT ANDROID)
  add_custom_command(TARGET flyingsb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/fonts" "$<TARGET_FILE_DIR:flyingsb>/fonts")
endif()
if(EMSCRIPTEN)
  # 输出 flyingsb.html/.js/.wasm（由 server/ 的 Rust 服务器托管）；游戏数据不打包，运行时经 HTTP 向服务器读取
  set_target_properties(flyingsb PROPERTIES SUFFIX ".html")
  target_link_options(flyingsb PRIVATE
    -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=4
    -sINITIAL_MEMORY=640MB -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=1GB -sSTACK_SIZE=4MB
    -sFORCE_FILESYSTEM=1 -lidbfs.js -sEXPORTED_RUNTIME_METHODS=FS,callMain,HEAP32,HEAPU32
    -sINVOKE_RUN=1 -sEXIT_RUNTIME=1
    --shell-file "${CMAKE_SOURCE_DIR}/web/shell.html")
  set_target_properties(flyingsb PROPERTIES LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/shell.html")
endif()
