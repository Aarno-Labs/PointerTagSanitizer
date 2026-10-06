# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Static C++ libraries for instrumented musl. Instrumentation flags and the
# sysroot are supplied by the dependency build for the selected PTSan mode.
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
include("${CMAKE_CURRENT_LIST_DIR}/../../../llvm/cmake/modules/LLVMPTSanMuslTarget.cmake")
# PTSAN_MUSL_TARGET is the single target selection for every language.
set(CMAKE_C_COMPILER_TARGET "${PTSAN_MUSL_TARGET}" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER_TARGET "${PTSAN_MUSL_TARGET}" CACHE STRING "" FORCE)
set(CMAKE_ASM_COMPILER_TARGET "${PTSAN_MUSL_TARGET}" CACHE STRING "" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY CACHE STRING "")
set(LLVM_ENABLE_RUNTIMES "libunwind;libcxxabi;libcxx" CACHE STRING "")
set(CMAKE_PROJECT_Runtimes_INCLUDE
  "${CMAKE_CURRENT_LIST_DIR}/../../../llvm/cmake/modules/LLVMPTSanConfigureUnwind.cmake"
  CACHE FILEPATH "")
set(LIBCXX_HAS_MUSL_LIBC ON CACHE BOOL "")
set(LIBCXX_ENABLE_SHARED OFF CACHE BOOL "")
set(LIBCXX_ENABLE_STATIC_ABI_LIBRARY ON CACHE BOOL "")
set(LIBCXXABI_ENABLE_SHARED OFF CACHE BOOL "")
set(LIBCXXABI_ENABLE_STATIC_UNWINDER ON CACHE BOOL "")
set(LIBUNWIND_ENABLE_SHARED OFF CACHE BOOL "")
set(LIBCXX_ENABLE_ASSERTIONS OFF CACHE BOOL "")
set(LIBCXX_INCLUDE_TESTS OFF CACHE BOOL "")
set(LIBCXX_INCLUDE_BENCHMARKS OFF CACHE BOOL "")
set(LIBCXXABI_INCLUDE_TESTS OFF CACHE BOOL "")
set(LIBUNWIND_INCLUDE_TESTS OFF CACHE BOOL "")
