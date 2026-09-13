# 验证记录 · 2026-09-13

范围：两个 Input 的代码/API 对接；用户明确不要求有线输出或真车验收。

## 最终结果

| 检查 | 结果 |
| --- | --- |
| WSL Debian CMake/CTest | 14/14 PASS，13.18秒 |
| 外部 CatPlay 评估树 Rust lib tests | 49/49 PASS，0 ignored |
| 索引解析器回归 | 2/2 PASS |
| Zero2W ARM64 原生编译/测试 | 3/3 PASS |
| 三层 CatPlay 补丁应用 | 无fuzz应用；5个修改文件与评审树逐字节一致；可检测已应用 |
| Reference 跟踪文件未暂存改动检查 | git diff --name-only -- Reference 为空；保留用户原有暂存搬移 |

## 本地 C++

```sh
cmake -S . -B Temp/input-audit-build -DBUILD_WIRELESS_CARLIFE_PLUS=ON -DCARLIFE_WITH_SDL=OFF -DCMAKE_BUILD_TYPE=Debug
timeout 120s cmake --build Temp/input-audit-build -j4
timeout 90s ctest --test-dir Temp/input-audit-build --output-on-failure --timeout 20
```

14项：core_tests、media_tests、display_config_tests、control_request_tests、
media_client_tests、media_ext_tests、carplay_input_api_tests、http_tests、
slow_http_tests、auth3_native_tests、unit-wire、unit-ap、unit-features、
carlife-input-api-tests。没有跳过失败测试；原有编译警告仍存在。

覆盖包括：9类HTTP控制解析及真实HTTP路由、非法参数/表单、会话归属/断线重置、
音频格式与流生命周期、封面/歌词片段边界、具体键码/数字串/旋钮映射、
8类应用事件、模块控制singular protobuf与有界队列、真实SDK摘要与失败路径、
提示音成功/失败结束准备但不启动麦克风。

## 外部 Rust 引擎

只在一次性独立副本应用三层补丁，不修改参考库、不部署产物。

```sh
CARGO_TARGET_DIR=/opt/zero2w-catplay-epoch-final-host \
  timeout 120s /root/.cargo/bin/cargo +1.88.0 test --offline \
  -p catplay_c2a --no-default-features --lib
```

运行目录：Temp/catplay-completion-20260913（WSL挂载路径）。
49项含原有测试和新增控制/HID字节、双点槽、Siri、DTMF、分包/粘包、
过期/重放扩展、元数据增量/UTF-8边界等。完整日志保留在WSL
/tmp/zero2w-input-rust-test.log。仅验证lib测试，不声称本轮已完成ARM Rust产物构建。

最新补丁验证副本：/tmp/zero2w-input-overlay-N2Vbl7PH。三层均无fuzz应用，
lib.rs/media_ipc.rs/preview_rx.rs/control_ext.rs/input_iap2.rs
与评审源文件一致；第三层反向dry-run可识别已应用状态。

## Zero2W ARM64

USB SSH 192.168.77.2，Debian12，GCC12.2。最终源码快照在
/tmp/zero2w-input-api-JAH1Dzoz；每次从源码原生编译，不复用旧可执行文件。

| 程序 | build_exit | test_exit | 结果 |
| --- | --- | --- | --- |
| carplay-api | 0 | 0 | CarPlay input API regressions passed |
| carplay-ipc | 0 | 0 | 73 checks |
| carlife-api | 0 | 0 | 180 passed, 0 failed |

[原始机器结果](board-results.json)。
[重跑脚本](run_board_tests.py) 从stdin获取密码，编译timeout180秒、运行timeout30秒。
脚本只写新建的隔离测试目录，不安装依赖、不修改服务/网络/USB或切换输入。
Windows GBK控制台曾使日志打印报UnicodeEncodeError；那两次不计通过。
现已设置UTF-8输出，以上是修正后完整成功运行。临时测试文件保留，未删除用户数据。

## 索引

```sh
python Input/API_AUDIT/test_inventory.py
python Input/API_AUDIT/generate_inventory.py
```

提取器修复了同一行多个case及多行fall-through遗漏；8类应用通知均索引到
onAppEvent。LIVI preload的70个成员均保留在对应状态表。
连续两次生成必须产生相同inventory.json；来源变化后先重新生成，再做确定性比较。
2687条索引是声明/消息/回调等混合证据，不是功能完成数。

## 不包含的验收

无手机投屏/电话/麦克风实测，无有线输出/USB角色切换，无MFi硬件鉴权、
OTA安装或激活服务验证。状态解码、命令排队、HTTP200和接口单测
不等于手机/车辆执行成功；未对齐项见INTERFACE_CONTRACT.md。
