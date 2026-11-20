# Minimal downstream project used by CI to check the installed CMake package:
#   cmake -S packaging/consumer -B build-consumer -DCMAKE_PREFIX_PATH=<tblog prefix>
#   cmake --build build-consumer
# Builds the examples against the *installed* tblog (not the source tree).
cmake_minimum_required(VERSION 3.10)
project(tblog_consumer LANGUAGES C CXX)

find_package(tblog 0.1 REQUIRED)

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
