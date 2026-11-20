# tblog — CMake build. This is the primary build (verified in CI, see
# .github/workflows/ci.yml); the Makefile is a Linux-only convenience.
#
#   cmake -S . -B build-cmake -DTBLOG_BUILD_TESTS=ON
#   cmake --build build-cmake -j8 && (cd build-cmake && ctest --output-on-failure)
#   cmake --install build-cmake --prefix /usr/local
#
# Consumers: find_package(tblog REQUIRED) + target_link_libraries(app tblog::tblog),
# or add_subdirectory(tblog) and link tblog::tblog.
#
# Header check only (compile the public headers, build nothing else; what the
# Windows and macOS CI jobs use until the library is ported there):
#   cmake -S . -B build-hc -DTBLOG_HEADERS_ONLY_CHECK=ON [-DCMAKE_CXX_STANDARD=20]
#   cmake --build build-hc
cmake_minimum_required(VERSION 3.10)
if(POLICY CMP0092)
  # MSVC: no default /W3 in CMAKE_<LANG>_FLAGS, so /W4 does not trigger D9025.
  cmake_policy(SET CMP0092 NEW)
endif()
project(tblog VERSION 0.1.0 LANGUAGES C CXX)

option(TBLOG_HEADERS_ONLY_CHECK "Configure only the public-header compile check (no library, tests or install)" OFF)
option(TBLOG_BUILD_SHARED   "tblog::tblog refers to the shared library (OFF: static)" ON)
option(TBLOG_BUILD_TESTS    "Build tests/unit/*.cpp and register them with CTest" OFF)
option(TBLOG_BUILD_EXAMPLES "Build examples/*.c and examples/*.cpp" OFF)
option(TBLOG_BUILD_BENCH    "Build bench/bench_latency.cpp" OFF)
option(TBLOG_WERROR         "Treat compiler warnings as errors" OFF)
set(TBLOG_CXX_STANDARD 17 CACHE STRING "C++ standard used to build tblog (11, 14 or 17)")
set_property(CACHE TBLOG_CXX_STANDARD PROPERTY STRINGS 11 14 17)
if(NOT TBLOG_CXX_STANDARD MATCHES "^(11|14|17)$")
  message(FATAL_ERROR "TBLOG_CXX_STANDARD must be 11, 14 or 17 (got '${TBLOG_CXX_STANDARD}')")
endif()

# ---- public header check -------------------------------------------------------
# tests/header_check/check_{c.c,cpp.cpp} use every public macro and function of
# <tblog/log.h> and <tblog/tblog.hpp>. Compiled only (object libraries, never
# linked), with strict warnings as errors whatever TBLOG_WERROR says, and once
# more with TBLOG_DLL (the dllimport flavour on Windows). Needs only the include
# directory, not the library. The C++ standard is CMAKE_CXX_STANDARD when given
# (e.g. 20), else TBLOG_CXX_STANDARD; C is C99 (C11 on MSVC).
function(tblog_add_header_check)
  set(_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/header_check")
  foreach(_variant plain dll)
    foreach(_lang C CXX)
      if(_lang STREQUAL "C")
        set(_name tblog_header_check_c)
        set(_src "${_dir}/check_c.c")
      else()
        set(_name tblog_header_check_cpp)
        set(_src "${_dir}/check_cpp.cpp")
      endif()
      if(_variant STREQUAL "dll")
        set(_name ${_name}_dll)
      endif()
      add_library(${_name} OBJECT "${_src}")
      target_include_directories(${_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
      if(_variant STREQUAL "dll")
        target_compile_definitions(${_name} PRIVATE TBLOG_DLL)
      endif()
      if(MSVC)
        target_compile_options(${_name} PRIVATE /W4 /WX)
        if(_lang STREQUAL "CXX")
          target_compile_options(${_name} PRIVATE /permissive-)
        endif()
      elseif(CMAKE_${_lang}_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${_name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      if(_lang STREQUAL "C")
        if(MSVC)
          set(_cstd 11)
        else()
          set(_cstd 99)
        endif()
        set_target_properties(${_name} PROPERTIES
          C_STANDARD ${_cstd} C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
      else()
        if(DEFINED CMAKE_CXX_STANDARD)
          set(_cxxstd ${CMAKE_CXX_STANDARD})
        else()
          set(_cxxstd ${TBLOG_CXX_STANDARD})
        endif()
        set_target_properties(${_name} PROPERTIES
          CXX_STANDARD ${_cxxstd} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
      endif()
    endforeach()
  endforeach()
endfunction()

if(TBLOG_HEADERS_ONLY_CHECK)
  tblog_add_header_check()
  return()
endif()

# Sanitizers for the library, tests, examples and bench. One of address,
# thread, undefined, or a comma-separated combination such as address,undefined.
set(TBLOG_SANITIZE "" CACHE STRING "Sanitizers: empty, address, thread, undefined or address,undefined")
set_property(CACHE TBLOG_SANITIZE PROPERTY STRINGS "" address thread undefined "address,undefined")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# zlib for file.compress=gzip; without it compression is disabled (with a warning).
option(TBLOG_WITH_ZLIB "Use zlib for gzip compression of rotated files" ON)
set(TBLOG_HAVE_ZLIB OFF)
if(TBLOG_WITH_ZLIB)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    set(TBLOG_HAVE_ZLIB ON)
  endif()
endif()

# New modules added under src/ are picked up automatically (re-run cmake on
# CMake < 3.12, which lacks CONFIGURE_DEPENDS).
if(NOT CMAKE_VERSION VERSION_LESS 3.12)
  file(GLOB_RECURSE TBLOG_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
else()
  file(GLOB_RECURSE TBLOG_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
endif()

set(TBLOG_WARNINGS)
set(TBLOG_LIB_OPTIONS)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(TBLOG_WARNINGS -Wall -Wextra)
  if(TBLOG_WERROR)
    list(APPEND TBLOG_WARNINGS -Werror)
  endif()
  set(TBLOG_LIB_OPTIONS -fno-omit-frame-pointer)
endif()

# Sanitizer flags: compile and link options of every target built here.
set(TBLOG_SAN_COMPILE)
set(TBLOG_SAN_LINK)
if(TBLOG_SANITIZE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "TBLOG_SANITIZE needs GCC or Clang")
  endif()
  string(REPLACE "," ";" _tblog_san_list "${TBLOG_SANITIZE}")
  foreach(_san ${_tblog_san_list})
    if(NOT _san MATCHES "^(address|thread|undefined)$")
      message(FATAL_ERROR "TBLOG_SANITIZE: unknown sanitizer '${_san}' (use address, thread, undefined)")
    endif()
  endforeach()
  if("thread" IN_LIST _tblog_san_list AND "address" IN_LIST _tblog_san_list)
    message(FATAL_ERROR "TBLOG_SANITIZE: thread and address cannot be combined")
  endif()
  set(TBLOG_SAN_COMPILE -fsanitize=${TBLOG_SANITIZE} -fno-omit-frame-pointer)
  if("undefined" IN_LIST _tblog_san_list)
    # Make UBSan findings fail the test instead of only printing a report.
    list(APPEND TBLOG_SAN_COMPILE -fno-sanitize-recover=undefined)
  endif()
  set(TBLOG_SAN_LINK -fsanitize=${TBLOG_SANITIZE})
endif()

# Warnings + sanitizers for any target built here.
function(tblog_target_flags target)
  target_compile_options(${target} PRIVATE ${TBLOG_WARNINGS} ${TBLOG_SAN_COMPILE})
  if(TBLOG_SAN_LINK)
    target_link_libraries(${target} PRIVATE ${TBLOG_SAN_LINK})
  endif()
endfunction()

# Common settings for both library flavours.
function(tblog_configure_library target)
  target_include_directories(${target}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(${target} PRIVATE ${TBLOG_LIB_OPTIONS})
  tblog_target_flags(${target})
  target_link_libraries(${target} PUBLIC Threads::Threads)
  if(TBLOG_HAVE_ZLIB)
    target_compile_definitions(${target} PRIVATE TBLOG_HAVE_ZLIB=1)
    target_link_libraries(${target} PRIVATE ZLIB::ZLIB)
  endif()
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME tblog
    CXX_STANDARD ${TBLOG_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)
endfunction()

add_library(tblog_static STATIC ${TBLOG_SOURCES})
tblog_configure_library(tblog_static)

add_library(tblog_shared SHARED ${TBLOG_SOURCES})
tblog_configure_library(tblog_shared)
set_target_properties(tblog_shared PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})
if(WIN32)
  # TBLOG_API (include/tblog/log.h): dllexport while building the DLL,
  # dllimport for whoever links it -- also through the installed package
  # (INTERFACE_COMPILE_DEFINITIONS of tblog::tblog_shared).
  target_compile_definitions(tblog_shared PRIVATE TBLOG_BUILDING_DLL INTERFACE TBLOG_DLL)
endif()
if(NOT APPLE AND NOT WIN32)
  # Export the tblog_* C API only (also hides std:: template instantiations).
  set(TBLOG_MAP "${CMAKE_CURRENT_SOURCE_DIR}/packaging/tblog.map")
  target_link_libraries(tblog_shared PRIVATE "-Wl,--version-script=${TBLOG_MAP}")
  set_target_properties(tblog_shared PROPERTIES LINK_DEPENDS "${TBLOG_MAP}")
  if(NOT TBLOG_SANITIZE)
    # Like the Makefile: every symbol of the .so must resolve at link time
    # (sanitizer runtimes are only pulled in by the executable).
    target_link_libraries(tblog_shared PRIVATE "-Wl,--no-undefined")
  endif()
endif()

if(TBLOG_BUILD_SHARED)
  add_library(tblog::tblog ALIAS tblog_shared)
else()
  add_library(tblog::tblog ALIAS tblog_static)
endif()
add_library(tblog::tblog_static ALIAS tblog_static)
add_library(tblog::tblog_shared ALIAS tblog_shared)

# ---- tests -------------------------------------------------------------------
if(TBLOG_BUILD_TESTS)
  tblog_add_header_check()
  enable_testing()
  set(TBLOG_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")

  # TSan aborts with "unexpected memory mapping" under high-entropy ASLR on new
  # kernels; run its binaries with ASLR disabled (same as the Makefile).
  set(TBLOG_TEST_RUNNER)
  if(TBLOG_SANITIZE MATCHES "thread" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(TBLOG_SETARCH setarch)
    if(TBLOG_SETARCH)
      set(TBLOG_TEST_RUNNER "${TBLOG_SETARCH}" "${CMAKE_HOST_SYSTEM_PROCESSOR}" -R)
    else()
      message(WARNING "setarch not found: TSan tests run with ASLR enabled and may abort")
    endif()
  endif()

  file(GLOB TBLOG_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp")
  foreach(src ${TBLOG_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    # Unit tests use internal C++ symbols, which only the static lib provides.
    target_link_libraries(${name} PRIVATE tblog_static)
    target_include_directories(${name} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    tblog_target_flags(${name})
    if(TBLOG_HAVE_ZLIB)
      # Compression tests (and their zlib-based gunzip) need it, like the Makefile.
      target_compile_definitions(${name} PRIVATE TBLOG_HAVE_ZLIB=1)
      target_link_libraries(${name} PRIVATE ZLIB::ZLIB)
    endif()
    set_target_properties(${name} PROPERTIES
      CXX_STANDARD ${TBLOG_CXX_STANDARD}
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
      ENABLE_EXPORTS ON                       # -rdynamic, like the Makefile
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests)
    add_test(NAME ${name} COMMAND ${TBLOG_TEST_RUNNER} $<TARGET_FILE:${name}>)
    # One scratch directory per test, so `ctest -j` cannot mix their files.
    file(MAKE_DIRECTORY "${TBLOG_TEST_DIR}/${name}")
    set_tests_properties(${name} PROPERTIES
      ENVIRONMENT "TBLOG_TEST_DIR=${TBLOG_TEST_DIR}/${name}"
      TIMEOUT 300)
  endforeach()

  # The .so must export exactly the tblog_* functions that libtblog.a defines
  # (`make check-symbols`). ELF only; skipped in sanitizer builds, whose
  # instrumentation is not part of the exported API.
  if(NOT WIN32 AND NOT APPLE AND NOT TBLOG_SANITIZE)
    set(_tblog_nm "${CMAKE_NM}")
    if(NOT _tblog_nm)
      find_program(TBLOG_NM_EXECUTABLE nm)
      set(_tblog_nm "${TBLOG_NM_EXECUTABLE}")
    endif()
    if(_tblog_nm)
      add_test(NAME check_symbols
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/packaging/check-symbols.sh"
                $<TARGET_FILE:tblog_static> $<TARGET_FILE:tblog_shared>)
      set_tests_properties(check_symbols PROPERTIES
        ENVIRONMENT "NM=${_tblog_nm}"
        TIMEOUT 60)
    else()
      message(STATUS "nm not found: check_symbols test not registered")
    endif()
  endif()
endif()

# ---- examples ----------------------------------------------------------------
if(TBLOG_BUILD_EXAMPLES)
  file(GLOB TBLOG_EXAMPLE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")
  foreach(src ${TBLOG_EXAMPLE_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(example_${name} ${src})
    target_link_libraries(example_${name} PRIVATE tblog::tblog)
    tblog_target_flags(example_${name})
    set_target_properties(example_${name} PROPERTIES
      OUTPUT_NAME ${name}
      C_STANDARD 99
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF
      CXX_STANDARD ${TBLOG_CXX_STANDARD}
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
      ENABLE_EXPORTS ON
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/examples)
  endforeach()
endif()

# ---- bench -------------------------------------------------------------------
# Run from the source directory: it writes under build/bench_tmp/.
if(TBLOG_BUILD_BENCH)
  add_executable(bench_latency "${CMAKE_CURRENT_SOURCE_DIR}/bench/bench_latency.cpp")
  target_link_libraries(bench_latency PRIVATE tblog_static)
  tblog_target_flags(bench_latency)
  set_target_properties(bench_latency PROPERTIES
    CXX_STANDARD ${TBLOG_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    ENABLE_EXPORTS ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bench)
endif()

# ---- install + package config -------------------------------------------------
include(CMakePackageConfigHelpers)
set(TBLOG_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/tblog")

install(TARGETS tblog_static tblog_shared
  EXPORT tblogTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/tblog DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
install(EXPORT tblogTargets NAMESPACE tblog:: DESTINATION ${TBLOG_CMAKE_DIR})

configure_package_config_file(packaging/tblogConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/tblogConfig.cmake"
  INSTALL_DESTINATION ${TBLOG_CMAKE_DIR})
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/tblogConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/tblogConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/tblogConfigVersion.cmake"
  DESTINATION ${TBLOG_CMAKE_DIR})

# tblog.pc — same template as `make install`.
set(TBLOG_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
foreach(kind LIBDIR INCLUDEDIR)
  if(IS_ABSOLUTE "${CMAKE_INSTALL_${kind}}")
    set(TBLOG_PC_${kind} "${CMAKE_INSTALL_${kind}}")
  else()
    set(TBLOG_PC_${kind} "\${prefix}/${CMAKE_INSTALL_${kind}}")
  endif()
endforeach()
set(TBLOG_VERSION ${PROJECT_VERSION})
if(TBLOG_HAVE_ZLIB)
  set(TBLOG_PC_PRIVATE_LIBS "-lz")
else()
  set(TBLOG_PC_PRIVATE_LIBS "")
endif()
configure_file(packaging/tblog.pc.in "${CMAKE_CURRENT_BINARY_DIR}/tblog.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/tblog.pc"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
