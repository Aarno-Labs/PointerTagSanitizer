# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# The runtime itself is uninstrumented, compiled against plain musl headers.
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
include("${CMAKE_CURRENT_LIST_DIR}/../../../llvm/cmake/modules/LLVMPTSanMuslTarget.cmake")
# PTSAN_MUSL_TARGET is the single target selection for every language.
set(CMAKE_C_COMPILER_TARGET "${PTSAN_MUSL_TARGET}" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER_TARGET "${PTSAN_MUSL_TARGET}" CACHE STRING "" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY CACHE STRING "")
set(COMPILER_RT_DEFAULT_TARGET_ONLY ON CACHE BOOL "")
set(COMPILER_RT_SANITIZERS_TO_BUILD ptsan CACHE STRING "")
set(COMPILER_RT_PTSAN_USE_INSTRUMENTED_LIBC ON CACHE BOOL "")
