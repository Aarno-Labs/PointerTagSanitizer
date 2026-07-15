// RUN: %clang_raw -fsanitize=ptsan -O3 -g -fno-discard-value-names -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-preanalyze-loops=0 -mllvm -ptsan-no-loop-check-hoist -mllvm -ptsan-reoptimize=0 -mllvm -ptsan-no-filter-removable-checks -S -emit-llvm -o - %s | FileCheck %s --check-prefix=UNFILTERED
// RUN: %clang_raw -fsanitize=ptsan -O3 -g -fno-discard-value-names -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-preanalyze-loops=0 -mllvm -ptsan-no-loop-check-hoist -mllvm -ptsan-reoptimize=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=FILTERED
// RUN: %clang_raw -fsanitize=ptsan -O3 -g -fno-discard-value-names -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-preanalyze-loops=1 -mllvm -ptsan-no-loop-check-hoist -mllvm -ptsan-reoptimize=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=PREONLY
// RUN: %clang_raw -fsanitize=ptsan -O3 -g -fno-discard-value-names -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-preanalyze-loops=1 -mllvm -ptsan-reoptimize=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=BOTH

// Extracted from 525.x264_r x264_src/encoder/rdo.c:x264_rdo_init.
// Without removable-check filtering this produces many global bounds checks;
// filtering should prove and remove them before postoptimization.
//
// UNFILTERED-LABEL: define {{.*}}@x264_rdo_init(
// UNFILTERED: call void @ptsan_complain
// UNFILTERED: ret void
//
// FILTERED-LABEL: define {{.*}}@x264_rdo_init(
// FILTERED-NOT: call void @ptsan_complain
// FILTERED: ret void
//
// PREONLY-NOT: c"PREANALYZED_LOOP
// PREONLY-LABEL: define {{.*}}@x264_rdo_init(
// PREONLY-NOT: call void @ptsan_complain
// PREONLY: ret void
//
// BOTH-NOT: c"PREANALYZED_LOOP
// BOTH-LABEL: define {{.*}}@x264_rdo_init(
// BOTH-NOT: call void @ptsan_complain
// BOTH: ret void

#include <stdint.h>

extern const uint8_t x264_cabac_transition[128][2];
extern const uint16_t x264_cabac_entropy[128];

uint8_t cabac_transition_unary[15][128];
uint16_t cabac_size_unary[15][128];
uint8_t cabac_transition_5ones[128];
uint16_t cabac_size_5ones[128];

static inline int x264_cabac_size_decision2(uint8_t *state, long b) {
  int i_state = *state;
  *state = x264_cabac_transition[i_state][b];
  return x264_cabac_entropy[i_state ^ b];
}

void x264_rdo_init(void) {
  for (int i_prefix = 0; i_prefix < 15; i_prefix++) {
    for (int i_ctx = 0; i_ctx < 128; i_ctx++) {
      int f8_bits = 0;
      uint8_t ctx = i_ctx;

      for (int i = 1; i < i_prefix; i++)
        f8_bits += x264_cabac_size_decision2(&ctx, 1);
      if (i_prefix > 0 && i_prefix < 14)
        f8_bits += x264_cabac_size_decision2(&ctx, 0);
      f8_bits += 1 << 8;

      cabac_size_unary[i_prefix][i_ctx] = f8_bits;
      cabac_transition_unary[i_prefix][i_ctx] = ctx;
    }
  }

  for (int i_ctx = 0; i_ctx < 128; i_ctx++) {
    int f8_bits = 0;
    uint8_t ctx = i_ctx;

    for (int i = 0; i < 5; i++)
      f8_bits += x264_cabac_size_decision2(&ctx, 1);
    f8_bits += 1 << 8;

    cabac_size_5ones[i_ctx] = f8_bits;
    cabac_transition_5ones[i_ctx] = ctx;
  }
}
