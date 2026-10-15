//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: mkdir -p %t.dir
// RUN: %ptsan_musl_strict \
// RUN:   -o %t.dir/progname-short %s
// RUN: %ptsan_musl_strict -DREAD_FULL_NAME \
// RUN:   -o %t.dir/progname-full %s
// RUN: rm -f %t.failed
// RUN: %run %t.dir/progname-short > %t.short.log 2>&1 || touch %t.failed
// RUN: %run %t.dir/progname-full > %t.full.log 2>&1 || touch %t.failed
// RUN: test ! -e %t.failed

// musl's __init_libc saves argv[0] in __progname_full and a pointer past its
// last slash in __progname. These are the actual libc fields, also exposed as
// program_invocation_name and program_invocation_short_name. Do not initialize
// them here: the test must exercise the pointers retained by musl startup.
//
// Tagging argv's entries later in main would not update these aliases.
// Startup tagging must account for the saved full pointer and the interior
// basename pointer as well, before application constructors execute.
// Run both cases even if the first fails, and avoid musl's normal exit path.
#define _GNU_SOURCE
#include "../Inputs/ptsan_test_interface.h"
#include <errno.h>
#include <stdlib.h>

extern char *__progname;
extern char *__progname_full;

__attribute__((constructor)) static void checkProgramName(void) {
#ifdef READ_FULL_NAME
  unsigned char c = *(volatile unsigned char *)__progname_full;
  if (c == '\0')
    _Exit(2);
#else
  unsigned char c = *(volatile unsigned char *)__progname;
  if (c != 'p')
    _Exit(2);
#endif
}

int main(int argc, char **argv) {
  unsigned id = get_id(argv[0]);
  if (id == 0 || id == ptsan_default_id() || get_id(__progname) != id ||
      get_id(__progname_full) != id)
    _Exit(3);
  if (__progname_full != argv[0] ||
      program_invocation_name != __progname_full ||
      program_invocation_short_name != __progname)
    _Exit(4);
  checkProgramName();
  _Exit(0);
}
