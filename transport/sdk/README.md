# CLDT 客户端 SDK 0.2.0

异步 C API，可嵌入现有客户端。独立 SDK 不依赖 Muduo/epoll/内存池；Linux 服务端是单独组件，不包含在 SDK 内。

## 构建与二进制集成

默认静态库，启用 OpenSSL TLS，要求目标平台 OpenSSL >=1.1.1 开发库；部署应选择仍受维护的版本。

```sh
cmake -S transport/sdk -B build/sdk -DCMAKE_BUILD_TYPE=Release -DCLDT_SDK_BUILD_EXAMPLE=ON
cmake --build build/sdk --config Release
cmake --install build/sdk --config Release --prefix /path/to/sdk
cpack --config build/sdk/CPackConfig.cmake -C Release -B build/packages
```

动态库加 `-DCLDT_SDK_BUILD_SHARED=ON`。包提供公开头文件、库、CMake 配置及文档，不含实现源码或独立协议库。使用方设置 `CMAKE_PREFIX_PATH` 指向解压目录：

```cmake
project(my_client LANGUAGES C CXX)
find_package(cldt_client_sdk 0.2 CONFIG REQUIRED)
add_executable(my_client main.cpp)
target_link_libraries(my_client PRIVATE cldt::client_sdk)
```

**推荐交付静态二进制包**：Windows 为 `.lib`，Linux/Android/iOS 为 `.a`。使用方只编译自己的代码，链接时把 SDK 机器码合入自己的可执行文件或 JNI 动态库，不需要随应用携带单独的 SDK DLL/so。静态 SDK 不代表整个程序完全静态，OpenSSL、C++ 运行库仍按使用方的链接策略部署。

如果其他项目通过 Git 子模块或克隆引用此仓库，使用专门的二进制入口：

```cmake
# SDK 包由维护者提前构建；此路径内有 include/ 和 lib/。
set(CLDT_SDK_BINARY_ROOT "/absolute/path/to/extracted-static-sdk" CACHE PATH "")
add_subdirectory(third_party/CPP-Transport-Library/transport/sdk/prebuilt cldt-prebuilt)
target_link_libraries(my_client PRIVATE cldt::client_sdk)
```

`prebuilt` 仅导入预编译库，缺少包会报错，绝不自动回退到编译 SDK 源码。直接 `add_subdirectory(transport/sdk)` 属于维护者从源码构建的方式，使用方二进制集成应选择 `find_package` 或 `prebuilt`。无需构建仓库根目录或 Linux 服务端。

静态目标传递 C++、线程、OpenSSL 和 Windows ws2_32 链接依赖。动态库需部署 OpenSSL 运行库，依赖不自动打入 SDK 安装包；应用负责许可证、版本与安全更新。Windows 手工链接 DLL 需定义 `CLDT_SDK_SHARED`，不能定义 `CLDT_SDK_BUILD`。

每个平台/架构需分别构建。Windows 匹配 MSVC 运行库；Android 使用 NDK toolchain，提供该 ABI 的 OpenSSL 安装前缀，统一 libc++ 策略；iOS 需要 macOS/Xcode 和对应 OpenSSL。`tests/binary_consumer` 只依赖安装包和公开 C 头文件。旧 0.1.0 包不包含本次安全功能，须重新生成 0.2.0。

可显式 `-DCLDT_SDK_ENABLE_TLS=OFF` 构建无 OpenSSL 依赖版本，但启用 TLS 返回 NOT_SUPPORTED，绝不静默退回明文。

## 安全传输与部署

为兼容旧接口，create 后默认仍是明文；生产应用必须在提交操作前启用 TLS，并检查配置返回值：

```c
cldt_client_options options;
cldt_client_options_init(&options);
options.tls_enabled = 1;
options.ca_file = "/app/certs/ca-bundle.pem";
options.server_name = "files.example.com";
int status = cldt_client_set_options(client, &options);
if (status != CLDT_STATUS_OK) { /* 停止提交，不回退明文 */ }
```

最低 TLS1.2，支持 TLS1.3；强制验证证书链、有效期和 DNS/IP 身份。没有跳过验证或降级开关。server_name=NULL 时验证 create 的 host；ca_file=NULL 使用 OpenSSL 默认信任路径，**不等于 Windows/Android 原生证书库**，移动应用应显式提供 CA bundle。暂不实现证书吊销查询、证书绑定或双向 TLS。

服务端拓扑：`SDK --TLS--> stunnel:9443 --本机明文--> CLDT:9000`。模板见 `deploy/stunnel-server.conf`。使用有效证书链、受限权限私钥及高强度非空 token；stunnel 与后端应处于同一受信主机/网络命名空间。只对外暴露 TLS 入口。

示例服务端现在默认监听 127.0.0.1；可用 `CLDT_BIND_IP` 显式修改。旧脚本若需通过 Linux 网卡 IP 直连原生服务，必须设置该变量；TLS 代理部署保持回环监听，避免明文绕过。

命令行样例设置 `CLDT_TLS_CA=/path/ca.pem` 启用 TLS，可选 `CLDT_TLS_SERVER_NAME=files.example.com`。变量未设置时为明文；设置为空字符串时使用 OpenSSL 默认信任路径。

## 接口可靠性约定

- 每实例只接受一个操作；忙时提交及修改配置返回 BUSY。进度和完成回调运行于工作线程，UI 更新由应用切回主线程。
- 接受的请求产生一次完成回调；同步拒绝不产生回调。完成回调前实例恢复空闲，可从完成回调提交下一项。
- DNS/connect/TLS 握手共同超时默认10秒；网络无进展超时默认60秒。options 可设置正数毫秒值。连接/收发可取消，取消返回 CANCELLED，超时返回 TIMEOUT，证书/握手失败返回 TLS_ERROR。
- 系统 DNS 用有界后台解析隔离，最多8个未结束调用。取消/超时不等待系统解析，但系统调用可能仍在后台；动态 SDK 应保持加载至进程退出，不能在可能有未结束解析时卸载 DLL/so。
- destroy 等待工作线程和回调，不能在本实例回调内调用；销毁必须与其他 API 调用在应用层同步。阻塞文件回调、磁盘 I/O、fsync 必须返回后才能完成销毁，网络超时不能中断应用代码。
- host/token/本地路径为 UTF-8，token 最多256字节。无效远端路径和超出有符号64位范围的 offset/length 同步拒绝。Windows 使用宽字符文件 API。
- 下载 length=0 为 offset 到 EOF；完整下载截断目标，分段下载保留其他区间。响应的长度、偏移、文件大小和帧校验通过后才打开目标。
- 默认 SYNC：下载成功前 fsync / Windows _commit，失败返回 FILE_ERROR。可显式 BUFFERED 提高吞吐，但成功只表示写入调用完成。文件 fsync 不保证新建目录项的崩溃原子持久化。
- 下载直接写入目标；取消/断连/磁盘故障可能留下部分文件，无原子替换、自动重试或跨进程恢复。进度100%不代表成功，必须等完成状态。
- 远端错误统一为 `2000 + TransferErrorCode`，如2004不存在、2008鉴权失败；1000段为本地SDK状态。CRC用于帧检查，不能替代TLS认证加密。

Android URI/iOS文档句柄用 cldt_file_io 接入，应用负责授权、生命周期、按偏移短读写。SYNC下载必须在 set_file_io **之后**注册 cldt_client_set_file_sync，否则返回 NOT_SUPPORTED。更换 provider 清除旧 sync；sync 必须实际持久化，失败返回非零。内存样例显式使用 BUFFERED。

回调必须遵守 C ABI、不抛异常、不无限阻塞。SDK拒绝回调返回越界读写数量；请求构造和后台操作异常转换为 INTERNAL_ERROR。

## 验证与性能

```sh
cmake -S transport/sdk -B build/sdk-test -DCLDT_SDK_BUILD_SHARED=ON -DCLDT_SDK_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/sdk-test --parallel
ctest --test-dir build/sdk-test --output-on-failure
python3 transport/sdk/tests/security_regression.py --library /path/libcldt_client_sdk.so --server /path/cloud_disk_server
```

回归依赖 Python/openssl 命令，真实服务端集成另需 stunnel。WSL 镜像网络若回环不可达，可设置 `CLDT_TEST_HOST=<本机Linux网卡IP>`；测试只监听该地址，生成临时证书，不写入系统信任库。

本轮 Linux/WSL TLS和明文的真实往返、异常协议、自定义I/O、超时/取消及证书回归见 `benchmarks/sdk-security-regression.json`。Windows x64 和 Android arm64/API24 当前验证的是TLS-off构建；Windows OpenSSL下载受阻，尚未完成Windows TLS运行验证；iOS无Xcode，未验证。不能用本轮结果替代移动设备测试或独立安全审计。

明文Linux/Android原生上传保留sendfile；TLS走加密缓冲路径，默认每实例256KiB，可配置 CLDT_SDK_IO_BUFFER_SIZE，CLDT_SDK_ENABLE_SENDFILE=OFF用于对照。

`tests/security_benchmark.py`用相同SDK、相同SYNC完成语义交错测明文/TLS，CPU包含客户端、后端及stunnel。结果和条件见 `benchmarks/sdk-security-performance.md`；本机内存盘不代表实际磁盘/公网/手机性能。历史 sdk-split-performance.md 的旧SDK下载不等待fsync，不能与本轮直接比较。
