cmake_minimum_required(VERSION 3.20)
project(zero2w_car_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(session_core Core/MainMenu/src/session_core.cpp Core/MainMenu/src/desktop.cpp)
target_include_directories(session_core PUBLIC Core/MainMenu/include)

add_library(synthetic_wireless
  Input/WirelessCarPlay/src/wireless_carplay.cpp
  Input/WirelessCarPlay/src/catplay_media_client.cpp
  Core/Convert/src/real_media_store.cpp
  Core/Web/src/display_config.cpp)
target_include_directories(synthetic_wireless PUBLIC
  Input/WirelessCarPlay/include
  Core/Convert/include
  Core/Web/include)
target_link_libraries(synthetic_wireless PUBLIC session_core)

find_package(Threads REQUIRED)
target_link_libraries(synthetic_wireless PUBLIC Threads::Threads)

enable_testing()
add_executable(core_tests Core/MainMenu/tests/core_tests.cpp)
target_include_directories(core_tests PRIVATE Core/Web/include)
target_link_libraries(core_tests PRIVATE session_core)
add_test(NAME core_tests COMMAND core_tests)

add_executable(media_tests Core/Convert/tests/media_tests.cpp)
target_include_directories(media_tests PRIVATE Core/Convert/include)
target_link_libraries(media_tests PRIVATE synthetic_wireless)
add_test(NAME media_tests COMMAND media_tests)

add_executable(display_config_tests Core/Web/tests/display_config_tests.cpp)
target_include_directories(display_config_tests PRIVATE Core/Web/include)
target_link_libraries(display_config_tests PRIVATE synthetic_wireless)
add_test(NAME display_config_tests COMMAND display_config_tests)

add_executable(control_request_tests Core/Web/tests/control_request_tests.cpp)
target_link_libraries(control_request_tests PRIVATE synthetic_wireless)
add_test(NAME control_request_tests COMMAND control_request_tests)
set_tests_properties(control_request_tests PROPERTIES TIMEOUT 20)

if(UNIX)
  add_executable(media_client_tests Input/WirelessCarPlay/tests/media_client_tests.cpp)
  target_include_directories(media_client_tests PRIVATE
    Input/WirelessCarPlay/include
    Core/Convert/include)
  target_link_libraries(media_client_tests PRIVATE synthetic_wireless Threads::Threads)
  add_test(NAME media_client_tests COMMAND media_client_tests)

  add_executable(media_ext_tests Input/WirelessCarPlay/tests/media_ext_tests.cpp)
  target_link_libraries(media_ext_tests PRIVATE synthetic_wireless Threads::Threads)
  add_test(NAME media_ext_tests COMMAND media_ext_tests)
  set_tests_properties(media_client_tests media_ext_tests PROPERTIES TIMEOUT 30)
  add_executable(carplay_input_api_tests Input/WirelessCarPlay/tests/input_api_tests.cpp)
  target_link_libraries(carplay_input_api_tests PRIVATE synthetic_wireless Threads::Threads)
  add_test(NAME carplay_input_api_tests COMMAND carplay_input_api_tests)
  set_tests_properties(carplay_input_api_tests PROPERTIES TIMEOUT 30)
endif()

option(BUILD_MFI_AUTH3_NATIVE "Build the native MFi Auth 3.0 utility and its tests" ON)
if(BUILD_MFI_AUTH3_NATIVE)
  add_subdirectory(Input/WirelessCarPlay/MFI)
endif()

if(UNIX)
  add_executable(mvp_server Core/Web/src/main.cpp)
  target_link_libraries(mvp_server PRIVATE synthetic_wireless Threads::Threads)

  add_executable(http_tests Core/Web/tests/http_tests.cpp)
  target_link_libraries(http_tests PRIVATE synthetic_wireless Threads::Threads)
  add_test(NAME http_tests COMMAND http_tests $<TARGET_FILE:mvp_server>)

  add_executable(slow_http_tests Core/Web/tests/slow_http_tests.cpp)
  target_link_libraries(slow_http_tests PRIVATE synthetic_wireless Threads::Threads)
  add_test(NAME slow_http_tests COMMAND slow_http_tests $<TARGET_FILE:mvp_server>)
endif()

option(BUILD_WIRELESS_CARLIFE_PLUS "Build the Wireless CarLife+ input and link it into the web server" OFF)
if(BUILD_WIRELESS_CARLIFE_PLUS)
  add_subdirectory(Input/WirelessCarLifePlus)
  if(TARGET carlife_input AND TARGET mvp_server)
    target_link_libraries(mvp_server PRIVATE carlife_input)
    # 让装配层知道 CarLife 输入可用；不需要 SDL：适配器只做协议与媒体转发。
    target_compile_definitions(mvp_server PRIVATE ZERO2W_WITH_CARLIFE=1)
  endif()
endif()

# Standalone output contract only: no Core/Convert/Input linkage or USB activation.
option(BUILD_WIRED_CARPLAY_OUTPUT "Build the independent Wired CarPlay output interface" ON)
if(BUILD_WIRED_CARPLAY_OUTPUT)
  add_subdirectory(Output/WiredCarPlay)
endif()

option(BUILD_CARPLAY_FORWARD "Build Core wireless-to-wired CarPlay forwarding" ${BUILD_WIRED_CARPLAY_OUTPUT})
if(BUILD_CARPLAY_FORWARD)
  if(NOT BUILD_WIRED_CARPLAY_OUTPUT)
    message(FATAL_ERROR "BUILD_CARPLAY_FORWARD requires BUILD_WIRED_CARPLAY_OUTPUT")
  endif()
  add_subdirectory(Core/Forward)
endif()
