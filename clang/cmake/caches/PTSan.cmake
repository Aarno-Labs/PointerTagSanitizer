# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Linux x86-64/AArch64 compiler with PTSan dependency builds.
# Supply PTSAN_MUSL_SOURCE_DIR to also build both instrumented musl modes.
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(LLVM_ENABLE_PROJECTS "clang;compiler-rt" CACHE STRING "")
set(LLVM_TARGETS_TO_BUILD "X86;AArch64" CACHE STRING "")
set(LLVM_ENABLE_PTSAN_DEPENDENCIES ON CACHE BOOL "")
