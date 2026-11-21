# Fails unless the CTest tests registered in a build directory are exactly the
# ones the source tree and that build's options call for, so no test can drop
# out of CI unnoticed (a test file that is not registered, an `if()` that
# skips a check on one platform, a stale extra test).
#
#   cmake -DBUILD_DIR=build [-DCONFIG=Release] -P packaging/check-test-inventory.cmake
#
# Expected (from the files on disk and the build's CMakeCache.txt):
#   tests/unit/*.cpp, tests/shared/*.cpp   one test each, named after the file
#   examples/*.c, examples/*.cpp           example_<name>  (TBLOG_BUILD_EXAMPLES)
#   bench_latency_smoke                    TBLOG_BUILD_BENCH, not on Windows
#   check_symbols                          not on Windows, no TBLOG_SANITIZE
#   check_exports                          Windows
# CONFIG is passed to `ctest -C` (multi-config generators: MSVC).
cmake_minimum_required(VERSION 3.10)

if(NOT BUILD_DIR)
  message(FATAL_ERROR "usage: cmake -DBUILD_DIR=<build> [-DCONFIG=<cfg>] -P check-test-inventory.cmake")
endif()
get_filename_component(BUILD_DIR "${BUILD_DIR}" ABSOLUTE)
get_filename_component(_src "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT EXISTS "${BUILD_DIR}/CMakeCache.txt")
  message(FATAL_ERROR "check-test-inventory: ${BUILD_DIR}/CMakeCache.txt not found")
endif()

# A cache entry NAME:TYPE=VALUE of the build; "" when absent.
function(_cache_value name out)
  file(STRINGS "${BUILD_DIR}/CMakeCache.txt" _line REGEX "^${name}:[A-Z]+=")
  set(_v "")
  if(_line)
    list(GET _line 0 _line)
    string(REGEX REPLACE "^${name}:[A-Z]+=" "" _v "${_line}")
  endif()
  set(${out} "${_v}" PARENT_SCOPE)
endfunction()

_cache_value(TBLOG_BUILD_TESTS _tests)
_cache_value(TBLOG_BUILD_EXAMPLES _examples)
_cache_value(TBLOG_BUILD_BENCH _bench)
_cache_value(TBLOG_SANITIZE _sanitize)
if(NOT _tests)
  message(FATAL_ERROR "check-test-inventory: ${BUILD_DIR} was not configured with TBLOG_BUILD_TESTS=ON")
endif()

# ---- expected
set(_expected)
file(GLOB _files "${_src}/tests/unit/*.cpp" "${_src}/tests/shared/*.cpp")
foreach(_f ${_files})
  get_filename_component(_n "${_f}" NAME_WE)
  list(APPEND _expected ${_n})
endforeach()
if(_examples)
  file(GLOB _files "${_src}/examples/*.c" "${_src}/examples/*.cpp")
  foreach(_f ${_files})
    get_filename_component(_n "${_f}" NAME_WE)
    list(APPEND _expected example_${_n})
  endforeach()
endif()
if(CMAKE_HOST_WIN32)
  list(APPEND _expected check_exports)
else()
  if(_bench)
    list(APPEND _expected bench_latency_smoke)
  endif()
  if(NOT _sanitize)
    list(APPEND _expected check_symbols)
  endif()
endif()

# ---- registered (ctest -N)
get_filename_component(_bindir "${CMAKE_COMMAND}" DIRECTORY)
find_program(_ctest NAMES ctest HINTS "${_bindir}" NO_DEFAULT_PATH)
if(NOT _ctest)
  find_program(_ctest NAMES ctest)
endif()
if(NOT _ctest)
  message(FATAL_ERROR "check-test-inventory: ctest not found")
endif()
set(_cfg)
if(CONFIG)
  set(_cfg -C "${CONFIG}")
endif()
execute_process(COMMAND "${_ctest}" -N ${_cfg}
  WORKING_DIRECTORY "${BUILD_DIR}"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "check-test-inventory: ctest -N failed (${_rc}):\n${_out}${_err}")
endif()
string(REGEX MATCHALL "Test +#[0-9]+: [A-Za-z0-9_.-]+" _lines "${_out}")
set(_registered)
foreach(_l ${_lines})
  string(REGEX REPLACE "^Test +#[0-9]+: " "" _n "${_l}")
  list(APPEND _registered ${_n})
endforeach()

# ---- compare
set(_missing)
foreach(_n ${_expected})
  list(FIND _registered ${_n} _i)
  if(_i EQUAL -1)
    list(APPEND _missing ${_n})
  endif()
endforeach()
set(_extra)
foreach(_n ${_registered})
  list(FIND _expected ${_n} _i)
  if(_i EQUAL -1)
    list(APPEND _extra ${_n})
  endif()
endforeach()
list(LENGTH _expected _ne)
list(LENGTH _registered _nr)
string(REPLACE ";" " " _reg_text "${_registered}")
message(STATUS "check-test-inventory: ${_nr} registered, ${_ne} expected: ${_reg_text}")
if(_missing OR _extra)
  string(REPLACE ";" " " _missing "${_missing}")
  string(REPLACE ";" " " _extra "${_extra}")
  message(FATAL_ERROR "check-test-inventory: FAIL\n  not registered: ${_missing}\n  unexpected: ${_extra}")
endif()
message(STATUS "check-test-inventory: OK")
