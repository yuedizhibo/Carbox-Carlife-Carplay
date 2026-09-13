cmake_minimum_required(VERSION 3.16)
project(carlife_hu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# 本地 SDL 呈现（carlife-hu 工具）是可选的：Core 接入路径只需要协议与媒体转发，
# 在 1 GB 目标上不需要带 SDL/FFmpeg。见 PROJECT_ARCHITECTURE.md 的输入层边界。
option(CARLIFE_WITH_SDL "Build the SDL host sink and the standalone carlife-hu tool" ON)

# ---- 协议侧：wire / transport / session / 蓝牙引导 / 热点规划 / SPP 注册 ----
# SPP 必须走 BlueZ 的 org.bluez.ProfileManager1（sd-bus）：BlueZ 5 里
# 旧的 `sdptool add SP` 已不生效（实测适配器 UUID 里始终没有 1101）。
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(SYSTEMD libsystemd)
endif()

# 内容加密（AUDIT 3.1 / 5.4）：RSA-2048 + AES-ECB/PKCS5。
# 为什么用 OpenSSL：参考实现用的是 Java JCE（RSA/ECB/PKCS1Padding + AES），
# OpenSSL 是它的等价物且不会写错填充；已确认目标机（Debian/aarch64 + WSL）都有
# libcrypto.so.3 与头文件（板上：/usr/include/openssl/evp.h + libcrypto.so.3）。
# 找不到 OpenSSL 时不报错退出，而是把加密能力编译成“不可用”（见 CARLIFE_HAVE_OPENSSL），
# 这样没装 openssl-dev 的环境仍能构建协议主体。
find_package(OpenSSL QUIET)

add_library(carlife_proto STATIC
  src/wire.cpp
  src/bt_link.cpp
  src/wifi_ap.cpp
  src/transport.cpp
  src/session.cpp
  src/encryption.cpp
  src/keycode_map.cpp
  src/spp_profile.cpp
  src/hfp_profile.cpp
  src/hfp_slc.cpp
  src/pairing_agent.cpp
  src/bt_client_profile.cpp)
target_include_directories(carlife_proto PUBLIC include)
target_compile_options(carlife_proto PUBLIC -Wall -Wextra)
target_link_libraries(carlife_proto PUBLIC pthread)
if(OpenSSL_FOUND)
  target_compile_definitions(carlife_proto PUBLIC CARLIFE_HAVE_OPENSSL=1)
  target_link_libraries(carlife_proto PUBLIC OpenSSL::Crypto)
  message(STATUS "CarLife content encryption: enabled (OpenSSL ${OPENSSL_VERSION})")
else()
  message(WARNING "OpenSSL not found; CarLife content encryption will be unavailable at runtime")
endif()
if(SYSTEMD_FOUND)
  target_compile_definitions(carlife_proto PUBLIC CARLIFE_HAVE_SD_BUS=1)
  target_include_directories(carlife_proto PUBLIC ${SYSTEMD_INCLUDE_DIRS})
  target_link_libraries(carlife_proto PUBLIC ${SYSTEMD_LIBRARIES})
  target_link_directories(carlife_proto PUBLIC ${SYSTEMD_LIBRARY_DIRS})
else()
  message(STATUS "libsystemd not found; SPP ProfileManager1 registration stays a stub")
endif()

# ---- 接入 Core 的 InputAdapter 实现 ----
# Core 头文件是外部依赖：本目录被单独 checkout（例如上游仓库）时不要构建这一层。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/MainMenu/include/core/session_core.hpp)
  set(ZERO2W_CORE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  set(_carlife_input_sources src/carlife_input.cpp)
  # 作为外层根工程的一部分构建时，Core 目标已经存在，用它即可；
  # 本目录被单独构建时，把 Core 的两个数据面源文件直接编进来。
  if(NOT TARGET session_core)
    list(APPEND _carlife_input_sources
      ${ZERO2W_CORE}/Core/Convert/src/real_media_store.cpp
      ${ZERO2W_CORE}/Core/MainMenu/src/session_core.cpp)
  endif()
  add_library(carlife_input STATIC ${_carlife_input_sources})
  target_include_directories(carlife_input PUBLIC
    include
    ${ZERO2W_CORE}/Core/MainMenu/include
    ${ZERO2W_CORE}/Core/Convert/include)
  target_compile_options(carlife_input PUBLIC -Wall -Wextra)
  # Core 的数据面头文件使用 C++20（std::span）；本模块其余部分保持 C++17。
  target_compile_features(carlife_input PUBLIC cxx_std_20)
  target_link_libraries(carlife_input PUBLIC carlife_proto)
  if(TARGET session_core)
    target_link_libraries(carlife_input PUBLIC session_core)
  endif()
else()
  message(STATUS "zero2w Core not found next to this module; skipping carlife_input")
endif()

enable_testing()
add_executable(unit-wire tests/unit_wire.cpp)
target_link_libraries(unit-wire PRIVATE carlife_proto)
add_executable(unit-ap tests/unit_ap.cpp)
target_link_libraries(unit-ap PRIVATE carlife_proto)
add_test(NAME unit-wire COMMAND unit-wire)
add_test(NAME unit-ap COMMAND unit-ap)

# 本轮「全量接口」新增的测试：
#   unit-features   —— 全部新消息的编解码 + 内容加密往返（不需网络，毫秒级）
#   feature-probe   —— 真跑一个会话，把 SessionCore 快照打印出来（由 run_wsl_tests.sh 搭配 mdsim 调用）
add_executable(unit-features tests/unit_features.cpp)
target_link_libraries(unit-features PRIVATE carlife_proto)
add_test(NAME unit-features COMMAND unit-features)

if(TARGET carlife_input)
  add_executable(carlife-input-api-tests tests/input_api_tests.cpp)
  target_link_libraries(carlife-input-api-tests PRIVATE carlife_input)
  if(TARGET synthetic_wireless)
    target_link_libraries(carlife-input-api-tests PRIVATE synthetic_wireless)
  endif()
  add_test(NAME carlife-input-api-tests COMMAND carlife-input-api-tests)
  set_tests_properties(carlife-input-api-tests PROPERTIES TIMEOUT 30)
  add_executable(feature-probe tests/feature_probe.cpp)
  target_link_libraries(feature-probe PRIVATE carlife_input)
  # session_core / synthetic_wireless 都是【根工程】里定义的目标：
  #   * 根工程构建时它们存在，直接链上；
  #   * 本目录被单独构建时它们不存在 —— 此时 carlife_input 已经把
  #     Core/MainMenu/src/session_core.cpp 与 Core/Convert/src/real_media_store.cpp
  #     直接编进来了（见上面的 _carlife_input_sources）。
  # 【坑】不能无条件写 target_link_libraries(... session_core)：目标不存在时 CMake
  # 会把它当成库名，变成链接参数 -lsession_core，报 “cannot find -lsession_core”。
  if(TARGET session_core)
    target_link_libraries(feature-probe PRIVATE session_core)
  endif()
  if(TARGET synthetic_wireless)
    target_link_libraries(feature-probe PRIVATE synthetic_wireless)
  endif()
  target_compile_features(feature-probe PRIVATE cxx_std_20)
endif()

if(CARLIFE_WITH_SDL)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  pkg_check_modules(AVCODEC REQUIRED libavcodec)
  pkg_check_modules(AVUTIL REQUIRED libavutil)
  pkg_check_modules(SWSCALE REQUIRED libswscale)

  # ---- 本机呈现侧：解码 + SDL 窗口 + SDL 音频 + 快照 ----
  add_library(carlife_sdl STATIC src/render.cpp src/sdl_host_sink.cpp)
  target_include_directories(carlife_sdl PUBLIC include ${SDL2_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS}
                                                    ${AVUTIL_INCLUDE_DIRS} ${SWSCALE_INCLUDE_DIRS})
  target_compile_options(carlife_sdl PUBLIC -Wall -Wextra ${SDL2_CFLAGS_OTHER})
  target_link_libraries(carlife_sdl PUBLIC carlife_proto
    ${AVCODEC_LIBRARIES} ${SWSCALE_LIBRARIES} ${AVUTIL_LIBRARIES} ${SDL2_LIBRARIES})
  target_link_directories(carlife_sdl PUBLIC ${AVCODEC_LIBRARY_DIRS} ${SWSCALE_LIBRARY_DIRS}
                                              ${AVUTIL_LIBRARY_DIRS} ${SDL2_LIBRARY_DIRS})

  add_executable(carlife-hu src/main.cpp)
  target_link_libraries(carlife-hu PRIVATE carlife_sdl)
endif()
