# tblog — CMake build. The hand-written Makefile is the primary build; this
# file mirrors it for projects that consume tblog through CMake.
#
#   cmake -S . -B build-cmake -DTBLOG_BUILD_TESTS=ON
#   cmake --build build-cmake -j8 && (cd build-cmake && ctest --output-on-failure)
#   cmake --install build-cmake --prefix /usr/local
#
# Consumers: find_package(tblog REQUIRED) + target_link_libraries(app tblog::tblog),
# or add_subdirectory(tblog) and link tblog::tblog.
cmake_minimum_required(VERSION 3.10)
project(tblog VERSION 0.1.0 LANGUAGES C CXX)

option(TBLOG_BUILD_SHARED   "tblog::tblog refers to the shared library (OFF: static)" ON)
option(TBLOG_BUILD_TESTS    "Build tests/unit/*.cpp and register them with CTest" OFF)
option(TBLOG_BUILD_EXAMPLES "Build examples/*.c" OFF)
set(TBLOG_CXX_STANDARD 17 CACHE STRING "C++ standard used to build tblog (11, 14 or 17)")
set_property(CACHE TBLOG_CXX_STANDARD PROPERTY STRINGS 11 14 17)
if(NOT TBLOG_CXX_STANDARD MATCHES "^(11|14|17)$")
  message(FATAL_ERROR "TBLOG_CXX_STANDARD must be 11, 14 or 17 (got '${TBLOG_CXX_STANDARD}')")
endif()

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
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(TBLOG_WARNINGS -Wall -Wextra)
  set(TBLOG_LIB_OPTIONS -fno-omit-frame-pointer)
endif()

# Common settings for both library flavours.
function(tblog_configure_library target)
  target_include_directories(${target}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(${target} PRIVATE ${TBLOG_WARNINGS} ${TBLOG_LIB_OPTIONS})
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
if(NOT APPLE AND NOT WIN32)
  # Export the tblog_* C API only (also hides std:: template instantiations).
  set(TBLOG_MAP "${CMAKE_CURRENT_SOURCE_DIR}/packaging/tblog.map")
  target_link_libraries(tblog_shared PRIVATE "-Wl,--version-script=${TBLOG_MAP}")
  set_target_properties(tblog_shared PROPERTIES LINK_DEPENDS "${TBLOG_MAP}")
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
  enable_testing()
  set(TBLOG_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
  file(MAKE_DIRECTORY "${TBLOG_TEST_DIR}")
  file(GLOB TBLOG_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp")
  foreach(src ${TBLOG_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    # Unit tests use internal C++ symbols, which only the static lib provides.
    target_link_libraries(${name} PRIVATE tblog_static)
    target_include_directories(${name} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(${name} PRIVATE ${TBLOG_WARNINGS})
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
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "TBLOG_TEST_DIR=${TBLOG_TEST_DIR}")
  endforeach()
endif()

# ---- examples ----------------------------------------------------------------
if(TBLOG_BUILD_EXAMPLES)
  file(GLOB TBLOG_EXAMPLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c")
  foreach(src ${TBLOG_EXAMPLE_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(example_${name} ${src})
    target_link_libraries(example_${name} PRIVATE tblog::tblog)
    set_target_properties(example_${name} PROPERTIES
      OUTPUT_NAME ${name}
      C_STANDARD 99
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF
      ENABLE_EXPORTS ON
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/examples)
  endforeach()
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
