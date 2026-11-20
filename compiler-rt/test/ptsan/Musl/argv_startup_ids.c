//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_nonstrict -o %t %s
// RUN: env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t argument ""
// RUN: env -i %run %t argument ""
// RUN: not env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t argument "" bad 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -o %t.strict %s
// RUN: env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t.strict argument ""
// RUN: env -i %run %t.strict argument ""
// RUN: not env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t.strict argument "" bad 2>&1 | FileCheck %s
//
// ID32 startup strings must share an origin as well as an ID.
// RUN: %ptsan_musl_id32_nonstrict -o %t.id32 %s
// RUN: env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t.id32 argument ""
// RUN: env -i %run %t.id32 argument ""
// RUN: not env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t.id32 argument "" bad 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict -o %t.id32.strict %s
// RUN: env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t.id32.strict argument ""
// RUN: env -i %run %t.id32.strict argument ""
// RUN: not env -i PTSAN_ENV=alpha PTSAN_EMPTY= %run %t.id32.strict argument "" bad 2>&1 | FileCheck %s

// CHECK: Found bad POINT access
// CHECK-NOT: Found bad

// Initial argument and environment strings share permanent bounds. Their
// pointer arrays remain separate objects. Process-title writes may span the
// strings, but accesses past the entire block must still be rejected.
#include "../Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;
static volatile uintptr_t saved_end;

__attribute__((noinline)) static uint32_t title_length(char *begin, char *last) {
  // Preserve Perl's adjusted-integer subtraction shape. Sharing the origin
  // must make the result correct without special arithmetic recognition.
  uintptr_t end = (uintptr_t)last + 1;
  saved_end = end;
  return (uint32_t)(end - (uintptr_t)begin);
}

__attribute__((noinline)) static void write_title(char *begin, uint32_t size) {
  memset(begin, 'x', size - 1);
  begin[size - 1] = '\0';
}

__attribute__((noinline)) static void read_past_end(char *begin, uint32_t size) {
  (void)*(volatile char *)(begin + size);
}

int main(int argc, char **argv) {
  assert(argc == 3 || argc == 4);
  unsigned id = get_id(argv[0]);
  assert(id != 0 && id != ptsan_default_id());
  assert(get_id(argv) != id && get_id(environ) != id);
  assert((ptsan_id_bits() == 32 ? ptsan_get_base(id) :
          ptsan_get_address((void *)ptsan_get_base(id))) ==
         ptsan_get_address(argv[0]));

  uint64_t end = ptsan_get_address(argv[0]);
  char *last = NULL;
  for (int i = 0; i < argc; ++i) {
    assert(get_id(argv[i]) == id);
    assert(ptsan_get_address(argv[i]) == end);
    size_t length = strlen(argv[i]);
    end += length + 1;
    last = argv[i] + length;
  }
  for (unsigned i = 0; environ[i]; ++i) {
    assert(get_id(environ[i]) == id);
    assert(ptsan_get_address(environ[i]) == end);
    size_t length = strlen(environ[i]);
    end += length + 1;
    last = environ[i] + length;
  }
  assert(argv[1][8] == '\0' && argv[2][0] == '\0');
  uint64_t size = end - ptsan_get_address(argv[0]);
  assert(size == ptsan_get_size(id) && size <= UINT32_MAX);
  assert(title_length(argv[0], last) == size);
  if (argc == 4) {
    read_past_end(argv[0], size);
    assert(!"access beyond the startup string block should fail");
  }

  write_title(argv[0], size);
  assert(argv[0][0] == 'x' && argv[1][0] == 'x');
  if (environ[0]) {
    assert(environ[0][0] == 'x');
  }
  assert(*last == '\0');
  _Exit(0);
}
