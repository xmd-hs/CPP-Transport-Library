# SDK 验证与性能观察（2026-11-19）

环境：WSL2 Linux 6.18.33.2，GCC 15.2，Release，服务地址 192.168.80.211；4 个客户端进程。测量包含进程启动，SHA-256 在计时区间外验证。以下均是本机合成测量，不能代表公网或手机性能。

## 验证

- 项目整体构建与独立 SDK 构建通过；7/7 CTest 通过。
- Linux 原客户端上传、续传、下载、首段保留尾部、并行下载 x3、空文件、拒绝鉴权通过。第一次 loopback 测试在续传超时，后续完整运行通过；未证明根因或声称修复该偶发超时。
- SDK 文件上传/stat/下载 SHA-256 通过；自定义内存 I/O 的 3145865 字节往返逐字节一致。
- Windows x64 MSVC 19.44 Release 构建通过，连接 Linux 服务端执行文件、内存回调和中文路径往返通过。修复了示例 argv 非 UTF-8 导致中文路径打不开的问题。
- Android arm64 NDK 28.2 / API 24 构建 SDK 和两个示例通过；adb 无设备，未运行。iOS 无 Xcode，未验证。
- 补充 Apple socket SO_NOSIGPIPE；MSVC 改用安全 CRT 文件打开接口及 UTF-8 编译选项。

## 4 路传输观察

服务端磁盘池 4 worker，每进程 128 MiB，tmpfs，native/sdk 交错各五轮：

| 客户端 | 上传中位数 MiB/s | 下载中位数 MiB/s | 上传合计 CPU 秒中位数 | 下载合计 CPU 秒中位数 |
|---|---:|---:|---:|---:|
| native | 5719.60 | 4967.04 | 0.3451 | 0.8090 |
| SDK | 4406.01 | 9773.58 | 0.7170 | 0.2151 |

所有文件 SHA-256 通过。native 上传包含 sendfile 零拷贝，SDK 上传为 read/send；SDK 下载没有 native 的 fsync 完成语义，不能用上表得出 SDK 下载加速比。启动/调度/热身影响明显：native 上传首轮 2222.20 MiB/s，其余四轮 5630.07～5963.77。SDK 上传五轮 3671.86～4596.23，下载 8718.29～10360.22。适合定位候选优化，不是生产 SLA。

ext4 工作目录每进程 128 MiB，服务端 2/4 worker 交错各五轮：上传中位数 923.87/1053.61 MiB/s，下载 1116.19/1129.22 MiB/s，全部 SHA-256 通过；轮次很短、系统缓存和热身影响明显，保留默认 2 worker。

SDK 缓冲 256 KiB/1 MiB/4 MiB 交错各三轮，tmpfs，每进程 128 MiB，4 客户端：

| 缓冲 | 上传中位数 MiB/s | 下载中位数 MiB/s |
|---|---:|---:|
| 256 KiB | 4476.74 | 8672.72 |
| 1 MiB | 4168.76 | 9215.41 |
| 4 MiB | 3667.44 | 6590.28 |

256 KiB 首轮存在冷启动影响；64 KiB 配置未构建，相关尝试被脚本拒绝，没有有效数据。不能据此提高默认块大小。保留 256 KiB，并提供 CLDT_SDK_IO_BUFFER_SIZE 构建参数供目标端测量。

## GitHub 调研与下一步

实际查询并阅读：

- [libuv Unix 文件 I/O](https://github.com/libuv/libuv/blob/v1.x/src/unix/fs.c)：sendfile 的平台分支、EINTR/EAGAIN 处理与模拟回退。SDK 可考虑原生文件专用零拷贝上传，并保留自定义 I/O 的 read/send 回退；Windows/Apple/Linux sendfile 接口不同，不能照搬 Linux 路径。
- [liburing send-zerocopy 示例](https://github.com/axboe/liburing/blob/master/examples/send-zerocopy.c)：注册文件与 send_zc。需要完成通知和缓冲生命周期管理，只适用于 Linux 服务端；不是 Windows/iOS 的通用后端，当前未引入依赖或宣称收益。
- [uring-sendfile](https://github.com/johalun/uring-sendfile)：作者明确为简单演示，可参考实现路径，不能把其结果视为本项目性能证据。

优先级：先补齐 SDK 持久化完成语义及可取消连接；再对原生文件零拷贝上传做同语义 A/B。真实 SSD、跨机网络、Android/iOS 真机上的吞吐、CPU、内存及取消尾延迟仍待测。io_uring 属于后续服务端独立实验，不作为当前跨平台 SDK 的基础依赖。
