# WiredCarPlay 输出

面向原车主机的有线 CarPlay 输出接口。

当前已实现独立 C++20 **接口层**：状态机、异步请求/执行回执、能力检查、媒体与麦克风双向队列、车机控制及超时应答、结构化元数据、完整参考消息目录。本库仍不依赖 Core/Convert/Input；`Core/Forward` 已作为调用方接入，提供无线 CarPlay 直通协调。

本库不是 USB/CarPlay 协议引擎；没有实际切换 USB、鉴权、网络连接、编码/解码或向车机发送字节。`start()` 只入队，必须由后端完成实际启动并提交 `PeerInfo` 才能进入 Ready。测试中的成功回执来自测试夹具，不是生产后端。

## 文件

- `include/wired_carplay/output.hpp`：所有公共会话、控制、流、USB/鉴权提供者接口。
- `include/wired_carplay/metadata.hpp`：媒体、歌词、导航、通讯录、通话、车辆、位置等结构化数据契约。
- `include/wired_carplay/catalog.hpp`：13 个控制命令、59 个 iAP2 CSM 消息 ID、11 个仅声明扩展。
- `src/output.cpp`：线程安全状态机、参数检查、能力检查和有界队列。
- `API.md`：完整功能/API 清单、方向和后端实现边界。
- `TEST_RESULTS.md`：本轮实测记录。

## 单独构建

```sh
cmake -S Output/WiredCarPlay -B Temp/wired-output-build -DCMAKE_BUILD_TYPE=Debug
cmake --build Temp/wired-output-build -j 4
ctest --test-dir Temp/wired-output-build --output-on-failure
```

根构建的 `BUILD_WIRED_CARPLAY_OUTPUT=ON` 默认只增加独立库和测试，不将其链接到 Core/Web。单独构建不依赖 Reference、Rust、protobuf、OpenSSL 或现有 Input。

开发侧逐项参考核对（不属于产品构建依赖）：

```powershell
powershell -NoProfile -File Output/WiredCarPlay/tests/check_reference_catalog.ps1
```

参考依据为本地 `Reference/CatPlaySource` 的 transmitter API、commands、streams、modes、CSM 消息及 USB phone gadget 状态。只记录互操作名称、编号、方向和接口行为，不导入/链接参考实现。未来真实后端仍需要单独完成来源与许可证审查。
