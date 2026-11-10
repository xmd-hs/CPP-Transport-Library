#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "xmdMemoryPool::memory_pool" for configuration "Release"
set_property(TARGET xmdMemoryPool::memory_pool APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(xmdMemoryPool::memory_pool PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libxmd_memory_pool.a"
  )

list(APPEND _cmake_import_check_targets xmdMemoryPool::memory_pool )
list(APPEND _cmake_import_check_files_for_xmdMemoryPool::memory_pool "${_IMPORT_PREFIX}/lib/libxmd_memory_pool.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
