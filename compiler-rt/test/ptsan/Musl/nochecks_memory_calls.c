//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.split
// RUN: %ptsan_musl_strict -mllvm -ptsan-nochecks \
// RUN:   -ffreestanding -I%S/../Inputs -c %t.split/nochecks.c -o %t.nochecks.o
// RUN: %ptsan_musl_strict \
// RUN:   -I%S/../Inputs -c %t.split/checked.c -o %t.checked.o
// RUN: %ptsan_musl_strict -fno-sanitize=ptsan \
// RUN:   -I%S/../Inputs -c %t.split/main.c -o %t.main.o
// RUN: %ptsan_musl_strict %t.main.o %t.nochecks.o %t.checked.o -o %t
// RUN: %run %t
//
// RUN: %ptsan_musl_id32_strict_indicator -mllvm -ptsan-may-decode-raw -mllvm -ptsan-nochecks \
// RUN:   -ffreestanding -I%S/../Inputs -c %t.split/nochecks.c -o %t.id32.nochecks.o
// RUN: %ptsan_musl_id32_strict_indicator \
// RUN:   -I%S/../Inputs -c %t.split/checked.c -o %t.id32.checked.o
// RUN: %ptsan_musl_id32_strict_indicator -fno-sanitize=ptsan \
// RUN:   -I%S/../Inputs -c %t.split/main.c -o %t.id32.main.o
// RUN: %ptsan_musl_id32_strict_indicator %t.id32.main.o %t.id32.nochecks.o %t.id32.checked.o -o %t.id32
// RUN: %run %t.id32

// Use registered buffers so both pointer encodings have real metadata.
// Like the allocator wrappers, this plain caller supplies raw addresses to
// no-checks code. Explicit memory calls/references must not recheck them.
// The no-checks TU must still pass a tagged global to a checked callee.

//--- nochecks.c
#include <stdint.h>
#include <string.h>

static int counter;
void checked_bump(int *);
static void *(*volatile copy)(void *, const void *, size_t) = memcpy;
static void *(*volatile move)(void *, const void *, size_t) = memmove;
static void *(*volatile fill)(void *, int, size_t) = memset;

uintptr_t exercise(char *dest, char *source, size_t size, int indirect) {
  checked_bump(&counter);
  if (indirect) {
    fill(source, 7, size);
    copy(dest, source, size);
    return (uintptr_t)move(dest + 1, dest, size - 1);
  }
  memset(source, 7, size);
  memcpy(dest, source, size);
  return (uintptr_t)memmove(dest + 1, dest, size - 1);
}

//--- checked.c
#include "ptsan_test_interface.h"
#include <stdint.h>
#include <stdlib.h>

char *allocate_buffer(size_t size) {
  return malloc(size);
}

void checked_bump(int *p) {
  if (!get_id(p)) {
    _Exit(10);
  }
  ++*p;
}

//--- main.c
#include "ptsan_test_interface.h"
#include <stdint.h>
#include <stdlib.h>

uintptr_t exercise(char *, char *, size_t, int);
char *allocate_buffer(size_t);

int main(void) {
  const size_t size = 64;
  char *tagged_dest = allocate_buffer(size);
  char *tagged_source = allocate_buffer(size);
  if (!tagged_dest || !tagged_source) {
    _Exit(3);
  }
  char *dest = (char *)ptsan_get_address(tagged_dest);
  char *source = (char *)ptsan_get_address(tagged_source);
  for (int tagged = 0; tagged != 2; ++tagged) {
    char *d = tagged ? tagged_dest : dest;
    char *s = tagged ? tagged_source : source;
    for (int indirect = 0; indirect != 2; ++indirect) {
      if (exercise(d, s, size, indirect) != (uintptr_t)d + 1) {
        _Exit(1);
      }
      for (size_t i = 0; i < size; ++i) {
        if (dest[i] != 7 || source[i] != 7) {
          _Exit(2);
        }
      }
    }
  }
  _Exit(0);
}
