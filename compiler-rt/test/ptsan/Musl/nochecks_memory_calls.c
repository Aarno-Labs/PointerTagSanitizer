//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.split
// RUN: %ptsan_musl_strict -mllvm -ptsan-nochecks \
// RUN:   -ffreestanding -c %t.split/nochecks.c -o %t.nochecks.o
// RUN: %ptsan_musl_strict \
// RUN:   -c %t.split/checked.c -o %t.checked.o
// RUN: %ptsan_musl_strict -fno-sanitize=ptsan \
// RUN:   -c %t.split/main.c -o %t.main.o
// RUN: %ptsan_musl_strict %t.main.o %t.nochecks.o %t.checked.o -o %t
// RUN: %run %t

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
#include <stdint.h>
#include <stdlib.h>

void checked_bump(int *p) {
  if (!((uintptr_t)p >> 48)) {
    _Exit(10);
  }
  ++*p;
}

//--- main.c
#include <stdint.h>
#include <stdlib.h>

uintptr_t exercise(char *, char *, size_t, int);

int main(void) {
  char dest[64], source[64];
  for (int tagged = 0; tagged != 2; ++tagged) {
    uintptr_t id = (uint64_t)(tagged ? 42 : 0) << 48;
    char *d = (char *)((uintptr_t)dest | id);
    char *s = (char *)((uintptr_t)source | id);
    for (int indirect = 0; indirect != 2; ++indirect) {
      if (exercise(d, s, sizeof(dest), indirect) != (uintptr_t)d + 1) {
        _Exit(1);
      }
      for (size_t i = 0; i < sizeof(dest); ++i) {
        if (dest[i] != 7 || source[i] != 7) {
          _Exit(2);
        }
      }
    }
  }
  _Exit(0);
}
