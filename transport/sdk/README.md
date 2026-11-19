# CLDT 客户端 SDK

此目录提供可嵌入现有应用的 C API，连接项目中的 Linux CLDT v2 服务端，不包含 GUI。独立构建不依赖 Muduo、epoll 或内存池。

## 构建与集成

```sh
cmake -S transport/sdk -B build/sdk -DCLDT_SDK_BUILD_EXAMPLE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/sdk --config Release
```

已有 CMake 应用可使用 `add_subdirectory(<项目路径>/transport/sdk cldt-sdk)`，并链接 `cldt::client_sdk`。公开头文件为 `cldt_client_sdk.h`；链接目标会传递线程、协议库和 Windows 的 `ws2_32` 依赖。Android JNI 动态库集成时应为静态依赖启用 `CMAKE_POSITION_INDEPENDENT_CODE=ON`。

Android 使用 NDK 的 `build/cmake/android.toolchain.cmake`，设置 `ANDROID_ABI=arm64-v8a` 和目标 API，例如 `ANDROID_PLATFORM=android-24`。Windows 使用 Visual Studio 2022 x64。iOS 需在 macOS/Xcode 上设置 `CMAKE_SYSTEM_NAME=iOS`、`CMAKE_OSX_SYSROOT=iphoneos`、`CMAKE_OSX_ARCHITECTURES=arm64`，并在应用侧处理文档权限。

## 使用约定

1. `cldt_client_create` 创建实例，设置进度和完成回调。
2. 可选配置 `cldt_client_set_file_io`；随后提交 upload、download 或 stat。
3. 同一实例一次仅处理一个请求；并发传输使用多个实例。回调运行在 SDK 工作线程，UI 更新应切回应用主线程。
4. `cldt_client_cancel` 中断活动 socket；`cldt_client_destroy` 等待工作线程退出，不能在该实例的回调里调用。

路径、host、token 均为 UTF-8。Windows 示例使用宽字符入口转换命令行，支持中文本地路径。offset 和 length 单位为字节；下载 length=0 表示从 offset 到 EOF，完整下载才截断目标文件，分段下载保留其他区间。

Android content URI 和 iOS 文档句柄通过 `cldt_file_io` 接入，应用负责获取访问授权、保持句柄有效，并实现按 offset 的读写。SDK 不直接解释 URI。`sample/cldt_sdk_memory_io_sample.cc` 展示完整回调接口；其内存样例已在 Linux 和 Windows 与服务端互通验证，但不代表移动系统权限层已经验证。

## 验证状态（2026-11-19）

| 平台 | 当前证据 |
|---|---|
| Linux / WSL GCC 15.2 | Release 构建；SDK 文件及自定义 I/O 上传/stat/下载；SHA-256/逐字节校验通过 |
| Windows x64 / MSVC 19.44 | Release 构建；连接 Linux 服务端；文件/内存 I/O 和中文路径往返通过 |
| Android arm64 / NDK 28.2 | API 24 交叉编译 SDK 及两个示例通过；无连接设备，未做真机运行 |
| iOS | 已补 socket 的 SO_NOSIGPIPE 配置；无 Xcode 环境，未验证构建和运行 |

连接建立当前使用阻塞 DNS/connect，连接阶段取消不能保证立即返回。普通传输仍有 socket 超时。当前不提供 TLS；部署条件与鉴权策略需由宿主应用和服务端共同确定。

## 性能测量

`scripts/bench_transfer.py --client-kind sdk --sdk-sample <可执行文件>` 可测 SDK，默认 native 测原 Linux 客户端。构建参数 `CLDT_SDK_IO_BUFFER_SIZE` 可调整每实例传输缓冲，默认 262144 字节。当前 1 MiB/4 MiB 试验没有稳定全面收益，因此不提高默认值。

SDK 下载完成表示写调用结束/文件关闭；原 Linux 客户端还等待 fsync。两者吞吐数字不能直接作为实现优劣或加速比。真实性能必须在目标设备、实际存储和跨机网络下复测。

### 模块划分与上传快路径

实现拆分为 C API 入口 `cldt_client_sdk.cc`、socket `SdkSocket.cc`、协议帧 `SdkWire.cc`、文件访问 `SdkFile.cc`、传输流程 `SdkTransfer.cc` 和工作线程 `SdkWorker.cc`；内部声明集中在不对外安装的 `SdkInternal.h`。公开头文件和 C API 不变。

Linux/Android 的原生文件上传默认使用 sendfile，不分配传输暂存缓冲；每次最多发送 1 MiB，并处理短发送、EINTR、提前 EOF、取消与 SIGPIPE。文件系统不支持时从当前已发送偏移回退为 read/send，自定义文件 I/O、Windows 和 Apple 保留缓冲路径。可用 `-DCLDT_SDK_ENABLE_SENDFILE=OFF` 构建对照版本。原生文件在上传期间应保持内容不变。

可用 `scripts/compare_sdk.py --before <旧版示例> --after <新版示例> --bin-dir <服务端目录> --host <本机Linux地址> --output <json路径>` 做交错测量；默认各有一次不计入结果的预热。`--mode download` 使用相同本地种子单独测下载，避免上传实现影响随后下载的缓存状态。本轮收益及原始数据见 `benchmarks/sdk-split-performance.md`。
