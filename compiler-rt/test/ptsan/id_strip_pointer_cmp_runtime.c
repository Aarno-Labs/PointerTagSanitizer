//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t.dir && split-file %s %t.dir
// RUN: %clang_strip -O3 -mllvm -ptsan-strip-ids-on-cmp \
// RUN:   -c %t.dir/helper.c -o %t.helper.o
// RUN: %clang_ptsan -O3 %t.dir/main.c %t.helper.o -o %t
// RUN: %run %t

//--- helper.c
#include <stddef.h>

volatile unsigned char Observed;

// This has the same relevant shape as libc++'s in-place string insertion:
// memory uses make Insert stripped, while the compare-only Dst + OldSize can
// remain tagged. libc++ deliberately uses raw-address ordering to test overlap
// between potentially unrelated buffers; mixing the two representations can
// therefore report false overlap.
__attribute__((noinline)) void prepend(char *Dst, size_t Pos, size_t OldSize,
                                       const char *Src, size_t Size) {
  char *Insert = Dst + Pos;
  Observed = Insert[0] + Insert[1] + Insert[2];
  if (Insert + 1 <= Src && Src < Dst + OldSize)
    Src += Size;

  __builtin_memmove(Insert + Size, Insert, OldSize - Pos);
  __builtin_memmove(Insert, Src, Size);
}

//--- main.c
#include <assert.h>
#include <stddef.h>
#include <string.h>

void prepend(char *Dst, size_t Pos, size_t OldSize, const char *Src,
             size_t Size);

int main(void) {
  char Source[32] = "Dimensions: \n";
  char Dst[32] = "  nx: 50\n";
  const char Expected[] = "Dimensions: \n  nx: 50\n";

  prepend(Dst, 0, 9, Source, 13);
  assert(memcmp(Dst, Expected, sizeof(Expected)) == 0);
  return 0;
}
