# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Shared by the musl dependency caches and Clang configuration generation.
# Callers select a target explicitly; neither architecture is a default.
if(NOT DEFINED PTSAN_MUSL_TARGET OR PTSAN_MUSL_TARGET STREQUAL "")
  message(FATAL_ERROR "PTSAN_MUSL_TARGET must be set explicitly")
endif()
if(NOT PTSAN_MUSL_TARGET MATCHES "^(x86_64|aarch64)(-[^-]+)?-linux-musl$")
  message(FATAL_ERROR
    "Unsupported PTSAN_MUSL_TARGET '${PTSAN_MUSL_TARGET}'; expected an x86-64 or AArch64 Linux musl target")
endif()
