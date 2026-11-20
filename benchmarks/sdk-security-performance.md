# SDK 0.2.0 安全传输与可靠性验证

日期：2026-11-19。WSL Ubuntu / GCC 15.2 Release / OpenSSL 3.5.5 / stunnel 5.77。

## 相同持久化语义的明文与 TLS

同一共享 SDK，4 路并发、每路 256 MiB，工作目录 /dev/shm，使用本机 Linux 网卡地址（该 WSL 的 127.0.0.1 连接存在拒绝问题）。客户端下载都使用默认 SYNC；服务端上传成功等待 fsync。每种方式一次预热，3 次交错测量，取中位数；每次传输后 SHA-256 验证。

| 方式 | 上传 MiB/s | 下载 MiB/s | 上传 CPU 秒 / GiB | 下载 CPU 秒 / GiB |
|---|---:|---:|---:|---:|
| 明文 | 5937.92 | 9458.82 | 0.628 | 0.466 |
| TLS + stunnel | 2108.51 | 3377.87 | 4.693 | 2.242 |

CPU 包含 Python/C ABI 客户端、原生服务端及 TLS 代理。计时包含客户端创建、连接/握手、文件写入同步与销毁，不包含测试数据生成和事后 SHA-256。原始记录：`sdk-security-performance.json`；脚本：`transport/sdk/tests/security_benchmark.py`。

加密带来可见开销：TLS 上传不能使用原生 sendfile，代理也引入额外读写和加解密。以上是本机内存盘数据，不是公网/实际磁盘/手机性能保证，也不是加密优于明文的对比。预热与实测间波动较大，不能用这些短测数据推导稳定 P99 或长期容量。

另以原来的进程式 benchmark 对旧 0.1.0 静态样例和新 0.2.0 共享样例做交错上传回归，4×256 MiB、3轮，中位数为 3294.87 → 5296.26 MiB/s，CPU 1.4403 → 0.8032 秒；原始记录 `sdk-security-upload-comparison.json`。旧版实测在2690～3956 MiB/s间波动，新版在5181～5343 MiB/s间；链接方式和环境噪声也不同，因此此项仅作未出现明显上传退化的本地证据，不宣称稳定加速比例。没有将旧版不等待 fsync 的下载与新版 SYNC 下载直接比较。

## 正确性验证

- 33 项安全/接口检查通过：可信 CA 的 DNS/IP 校验，错误 CA/域名/过期证书拒绝，TLS1.1 拒绝，不降级且不向明文端发送 token。
- 读和握手超时/取消、忙状态拒绝、取消后复用、阻塞上传超时、TLS 阻塞上传取消、destroy 后仅一次取消回调通过。
- 三类请求远端错误码一致；畸形长度/偏移/大小/CRC/超大错误帧在打开文件前拒绝，已有文件保持原内容。
- 自定义 I/O 短写、同步失败、越界写入数量检查通过；真实服务端明文/TLS 上传、stat、下载、分段 SHA-256 及64MiB上传提前鉴权拒绝通过。
- 原有7项 CTest、SDK文件/内存样例互通、原生传输回归（恢复/分段/并发/空文件/鉴权）通过。
- Linux TLS 静态/动态安装包的独立 C 消费方配置、链接、运行通过；TLS-off 构建请求 TLS 明确返回 NOT_SUPPORTED。
- Windows x64/MSVC 与 Android arm64/API24 的 TLS-off 构建通过。Windows OpenSSL 下载失败，TLS-on 尚未构建/运行验证；Android TLS-on/真机和 iOS/Xcode 尚未验证。

安全回归原始记录：`sdk-security-regression.json`。测试证书仅临时生成，不安装进系统信任库。TLS 入口通过成熟 stunnel 终止，原生服务默认回环监听；SDK旧接口默认仍明文，应用必须显式启用 TLS 并检查返回值。

## 复现

```sh
export CLDT_TEST_HOST=<本机Linux网卡IP> # 普通Linux可省略，使用回环
python3 transport/sdk/tests/security_regression.py \
  --library /path/libcldt_client_sdk.so --server /path/cloud_disk_server \
  --output benchmarks/sdk-security-regression.json
python3 transport/sdk/tests/security_benchmark.py \
  --library /path/libcldt_client_sdk.so --server /path/cloud_disk_server \
  --output benchmarks/sdk-security-performance.json
```

这里的 SYNC 保障文件同步调用成功，不提供目标文件原子替换或新建目录项的崩溃一致性；取消/断连可留下部分目标。网络取消也不能打断无限阻塞的应用文件回调或系统磁盘同步。详见 SDK README。
