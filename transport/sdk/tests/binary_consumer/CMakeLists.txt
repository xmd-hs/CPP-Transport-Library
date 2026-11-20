cmake_minimum_required(VERSION 3.16)
project(cldt_binary_consumer LANGUAGES C CXX)

find_package(cldt_client_sdk 0.2 CONFIG REQUIRED)
add_executable(cldt_binary_consumer main.c)
target_link_libraries(cldt_binary_consumer PRIVATE cldt::client_sdk)
enable_testing()
add_test(NAME installed_sdk_lifecycle COMMAND cldt_binary_consumer)
