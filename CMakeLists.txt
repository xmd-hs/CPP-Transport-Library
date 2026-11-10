#设置最低版本和项目名称
cmake_minimum_required(VERSION 3.5)
project(muduo-core) #锁定项目的工作目录

include(CTest)
option(MUDUO_USE_MEMORY_POOL "Use xmdMemoryPool allocators for core objects and containers" ON)

# xmdMemoryPool 2.0.0 的头文件要求 C++14
set (CMAKE_CXX_STANDARD 14)
set (CMAKE_CXX_STANDARD_REQUIRED True)

# v2.0.0 预编译包：头文件 + 静态库，替代本项目的堆分配
list(APPEND CMAKE_PREFIX_PATH
    "${CMAKE_SOURCE_DIR}/third_party/xmdMemoryPool-2.0.0-Linux-x86_64-GNU-static")
find_package(xmdMemoryPool 2.0.0 EXACT CONFIG REQUIRED)

#链接必要的库，比如刚刚我们写好的在src文件Cmakelists中muduo-core_lib静态库，还有全局链接库
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)
#示例静态库的输出
#set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/lib) # 设置静态库输出路径
#设置全局链接库
set(LIBS
    pthread
)

#添加子目录
add_subdirectory(src)
add_subdirectory(transport)
add_subdirectory(example)
