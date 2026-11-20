# Shared by correctness and performance CI. Paths and per-mode compiler configs
# are supplied by ptsan_ci_config.py; dependency recipes remain in PTSan.cmake.
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(CMAKE_MAKE_PROGRAM /usr/bin/make CACHE FILEPATH "")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "")
set(TEST_SUITE_HOST_CC /usr/bin/clang CACHE FILEPATH "")
set(TEST_SUITE_SUBDIRS MultiSource CACHE STRING "")
set(TEST_SUITE_RUN_TYPE train CACHE STRING "")
set(TEST_SUITE_BENCHMARKING_ONLY ON CACHE BOOL "")
set(TEST_SUITE_FORTRAN OFF CACHE BOOL "")
set(TEST_SUITE_COLLECT_CODE_SIZE OFF CACHE BOOL "")
set(TEST_SUITE_COLLECT_COMPILE_TIME OFF CACHE BOOL "")
set(TEST_SUITE_RUN_BENCHMARKS ON CACHE BOOL "")
# Build native fpcmp/timeit, not instrumented benchmark tools.
set(TEST_SUITE_USER_MODE_EMULATION ON CACHE BOOL "")
