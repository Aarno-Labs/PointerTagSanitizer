//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -O3 -fno-discard-value-names -S -emit-llvm -o raw.ll %s
// RUN: FileCheck %s --check-prefix=RAW < raw.ll
// RUN: %clang_raw -fsanitize=ptsan -g -O3 -fno-discard-value-names \
// RUN:   -mllvm -ptsan-reoptimize -mllvm -ptsan-runtime-validate \
// RUN:   -mllvm -ptsan-stop-after=optimization -S -emit-llvm \
// RUN:   -o optimized.ll %s
// RUN: FileCheck %s --check-prefix=OPT < optimized.ll
// RUN: FileCheck %s --check-prefix=NOACCESS < optimized.ll
// RUN: %clang_raw -fsanitize=ptsan -g -O3 -mllvm -ptsan-reoptimize \
// RUN:   -mllvm -ptsan-runtime-validate -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-stack-temporal-safety=0 -o test %s
// RUN: ./test 2> good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < good.txt
// RUN: not ./test bad 2> bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// RAW-LABEL: define {{.*}} @copy_rows(
// RAW: load <{{[0-9]+}} x i8>, ptr

// OPT-LABEL: define {{.*}} @copy_rows(
// OPT: [[INPUT_BASE:%.*]] = phi ptr
// OPT: call void @llvm.sideeffect() [ "ptsan.loop.preheader"
// OPT-COUNT-2: call void (...) @__ptsan_loop_preanalysis(
// OPT: [[NEXT_GEP:%next.gep[^ ]*]] = getelementptr i8, ptr [[INPUT_BASE]], i64
// OPT: [[STRIPPED_INPUT:%.*]] = call ptr @__ptsan_strip(ptr [[NEXT_GEP]])
// OPT: load <{{[0-9]+}} x i8>, ptr [[STRIPPED_INPUT]]

// NOACCESS-LABEL: define {{.*}} @copy_rows(
// NOACCESS-NOT: call void (...) @__ptsan_access(
// NOACCESS: ret void

// GOOD-NOT: Found bad
// BAD: Found bad {{.*}} access

// TBAA permits the row pointer load to be hoisted and the inner loop to be
// vectorized. After loop optimization, the input loads use a new loop-carried
// pointer PHI as their discovered base. The complete preanalysis summary has
// solved the original input argument instead, so normalized pointer identity
// cannot attribute those loads to an individual solved base. They therefore
// exercise the complete-summary fallback.
//
// The complete-summary fallback retains every marker-specific precheck because
// no single solved base can take responsibility for the transformed accesses.

#define INPUT_SIZE 64
#define OUTPUT_COLUMNS 32
#define ROWS 3
#define GOOD_COLUMNS 17
#define BAD_COLUMNS 22

volatile unsigned preanalysis_covers_all_rows = ROWS;
volatile unsigned preanalysis_covers_all_columns = GOOD_COLUMNS;
volatile unsigned short preanalysis_covers_all_sink;

__attribute__((noinline))
static void copy_rows(unsigned short **output, const unsigned char *input,
                      unsigned rows, unsigned columns) {
  for (unsigned row = 0; row < rows; ++row) {
    for (unsigned column = 0; column < columns; ++column) {
      output[row][column] = *input++;
    }
  }
}

int main(int argc, char **argv) {
  unsigned char input[INPUT_SIZE] = {0};
  unsigned short first[OUTPUT_COLUMNS] = {0};
  unsigned short second[OUTPUT_COLUMNS] = {0};
  unsigned short third[OUTPUT_COLUMNS] = {0};
  unsigned short *output[ROWS] = {first, second, third};
  unsigned columns = preanalysis_covers_all_columns;

  if (argc > 1 && argv[1][0] == 'b') {
    columns = BAD_COLUMNS;
  }

  copy_rows(output, input, preanalysis_covers_all_rows, columns);
  preanalysis_covers_all_sink = output[0][0];
  return 0;
}
