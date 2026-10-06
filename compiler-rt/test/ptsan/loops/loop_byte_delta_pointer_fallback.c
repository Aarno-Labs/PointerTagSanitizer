//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -O2 \
// RUN:   -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 \
// RUN:   -mllvm -ptsan-reoptimize=0 \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang_ptsan -O2 \
// RUN:   -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 \
// RUN:   -mllvm -ptsan-reoptimize=0 \
// RUN:   -o %exe %s
// RUN: ./%exe 4 2> good.err
// RUN: FileCheck %s --check-prefix=GOOD < good.err
// RUN: ./%exe 5 2> bad.err
// RUN: FileCheck %s --check-prefix=BAD < bad.err

// The row pointer and column stride are both recurrences of the outer loop.
// Consequently, the last address visited by the inner loop contains a pointer
// AddRec shifted by a second outer-loop AddRec.  There is no exact SSA
// representative for that address, and its offset from %row_base still
// contains a recurrence, so it cannot be serialized directly.  The byte delta
// therefore remains pointer-dependent and must be calculated from the low 48
// address bits rather than by subtracting full-width tagged pointers.
//
// IR-LABEL: define {{.*}}@varying_inner_stride(
// IR: [[FIRST:%.*]] = ptrtoint ptr %row_base.
// IR: %ptsan.last.int = ptrtoint ptr
// IR: [[FIRST48:%.*]] = trunc i64 [[FIRST]] to i48
// IR: %ptsan.first.low = zext i48 [[FIRST48]] to i64
// IR: %ptsan.last.low = zext i48
// IR: %ptsan.forward.delta = sub i64 %ptsan.last.low, %ptsan.first.low
// IR: %ptsan.backward.delta = sub i64 %ptsan.first.low, %ptsan.last.low
// IR: %ptsan.abs.delta = call i64 @llvm.smax.i64
// IR: %ptsan.loop.size = add i64 %ptsan.abs.delta, 1
// IR: call void @ptsan_complain({{.*}}i64 %ptsan.loop.size)

// GOOD-NOT: Found bad
// BAD: Found bad LOOP access

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

volatile unsigned sink;

__attribute__((noinline))
void varying_inner_stride(uint8_t *base, ptrdiff_t row_stride, unsigned rows,
                          unsigned columns) {
  unsigned column_stride = 1;
  uint8_t *row_base = base;
  for (unsigned row = 0; row < rows;
       ++row, ++column_stride, row_base += row_stride) {
    for (unsigned column = 0; column < columns; ++column) {
      sink += row_base[(ptrdiff_t)column * column_stride];
    }
  }
}

int main(int argc, char **argv) {
  unsigned rows = argc > 1 ? (unsigned)atoi(argv[1]) : 4;
  enum { ALLOCATED_ROWS = 4, ROW_STRIDE = 32, COLUMNS = 4 };
  // The fourth row's last access is 3 * 4 bytes past its start.
  size_t allocation_size = (ALLOCATED_ROWS - 1) * ROW_STRIDE + 13;
  uint8_t *data = (uint8_t *)malloc(allocation_size);
  if (!data) {
    return 2;
  }

  for (size_t i = 0; i < allocation_size; ++i) {
    data[i] = (uint8_t)i;
  }

  varying_inner_stride(data, ROW_STRIDE, rows, COLUMNS);
  free(data);
  return 0;
}
