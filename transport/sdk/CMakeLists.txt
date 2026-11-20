cmake_minimum_required(VERSION 3.16)
project(cldt_client_sdk VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
option(CLDT_SDK_ENABLE_TLS "Enable certificate-verified TLS using OpenSSL" ON)
if(CLDT_SDK_ENABLE_TLS)
    find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS SSL Crypto)
endif()

set(CLDT_SDK_IO_BUFFER_SIZE "262144" CACHE STRING
    "Upload/download working buffer size in bytes")
if(NOT CLDT_SDK_IO_BUFFER_SIZE MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "CLDT_SDK_IO_BUFFER_SIZE must be a positive integer")
endif()

option(CLDT_SDK_BUILD_SHARED "Build a shared client SDK instead of a static archive" OFF)
if(CLDT_SDK_BUILD_SHARED)
    set(CLDT_SDK_LIBRARY_TYPE SHARED)
else()
    set(CLDT_SDK_LIBRARY_TYPE STATIC)
endif()

add_library(cldt_client_sdk ${CLDT_SDK_LIBRARY_TYPE}
    ../src/ProtocolWire.cc
    src/cldt_client_sdk.cc
    src/SdkSocket.cc
    src/SdkTls.cc
    src/SdkOptions.cc
    src/SdkWire.cc
    src/SdkFile.cc
    src/SdkTransfer.cc
    src/SdkWorker.cc)
add_library(cldt::client_sdk ALIAS cldt_client_sdk)

target_include_directories(cldt_client_sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(cldt_client_sdk PRIVATE Threads::Threads)
if(CLDT_SDK_ENABLE_TLS)
    target_compile_definitions(cldt_client_sdk PRIVATE CLDT_SDK_HAS_TLS=1)
    target_link_libraries(cldt_client_sdk PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
set_target_properties(cldt_client_sdk PROPERTIES
    EXPORT_NAME client_sdk
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
if(CLDT_SDK_BUILD_SHARED)
    target_compile_definitions(cldt_client_sdk PUBLIC CLDT_SDK_SHARED=1
        PRIVATE CLDT_SDK_BUILD=1)
    # Android's native loader expects an unversioned .so name.
    if(NOT ANDROID)
        set_target_properties(cldt_client_sdk PROPERTIES
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR})
    endif()
    if(UNIX AND NOT APPLE)
        set(CLDT_SDK_EXPORT_MAP "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cldt_client_sdk.map")
        target_link_options(cldt_client_sdk PRIVATE "LINKER:--version-script=${CLDT_SDK_EXPORT_MAP}")
        set_property(TARGET cldt_client_sdk APPEND PROPERTY LINK_DEPENDS "${CLDT_SDK_EXPORT_MAP}")
    endif()
endif()
target_compile_definitions(cldt_client_sdk PRIVATE
    CLDT_SDK_IO_BUFFER_SIZE=${CLDT_SDK_IO_BUFFER_SIZE})
option(CLDT_SDK_ENABLE_SENDFILE "Use Linux/Android native file upload fast path" ON)
if(NOT CLDT_SDK_ENABLE_SENDFILE)
    target_compile_definitions(cldt_client_sdk PRIVATE CLDT_SDK_DISABLE_SENDFILE=1)
endif()
if(WIN32)
    target_link_libraries(cldt_client_sdk PRIVATE ws2_32)
endif()

set(CLDT_SDK_INSTALL_DEFAULT OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CLDT_SDK_INSTALL_DEFAULT ON)
endif()
option(CLDT_SDK_INSTALL "Enable binary SDK installation and CMake package export"
    ${CLDT_SDK_INSTALL_DEFAULT})
if(CLDT_SDK_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    # Keep the exported include path in sync with custom install layouts.
    target_include_directories(cldt_client_sdk INTERFACE
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    install(TARGETS cldt_client_sdk EXPORT cldtClientSdkTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/cldt_client_sdk.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(FILES tests/binary_consumer/CMakeLists.txt
        tests/binary_consumer/main.c tests/binary_consumer/README.md
        DESTINATION examples/binary_consumer)
    set(CLDT_SDK_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/cldt_client_sdk")
    configure_package_config_file(cmake/cldt_client_sdkConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/cldt_client_sdkConfig.cmake"
        INSTALL_DESTINATION "${CLDT_SDK_CMAKE_DIR}")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/cldt_client_sdkConfigVersion.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(EXPORT cldtClientSdkTargets FILE cldtClientSdkTargets.cmake
        NAMESPACE cldt:: DESTINATION "${CLDT_SDK_CMAKE_DIR}")
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/cldt_client_sdkConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/cldt_client_sdkConfigVersion.cmake"
        DESTINATION "${CLDT_SDK_CMAKE_DIR}")

    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        string(TOLOWER "${CMAKE_SYSTEM_NAME}" CLDT_SDK_PACKAGE_SYSTEM)
        if(ANDROID)
            set(CLDT_SDK_PACKAGE_ARCH "${ANDROID_ABI}-api${ANDROID_PLATFORM_LEVEL}")
        else()
            set(CLDT_SDK_PACKAGE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        string(TOLOWER "${CLDT_SDK_LIBRARY_TYPE}" CLDT_SDK_PACKAGE_LINKAGE)
        set(CPACK_PACKAGE_NAME cldt_client_sdk)
        set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
        set(CPACK_PACKAGE_FILE_NAME
            "cldt-client-sdk-${PROJECT_VERSION}-${CLDT_SDK_PACKAGE_SYSTEM}-${CLDT_SDK_PACKAGE_ARCH}-${CLDT_SDK_PACKAGE_LINKAGE}")
        if(CLDT_SDK_ENABLE_TLS)
            string(APPEND CPACK_PACKAGE_FILE_NAME "-tls")
        else()
            string(APPEND CPACK_PACKAGE_FILE_NAME "-no-tls")
        endif()
        set(CPACK_GENERATOR ZIP)
        if(NOT WIN32)
            set(CPACK_GENERATOR TGZ)
        endif()
        include(CPack)
    endif()
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

option(CLDT_SDK_BUILD_TESTS "Run adversarial C ABI/TLS tests (requires shared SDK, Python, openssl CLI)" OFF)
if(CLDT_SDK_BUILD_TESTS)
    if(NOT CLDT_SDK_BUILD_SHARED OR NOT CLDT_SDK_ENABLE_TLS)
        message(FATAL_ERROR "Security tests require CLDT_SDK_BUILD_SHARED=ON and CLDT_SDK_ENABLE_TLS=ON")
    endif()
    include(CTest)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME sdk_security COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/security_regression.py
        --library $<TARGET_FILE:cldt_client_sdk>)
endif()
