# muduo-core

基于 muduo 的 Linux C++ 网络核心，面向后续传输库 / 下载库底座。

- Reactor：`epoll` + One Loop Per Thread
- 堆分配：接入 [xmdMemoryPool 2.0.0](https://github.com/xmd-hs/xmdMemoryPool/releases/tag/v2.0.0)
- 传输向优化：发送背压、`writeComplete` 合并、大缓冲推流、accept 快排空

当前产出静态库 `libmuduo_core.a`（内存池静态包使用 local-exec TLS，不能链进 `.so`）。

## 依赖

| 项 | 要求 |
|---|---|
| 系统 | Linux x86_64（推荐 WSL2 / 原生 Linux） |
| 编译器 | GCC，C++14 |
| 构建 | CMake ≥ 3.5 |
| 第三方 | `third_party/xmdMemoryPool-2.0.0-Linux-x86_64-GNU-static` |

内存池预编译包来自上游 Release；若目录缺失，从 [v2.0.0](https://github.com/xmd-hs/xmdMemoryPool/releases/tag/v2.0.0) 下载 Linux GNU static 包并解压到 `third_party/`。

## 构建

> 若工程目录名含 `]`（本仓库默认如此），CMake 会把 `]` 当成列表语法，请在无 `]` 的路径下构建，例如先拷到 `~/chuanshuku_src` 再 cmake。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

产物：

- `lib/libmuduo_core.a`
- `lib/libtransport.a`（云盘上传/下载传输库）
- `example/testserver`
- `example/bench_alloc`
- `example/bench_download`
- `example/cloud_disk_server`
- `example/cloud_disk_client`
- `example/p2p_seed` / `example/p2p_leech`（P2P DHT）

## 目录

```text
muduo-core/
├── include/          # 网络库头文件（含 TcpClient / Connector）
├── src/              # 网络库实现
├── transport/        # 云盘上传/下载传输库
├── example/          # 回显、压测、云盘示例
├── third_party/      # xmdMemoryPool 2.0.0 静态 SDK
└── lib/              # 构建输出
```

核心类型：`EventLoop` / `Channel` / `EPollPoller` / `TcpServer` / `TcpClient` / `TcpConnection` / `Buffer` / `EventLoopThreadPool`。

传输库类型：`transport::FileTransferServer` / `FileTransferClient` / `TransferSession` / `Codec`（协议魔数 `CLDT`，version=2）。

本轮增强：

- 网络库：`TimerQueue`（`runAfter` / `runEvery`）、`TcpConnection::setContext`、`Connector` 指数退避重试
- 传输库：控制帧 CRC32、可选 token 鉴权、路径规范化 + root 防穿越、空闲超时、连接复用、`Stat` + 多分片并行下载
- 单测：`example/test_codec`（半包 / 坏魔数 / 坏校验和 / 路径）

## 内存池怎么用

业务侧堆对象与容器走 `include/XmdMemory.h`：

- `makePoolUnique` / `makePoolShared`
- `PoolVector` / `PoolString` / `PoolUnorderedMap`
- `Buffer`、连接对象、跨线程 `send` 拷贝等热点路径已接入

池内部元数据仍使用系统 `operator new`，因此**不能**全局替换 `new/delete`，否则会递归。

## 性能验证

当前依赖已升级为 xmdMemoryPool 2.0.0。下述吞吐与 CPU 对比来自升级前的 KamaMemoryPool 1.0.0，不作为 2.0.0 的性能结果；升级验证见 PERFORMANCE.md。


最新续优化将 sendfile 描述符持有对象和引用控制块从两次系统堆分配合并为一次 xmdMemoryPool 池分配，补充接收回调回归测试；六项 CTest 与传输集成测试通过。这次未证明整体吞吐继续提升，完整五轮结果及撤回的调参实验见性能报告。

保留 xmdMemoryPool 内存池的应用适配优化、最新五轮传输前后对比，以及历史分配器 A/B 诊断详见 [PERFORMANCE.md](PERFORMANCE.md)。本轮调整池缓冲区的初始化与大小类别、复用回调批次容量，并保留 TCP 自动调优；不要把分配微基准加速比等同于文件传输加速比。

传输热路径已减少以下开销：空闲检测使用单调时钟记录活动时间，仅在定时器到期后重新安排检查；控制帧 CRC 校验直接读取接收缓冲区，避免 extra 字段的临时字符串；CRC 表通过 C++ 静态初始化保证多线程首次调用安全；示例客户端的中间进度每 100ms 最多刷新一次，保留最终进度。

基准工具使用独立临时目录，自动启动/清理服务器，支持并发上传和下载，并对每个结果验证 SHA-256：

```bash
python3 scripts/bench_transfer.py --size-mib 64 --clients 1
python3 scripts/bench_transfer.py --size-mib 64 --clients 4
# 可用 --bin-dir /path/to/example 比较不同构建；默认端口 19390，可用 --port 修改。
# 在独立 Linux 目录中重新编译、跑 CTest 和上述两组基准（构建目录保留供检查）：
bash scripts/validate-local.sh
```

2026-11-09，同一 WSL Linux 接口地址、Release、内存池开启，五轮交替测试中位数如下。单连接 512 MiB，4 连接各 128 MiB，使用 `/dev/shm`，全部 SHA-256 通过。

| 场景 | 修改前 MiB/s | 修改后 MiB/s |
|---|---:|---:|
| 单连接上传 | 1970.12 | 2285.95 |
| 单连接下载 | 1159.73 | 1423.02 |
| 4 连接上传 | 2569.20 | 4014.55 |
| 4 连接下载 | 4978.13 | 5737.53 |

上传使用整段 sendfile，每次系统调用最多 1 MiB；上传 CPU 中位时间分别降低约 47% 和 68%。同时修复分段下载截断和持续 sendfile 活动的空闲超时判断。

此前 `127.0.0.1` 的低吞吐与连接地址有关，不能当作环境上限或代码优化前后对比。可在 Linux 内用以下命令验证直接地址；工具默认仍为 localhost：

```bash
linux_ip=$(hostname -I | awk '{print $1}')
python3 scripts/bench_transfer.py --host "$linux_ip" --size-mib 512 --clients 1 --work-dir /dev/shm
python3 scripts/test_transfer.py --host "$linux_ip"
```

计时包括客户端启动、建连、传输与 fsync，排除生成数据和校验。短样本波动较大，结果代表本机 tmpfs 工作负载，不代表磁盘或公网吞吐。原始数据与限制见性能报告。
CTest 包含协议编解码、CRC 标准向量、路径 NUL 拒绝，以及真实 socket 上“活动延长空闲期限、超时关闭写端”的回归测试。

## 传输向改动摘要

- `writeComplete` 合并调度，避免推流时回调风暴
- `Buffer::writeFd` 写到 `EAGAIN`；扩容按倍增
- 连接保留 Linux TCP 缓冲区自动调优，不再强制锁定 4MiB
- `accept` 先排空 listen 队列再装配连接；`listen` backlog = 1024
- `TcpConnection` 提供 `outputBufferBytes()` / `isWriting()` 便于背压

## 示例

回显服务：

```bash
./example/testserver
# 默认监听 8080
```

分配微基准：

```bash
./example/bench_alloc
```

单向下载推流压测（可传端口、IO 线程数）：

```bash
./example/bench_download [port] [ioThreads]
```

在本机 WSL2 上（4 个 IO 线程）近期结果：1～64 连接场景全部传完；多连接合计吞吐大约 50 MiB/s，这是历史 localhost 路径结果，不能视为环境回环上限。WSL 瞬时并发 `connect` 可能直接 `ECONNREFUSED`，压测客户端已做短重试。

云盘传输（上传/下载/续传/鉴权/并行下载）：

```bash
./example/test_codec
./example/cloud_disk_server 9000 ./cloud_root 2 mytoken
# 另开终端
./example/cloud_disk_client upload    127.0.0.1 9000 ./local.bin docs/a.bin 0 mytoken
./example/cloud_disk_client download  127.0.0.1 9000 docs/a.bin ./out.bin 0 0 mytoken
./example/cloud_disk_client pdownload 127.0.0.1 9000 docs/a.bin ./out2.bin 4 mytoken
```

产物另含 `example/test_codec`。

### P2P DHT（Kademlia + TCP piece）

私有 Kademlia DHT（UDP：`ping` / `find_node` / `get_peers` / `announce_peer`）+ TCP piece 协议（魔数 `CLPP`）。**不兼容**公网 BitTorrent DHT。云盘 C/S 路径保持不变。

每节点监听 `udpPort`（发现）与 `tcpPort`（分片）。节点会自动探测宣告用 IPv4（可用环境变量 `P2P_ADVERTISE_IP` 覆盖）。部分 WSL 网络模式下 `127.0.0.1` UDP 不可用，示例默认走探测到的局域网地址。

```bash
# 终端1：种子（生成 .meta 并做种）
./example/p2p_seed ./data.bin ./data.meta 17001 18001
# 终端2：下载（bootstrap = seed 的 UDP 端口）
./example/p2p_leech ./data.meta ./out.bin 17002 18002 17001
cmp ./data.bin ./out.bin

# 同机 1seed+2leech 及“停 seed 后纯 P2P”验收（在构建目录旁）
python3 example/_run_demo.py
```

目录：`transport/p2p/`（`KademliaNode` / `TorrentMeta` / `PieceStore` / `PeerSession` / `TorrentSession`）。

## 参考

- 陈硕 muduo：https://github.com/chenshuo/muduo/
- xmdMemoryPool：https://github.com/xmd-hs/xmdMemoryPool
- 《Linux 多线程服务器编程》
- 《Linux 高性能服务器编程》
