//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// LLD emits PT_PHDR for this static executable, exercising musl's load-bias
// calculation from AT_PHDR. Supply the GCC support-library directory normally
// discovered by musl's GNU ld wrapper, which -fuse-ld=lld bypasses.
// REQUIRES: shell, lld-available
// RUN: GCC_DIR=$(dirname "$(%ptsan_musl_strict --sysroot=/ --rtlib=libgcc -print-libgcc-file-name)")
// RUN: %ptsan_musl_strict -fuse-ld=lld --gcc-install-dir="$GCC_DIR" -o %t %s
// RUN: %run %t

#define CHECK_ITERATION
#define REQUIRE_PT_PHDR
#include "auxv_phdr.c"
