//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#ifndef SPEC_MUSL_COMPAT_EXECINFO_H
#define SPEC_MUSL_COMPAT_EXECINFO_H

// SPEC Blender uses this glibc-only API for optional crash diagnostics.
// Omit the backtrace rather than importing glibc headers into a musl build.
// This is a benchmark porting shim, not a general execinfo implementation.
static inline int backtrace(void **buffer, int size) {
  (void)buffer;
  (void)size;
  return 0;
}

static inline char **backtrace_symbols(void *const *buffer, int size) {
  (void)buffer;
  (void)size;
  return (char **)0;
}

#endif
