# 二进制接入示例

此项目仅编译自己的 main.c，SDK 实现来自安装包中的预编译库。

静态包会将 SDK 的机器码链接进最终程序，不需要额外的 SDK DLL/so。
TLS 静态 SDK 仍需链接 OpenSSL；若 OpenSSL 为动态库，仍需部署其运行库。

```sh
cmake -S transport/sdk/tests/binary_consumer -B build/consumer -DCMAKE_PREFIX_PATH=/absolute/path/to/extracted-sdk
cmake --build build/consumer --config Release
ctest --test-dir build/consumer -C Release --output-on-failure
```

不要将 SDK 的 src/*.cc 加入自己的工程。
