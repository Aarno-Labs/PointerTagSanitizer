// RUN: %clang_raw -fsanitize=ptsan -g -O3 -fno-discard-value-names -mllvm -ptsan-runtime-validate -mllvm -ptsan-print-stats -mllvm -ptsan-stop-after=optimization -S -emit-llvm -o - %s | FileCheck %s --check-prefix=IR --implicit-check-not=SimpleAccessRange
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O3 -fno-discard-value-names -mllvm -ptsan-runtime-validate -mllvm -ptsan-print-stats -o %exe %s %S/../Inputs/preanalysis_negative_delta_globals.c
// RUN: ./%exe 8 2> err && FileCheck %s --check-prefix=OUT < err

// Isolate one mirrored access whose row and column addresses both decrease as
// their induction variables increase. Preanalysis must retain those negative
// affine deltas in a LoopAccessRange, materialize one precheck, and avoid a
// false positive at runtime.

// IR: call void (...) @__ptsan_loop_preanalysis(
// IR: LoopAccessRange

// OUT-NOT: Found bad
// OUT: Checks called:
// OUT: PREANALYZED_LOOP: 1

#include <stdio.h>
#include <stdlib.h>

extern int g[32][32];
extern int g_size[2];
extern volatile int sink;

__attribute__((noinline))
void fill_one_mirrored_store(void) {
  int rows = g_size[0] + 1;
  int cols = g_size[1];

  for (int i = 2; i < (g_size[0] + 3) / 2; i += 2)
    for (int j = 1; j < (g_size[1] / 2) + 1; j += 2)
      g[rows - i][cols - j] = i + j;
}

int main(int argc, char **argv) {
  int size = argc > 1 ? atoi(argv[1]) : 8;
  g_size[0] = size;
  g_size[1] = size;

  fill_one_mirrored_store();
  sink += g[7][7];
  fprintf(stderr, "%d\n", sink);
  return 0;
}
