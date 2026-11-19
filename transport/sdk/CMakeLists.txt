cmake_minimum_required(VERSION 3.16)
project(cldt_client_sdk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(CLDT_SDK_IO_BUFFER_SIZE "262144" CACHE STRING
    "Upload/download working buffer size in bytes")
if(NOT CLDT_SDK_IO_BUFFER_SIZE MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "CLDT_SDK_IO_BUFFER_SIZE must be a positive integer")
endif()

if(NOT TARGET transport_wire)
    add_library(transport_wire STATIC ../src/ProtocolWire.cc)
    target_include_directories(transport_wire
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    if(MSVC)
        target_compile_options(transport_wire PRIVATE /utf-8)
    endif()
endif()

add_library(cldt_client_sdk STATIC
    src/cldt_client_sdk.cc
    src/SdkSocket.cc
    src/SdkWire.cc
    src/SdkFile.cc
    src/SdkTransfer.cc
    src/SdkWorker.cc)
add_library(cldt::client_sdk ALIAS cldt_client_sdk)

target_include_directories(cldt_client_sdk
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(cldt_client_sdk PUBLIC transport_wire Threads::Threads)
target_compile_definitions(cldt_client_sdk PRIVATE
    CLDT_SDK_IO_BUFFER_SIZE=${CLDT_SDK_IO_BUFFER_SIZE})
option(CLDT_SDK_ENABLE_SENDFILE "Use Linux/Android native file upload fast path" ON)
if(NOT CLDT_SDK_ENABLE_SENDFILE)
    target_compile_definitions(cldt_client_sdk PRIVATE CLDT_SDK_DISABLE_SENDFILE=1)
endif()
if(WIN32)
    target_link_libraries(cldt_client_sdk PUBLIC ws2_32)
endif()
if(MSVC)
    target_compile_options(cldt_client_sdk PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(cldt_client_sdk PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(CLDT_SDK_BUILD_EXAMPLE "Build the SDK command-line integration sample" OFF)
if(CLDT_SDK_BUILD_EXAMPLE)
    add_executable(cldt_sdk_sample sample/cldt_sdk_sample.cc)
    target_link_libraries(cldt_sdk_sample PRIVATE cldt::client_sdk)

    add_executable(cldt_sdk_memory_io_sample sample/cldt_sdk_memory_io_sample.cc)
    target_link_libraries(cldt_sdk_memory_io_sample PRIVATE cldt::client_sdk)
endif()
