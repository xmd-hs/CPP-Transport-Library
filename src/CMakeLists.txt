# 显式列出源文件（工程目录名含 ']'，GLOB 字符类会误解析）
set(SRC_FILES
    Acceptor.cc
    Buffer.cc
    Channel.cc
    Connector.cc
    CurrentThread.cc
    DefaultPoller.cc
    EPollPoller.cc
    EventLoop.cc
    EventLoopThread.cc
    EventLoopThreadPool.cc
    InetAddress.cc
    Logger.cc
    Poller.cc
    Socket.cc
    TcpClient.cc
    TcpConnection.cc
    TcpServer.cc
    Thread.cc
    Timer.cc
    TimerQueue.cc
    Timestamp.cc
    UdpSocket.cc
)

# 静态库。v2.0.0 的静态内存池使用 local-exec TLS，不能再链进 .so，
# 要和本库一起进最终可执行文件。
add_library(muduo_core STATIC ${SRC_FILES})
target_compile_definitions(muduo_core PUBLIC
    MUDUO_USE_MEMORY_POOL=$<BOOL:${MUDUO_USE_MEMORY_POOL}>)

#设置头文件的路径
target_include_directories(muduo_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(muduo_core PUBLIC xmdMemoryPool::memory_pool)
