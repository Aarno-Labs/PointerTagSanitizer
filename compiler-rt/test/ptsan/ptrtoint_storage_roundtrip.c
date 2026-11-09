//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Serializing a pointer through volatile integer bytes must retain its packed
// representation. Ordinary integer storage is not an address-observation site.

// RUN: %clang_ptsan -O2 -o %t %s
// RUN: %run %t | FileCheck %s

// RUN: %clang_ptsan_id32 -O2 -o %t %s
// RUN: %run %t | FileCheck %s

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

__attribute__((noinline)) static uintptr_t serialize(void *Pointer) {
  uintptr_t Bits = (uintptr_t)Pointer;
  volatile unsigned char Bytes[sizeof(Bits)];

  for (unsigned I = 0; I < sizeof(Bits); ++I) {
    Bytes[I] = Bits >> (I * 8);
  }

  uintptr_t Result = 0;
  for (unsigned I = 0; I < sizeof(Bits); ++I) {
    Result |= (uintptr_t)Bytes[I] << (I * 8);
  }
  return Result;
}

int main(void) {
  void *Pointer = malloc(16);
  assert(Pointer);
  assert(serialize(Pointer) == (uintptr_t)Pointer);
  free(Pointer);
  puts("pointer representation preserved");
  return 0;
}

// CHECK: pointer representation preserved
