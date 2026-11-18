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
// RUN: not ./%exe 5 2> bad.err
// RUN: FileCheck %s --check-prefix=BAD < bad.err

// RUN: %clang_ptsan_id32 -O2 \
// RUN:   -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 \
// RUN:   -mllvm -ptsan-reoptimize=0 \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s --check-prefix=IR

// The row pointer and column stride are recurrences of the outer loop.
// LLVM 23 can express the combined extent as an integer offset, avoiding the
// pointer-dependent fallback needed in LLVM 19. Size arithmetic must remain
// independent of the tagged pointer bits; the runtime still checks both the
// valid four-row range and the invalid fifth row.
// IR-LABEL: define {{.*}}@varying_inner_stride(
// IR: [[EXTENT:%.*]] = call i64 @llvm.smax.i64(i64 {{%.*}}, i64 {{%.*}})
// IR-NEXT: [[SIZE:%.*]] = add i64 [[EXTENT]], 1
// IR: call void @ptsan_complain({{.*}}i64 [[SIZE]])

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
