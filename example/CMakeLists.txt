# 仅依赖网络库的示例
set(CORE_EXAMPLE_TARGETS
    testserver
    bench_alloc
    bench_allocator
    bench_buffer
    bench_eventloop
    test_buffer
    test_pool
    test_receive
    test_sendfile
    bench_download
)

foreach(target ${CORE_EXAMPLE_TARGETS})
    add_executable(${target} ${target}.cc)
    target_link_libraries(${target} muduo_core ${LIBS})
    target_compile_options(${target} PRIVATE -Wall)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endforeach()

# 云盘传输库 + P2P 示例
set(TRANSPORT_EXAMPLE_TARGETS
    cloud_disk_server
    cloud_disk_client
    test_codec
    test_idle
    p2p_seed
    p2p_leech
)

foreach(target ${TRANSPORT_EXAMPLE_TARGETS})
    add_executable(${target} ${target}.cc)
    target_link_libraries(${target} transport muduo_core ${LIBS})
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/transport/include
        ${CMAKE_SOURCE_DIR}/transport/p2p/include
    )
    target_compile_options(${target} PRIVATE -Wall)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endforeach()

if(BUILD_TESTING)
    add_test(NAME codec COMMAND test_codec)
    add_test(NAME idle_timeout COMMAND test_idle)
    add_test(NAME sendfile COMMAND test_sendfile)
    add_test(NAME receive COMMAND test_receive)
    add_test(NAME buffer COMMAND test_buffer)
    add_test(NAME pool COMMAND test_pool)
    set_tests_properties(codec idle_timeout sendfile receive buffer pool PROPERTIES TIMEOUT 10)
endif()
