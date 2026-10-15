// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -O3 -g -fno-discard-value-names -fno-unsafe-math-optimizations -fcommon -mllvm -ptsan-reoptimize -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-preanalyze-loops=0 -mllvm -ptsan-runtime-validate -mllvm -ptsan-print-stats -mllvm -ptsan-abort-on-error=0 -o loop-hoist %s
// RUN: %clang_raw -fsanitize=ptsan -O3 -g -fno-discard-value-names -fno-unsafe-math-optimizations -fcommon -mllvm -ptsan-reoptimize -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-preanalyze-loops=0 -mllvm -ptsan-no-loop-check-hoist -mllvm -ptsan-runtime-validate -mllvm -ptsan-print-stats -mllvm -ptsan-abort-on-error=0 -o no-loop-hoist %s
// RUN: ./loop-hoist 4 1 2> loop.good.err && FileCheck %s --check-prefixes=GOOD,FAST < loop.good.err
// RUN: ./loop-hoist 5 1 2> loop.bad.err && FileCheck %s --check-prefixes=BAD,FAST < loop.bad.err
// RUN: ./loop-hoist 4 0 2> loop.noden.good.err && FileCheck %s --check-prefixes=GOOD,FAST < loop.noden.good.err
// RUN: ./loop-hoist 5 0 2> loop.noden.bad.err && FileCheck %s --check-prefixes=BAD,FAST < loop.noden.bad.err
// RUN: ./no-loop-hoist 4 1 2> no-loop.good.err && FileCheck %s --check-prefixes=GOOD,SLOWGOOD < no-loop.good.err
// RUN: ./no-loop-hoist 5 1 2> no-loop.bad.err && FileCheck %s --check-prefixes=BAD,SLOWBAD < no-loop.bad.err
// RUN: ./no-loop-hoist 4 0 2> no-loop.noden.good.err && FileCheck %s --check-prefixes=GOOD,SLOWGOOD < no-loop.noden.good.err
// RUN: ./no-loop-hoist 5 0 2> no-loop.noden.bad.err && FileCheck %s --check-prefixes=BAD,SLOWBAD < no-loop.noden.bad.err

// GOOD-NOT: Found bad
// BAD: Found bad COMPOUND access
// FAST: Checks called:
// FAST: POINT: 2
// FAST: LOOP: 0
// FAST: PREANALYZED_LOOP: 0
// FAST: COMPOUND: 6
// FAST: Total checks called: 8
// SLOWGOOD: Checks called:
// SLOWGOOD: POINT: 2
// SLOWGOOD: LOOP: 0
// SLOWGOOD: PREANALYZED_LOOP: 0
// SLOWGOOD: COMPOUND: 12
// SLOWGOOD: Total checks called: 14
// SLOWBAD: Checks called:
// SLOWBAD: POINT: 2
// SLOWBAD: LOOP: 0
// SLOWBAD: PREANALYZED_LOOP: 0
// SLOWBAD: COMPOUND: 14
// SLOWBAD: Total checks called: 16

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

volatile uint8_t sink;

typedef struct {
  int i_scale;
  int i_denom;
  int i_offset;
} x264_weight_t;

static inline uint8_t x264_clip_uint8(int x) {
  if (x & ~255)
    return (-x) >> 31;
  return x;
}

#define opscale(x)                                                            \
  dst[x] = x264_clip_uint8(((src[x] * weight->i_scale +                      \
                             (1 << (weight->i_denom - 1))) >>                \
                            weight->i_denom) +                                \
                           weight->i_offset)

#define opscale_noden(x)                                                      \
  dst[x] = x264_clip_uint8(src[x] * weight->i_scale + weight->i_offset)

void mc_weight_w16_min(uint8_t *dst, int i_dst_stride, uint8_t *src,
                       int i_src_stride, const x264_weight_t *weight,
                       int height) {
  if (weight->i_denom >= 1) {
    for (int y = 0; y < height; y++, dst += i_dst_stride, src += i_src_stride)
      for (int x = 0; x < 16; x++)
        opscale(x);
  } else {
    for (int y = 0; y < height; y++, dst += i_dst_stride, src += i_src_stride)
      for (int x = 0; x < 16; x++)
        opscale_noden(x);
  }
}

int main(int argc, char **argv) {
  int height = argc > 1 ? atoi(argv[1]) : 4;
  int denom = argc > 2 ? atoi(argv[2]) : 1;
  const int allocated_height = 4;
  const int stride = 16;
  const size_t size = (size_t)allocated_height * stride;

  uint8_t *dst = (uint8_t *)malloc(size);
  uint8_t *src = (uint8_t *)malloc(size);
  if (!dst || !src) {
    fprintf(stderr, "alloc failed\n");
    return 1;
  }

  for (size_t i = 0; i < size; ++i) {
    dst[i] = 0;
    src[i] = (uint8_t)i;
  }

  x264_weight_t weight = {
      .i_scale = 3,
      .i_denom = denom,
      .i_offset = 1,
  };

  mc_weight_w16_min(dst, stride, src, stride, &weight, height);
  sink = dst[0];
  return 0;
}
