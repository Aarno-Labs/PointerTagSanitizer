# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# C++ libraries compatible with tagged pointers and the host glibc.
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(LLVM_ENABLE_RUNTIMES "libunwind;libcxxabi;libcxx" CACHE STRING "")
set(CMAKE_C_FLAGS "-mllvm -ptsan-strip-ids" CACHE STRING "")
set(CMAKE_CXX_FLAGS "-mllvm -ptsan-strip-ids" CACHE STRING "")
set(LIBCXX_ENABLE_ASSERTIONS OFF CACHE BOOL "")
set(LIBCXX_INCLUDE_TESTS OFF CACHE BOOL "")
set(LIBCXX_INCLUDE_BENCHMARKS OFF CACHE BOOL "")
set(LIBCXXABI_INCLUDE_TESTS OFF CACHE BOOL "")
set(LIBUNWIND_INCLUDE_TESTS OFF CACHE BOOL "")
