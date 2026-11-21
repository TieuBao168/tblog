# Minimal downstream project used by CI to check how other projects consume
# tblog. Builds the examples against:
#   - the *installed* tblog (find_package), by default:
#       cmake -S packaging/consumer -B build-consumer -DCMAKE_PREFIX_PATH=<tblog prefix>
#   - the source tree added as a subdirectory (no install), with
#     -DTBLOG_SOURCE_DIR=<tblog checkout>:
#       cmake -S packaging/consumer -B build-subdir -DTBLOG_SOURCE_DIR=$PWD
#   cmake --build build-consumer
cmake_minimum_required(VERSION 3.10)
project(tblog_consumer LANGUAGES C CXX)

if(TBLOG_SOURCE_DIR)
  # Like add_subdirectory(third_party/tblog) or FetchContent_MakeAvailable(tblog).
  # Executables and (Windows) tblog.dll in one directory, so they run as built.
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  add_subdirectory("${TBLOG_SOURCE_DIR}" tblog)
else()
  find_package(tblog 0.1 REQUIRED)
endif()

set(TBLOG_EXAMPLES_DIR "${CMAKE_CURRENT_LIST_DIR}/../../examples")

# tblog::tblog: the shared library unless tblog was built with TBLOG_BUILD_SHARED=OFF.
add_executable(basic_default "${TBLOG_EXAMPLES_DIR}/basic.c")
target_link_libraries(basic_default PRIVATE tblog::tblog)

add_executable(basic_static "${TBLOG_EXAMPLES_DIR}/basic.c")
target_link_libraries(basic_static PRIVATE tblog::tblog_static)

# The C++ header API must work for a C++11 consumer.
add_executable(cpp_api "${TBLOG_EXAMPLES_DIR}/cpp_api.cpp")
target_link_libraries(cpp_api PRIVATE tblog::tblog)
set_target_properties(cpp_api PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
