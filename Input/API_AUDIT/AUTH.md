# CarLife 手机校验接口

本轮实现 `SdkPhoneVerifier::verify`，Session 默认 `authMode=sdk`。保留显式 `trust/dev/deny` 供模拟器测试；未知模式拒绝，提供方异常拒绝，日志不输出挑战响应。注入的 `VerifySeam` 仍优先。

## 对照证据

本地参考 `apollo-DuerOS/CarLife-Android-Vehicle-V2.0/carlife-sdk`：

- `ConnectionEstablishHandler.kt:189` 发出 SDK 版本和连接时间；同文件约 207–213 行构造 `brand + model + sdk + connectTime`，调用 `EncryptionUtils.getVerifyResult`。产品 Session 使用相同字段及顺序。
- 随附 `src/main/jniLibs/x86_64/libencryption.so` 的 `Java_com_baidu_encryption_EncryptionUtils_getVerifyResult` 从 `0x11680` 开始。调用路径为 `GetStringUTFChars` → 在 seed 前插入 `MD5_HEAD` → `MD5` → `toStr` → `strcmp`。
- x86_64 `.data` 的 `MD5_HEAD` 指向 `0x31f9b`，字符串为 `CARLIFE`；`MD5::HEX_NUMBERS` 在 `0x32c70` 为小写十六进制字母表。
- 同仓库 arm64-v8a 版本在 `0x10f40` 开始的同名函数具有相同调用顺序。
- 这是本地二进制静态互操作分析，不是执行原 JNI 库的对拍。Android 库依赖 Bionic/Android 运行库，未假称可以直接链接 Debian glibc。

Linux 实现用现有 OpenSSL EVP 计算 `MD5("CARLIFE" + modified-UTF8(seed))` 并比较全部 32 个字符。UTF-8 验证、补充平面字符到 JNI 代理对编码、输入上限、格式错误、错误摘要均有测试；OpenSSL 不提供 MD5 时拒绝，不回退放行。该摘要只是遗留协议互操作算法，不是新设计的安全身份认证方案。

## 不混淆的边界

- `getVerifyResult` 是车机校验手机的接口，不是 OEM 激活服务。
- 没有移植/绕过 `getVerifyCode` 中的 Android 应用签名检查。
- `BOX_ACTIVE/HU_ACTIVE` 的 token 发放不在公开 handler 中。产品没有生成 token；默认激活状态未知，只有外部显式配置已激活且提供 token 时才发配置响应。这并不验证 token 的有效性，也不证明商业授权。
- 单元测试和 ARM 板原生测试不代表当前手机版本互操作已验收；按用户本轮范围不要求接车测试。
