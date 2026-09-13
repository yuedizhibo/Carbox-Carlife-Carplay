# Output

对原车主机的输出适配层。`WiredCarPlay/` 已有独立 C++20 接口库、会话状态机、有界双向队列、功能目录和测试。

现已由 `Core/Forward` 消费这些接口，实现无线 CarPlay 媒体直通及原生双向转发契约；未连接 CarLife Convert，也不操作 USB。USB/iAP2/RTSP/RTP 等真实后端尚未实现，不能宣称已经能向车机输出。完整接口清单见 `WiredCarPlay/API.md`，直连状态见 `Core/Forward/README.md`。
