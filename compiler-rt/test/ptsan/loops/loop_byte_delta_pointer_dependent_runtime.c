// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -O3 \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 \
// RUN:   -mllvm -ptsan-reoptimize=0 \
// RUN:   -o %exe %s
// RUN: ./%exe 2> err
// RUN: FileCheck %s --check-prefix=OUT < err

// OUT-NOT: Found bad
// OUT: Checks called:
// OUT: COMPOUND: {{[1-9][0-9]*}}

// Runtime smoke test for the x264-style vectorized loop shape covered more
// precisely by x264_loop_hoist_profitability.c.  Its inner-loop endpoints are
// rewritten in terms of the current outer-loop pointer, exposing an integer
// byte delta.  The runtime may merge the loop checks into COMPOUND checks after
// instrumentation.

#include <stdint.h>
#include <stdlib.h>

volatile uint8_t sink;

typedef struct {
  int i_scale;
  int i_denom;
  int i_offset;
} weight_t;

static inline uint8_t clip_uint8(int x) {
  if (x & ~255)
    return (-x) >> 31;
  return x;
}

__attribute__((noinline)) void
mc_weight_w20_like(uint8_t *dst, int dst_stride, uint8_t *src, int src_stride,
                   const weight_t *weight, int height) {
  if (weight->i_denom >= 1) {
    for (int y = 0; y < height; y++, dst += dst_stride, src += src_stride)
      for (int x = 0; x < 20; x++)
        dst[x] = clip_uint8(((src[x] * weight->i_scale +
                              (1 << (weight->i_denom - 1))) >>
                             weight->i_denom) +
                            weight->i_offset);
  } else {
    for (int y = 0; y < height; y++, dst += dst_stride, src += src_stride)
      for (int x = 0; x < 20; x++)
        dst[x] = clip_uint8(src[x] * weight->i_scale + weight->i_offset);
  }
}

int main(void) {
  enum { HEIGHT = 8, STRIDE = 32 };
  uint8_t *dst = (uint8_t *)malloc(HEIGHT * STRIDE);
  uint8_t *src = (uint8_t *)malloc(HEIGHT * STRIDE);
  weight_t weight = {3, 2, 7};

  if (!dst || !src)
    return 2;

  for (int i = 0; i < HEIGHT * STRIDE; ++i) {
    dst[i] = 0;
    src[i] = (uint8_t)i;
  }

  mc_weight_w20_like(dst, STRIDE, src, STRIDE, &weight, HEIGHT);
  sink = dst[(HEIGHT - 1) * STRIDE + 19];

  int ret = sink == 189 ? 0 : 1;
  free(src);
  free(dst);
  return ret;
}
