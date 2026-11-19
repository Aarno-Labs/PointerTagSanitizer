//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O3 -mllvm -ptsan-clear-ids-on-cmp=0 -c %t.dir/helper.c -o %t.helper.o
// RUN: %clang_ptsan -O3 %t.dir/main.c %t.helper.o -o %t
// RUN: %run %t

// Perl pack encodes pointer bytes as UTF-8. On little-endian x86-64, LLVM
// simplifies the high byte's top-bit test to a signed pointer comparison.
// Its branch must agree with the byte being encoded, including the marker bit.

//--- helper.c
#include <stddef.h>
#include <stdint.h>

uintptr_t encode(void *p, unsigned char *out, unsigned *length) {
  unsigned char byte = ((unsigned char *)&p)[sizeof(p) - 1];
  if (byte < 128) {
    out[0] = byte;
    *length = 1;
  } else {
    out[0] = 0xc0 | (byte >> 6);
    out[1] = 0x80 | (byte & 0x3f);
    *length = 2;
  }
  return (uintptr_t)p;
}

//--- main.c
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

uintptr_t encode(void *, unsigned char *, unsigned *);

int main(void) {
  void *p = malloc(16);
  assert(p);
  unsigned char out[2];
  unsigned length;
  uintptr_t bits = encode(p, out, &length);
  unsigned byte = bits >> 56;
  assert(length == (byte < 128 ? 1 : 2));
  unsigned decoded = length == 1 ? out[0] :
      ((out[0] & 0x1f) << 6) | (out[1] & 0x3f);
  assert(decoded == byte);
  free(p);
}
