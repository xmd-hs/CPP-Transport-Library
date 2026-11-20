cmake_minimum_required(VERSION 3.16)

# Binary-only repository entry point. Never add the SDK implementation sources
# here or fall back to compiling them when a binary package is missing.
set(CLDT_SDK_BINARY_ROOT "" CACHE PATH "Root of an extracted CLDT binary SDK package")
if(NOT CLDT_SDK_BINARY_ROOT)
    message(FATAL_ERROR
        "Set CLDT_SDK_BINARY_ROOT to an extracted SDK package (include/ and lib/). "
        "Build/package the SDK separately; this entry point never compiles SDK sources.")
endif()

set(_cldt_new_imports)
foreach(_cldt_target cldt::client_sdk Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
    if(NOT TARGET ${_cldt_target})
        list(APPEND _cldt_new_imports ${_cldt_target})
    endif()
endforeach()

find_package(cldt_client_sdk 0.2 CONFIG REQUIRED
    PATHS "${CLDT_SDK_BINARY_ROOT}"
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

if(NOT TARGET cldt::client_sdk)
    message(FATAL_ERROR "The binary package does not export cldt::client_sdk")
endif()

# Imported targets normally belong only to this directory. Make the targets
# created here available to the parent project using add_subdirectory().
foreach(_cldt_target IN LISTS _cldt_new_imports)
    if(TARGET ${_cldt_target})
        set_property(TARGET ${_cldt_target} PROPERTY IMPORTED_GLOBAL TRUE)
    endif()
endforeach()
