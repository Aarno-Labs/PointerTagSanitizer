// RUN: %clang_ptsan -O3 \
// RUN:   -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 \
// RUN:   -mllvm -ptsan-reoptimize=0 \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s --check-prefix=HOIST
// RUN: %clang_ptsan -O3 \
// RUN:   -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 \
// RUN:   -mllvm -ptsan-reoptimize \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s --check-prefix=REOPT
// RUN: %clang_ptsan -O3 \
// RUN:   -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 \
// RUN:   -mllvm -ptsan-no-loop-check-hoist \
// RUN:   -mllvm -ptsan-reoptimize=0 \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s --check-prefix=NOHOIST
// RUN: %clang_ptsan -O3 \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 \
// RUN:   -mllvm -ptsan-reoptimize=0 \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s --check-prefix=FALLBACK

// Extracted from the 525.x264_r common/mc.c motion-compensation shape.  The
// loop-hoist path currently materializes a runtime-direction whole-loop range
// for tiny block loops; the no-hoist path keeps simple point checks.
//
// HOIST: c"LOOP\00"
// HOIST-LABEL: define {{.*}}@x264_tiny_mc_like(
// HOIST-NOT: mul i48
// HOIST: ptrtoint ptr
// HOIST-NOT: mul i48
// HOIST-NOT: ptsan.first.low
// HOIST: call i64 @llvm.smax.i64
// HOIST-NOT: mul i48
// HOIST-NOT: ptsan.first.low
// HOIST: call void @ptsan_complain
// HOIST-NOT: mul i48
// HOIST-NOT: ptsan.first.low
// HOIST: ret void
//
// REOPT: c"LOOP\00"
// REOPT-LABEL: define {{.*}}@x264_tiny_mc_like(
// REOPT-NOT: ptsan.first.low
// REOPT: [[SRC_SMAX:%[^ ]+]] = tail call i64 @llvm.smax.i64
// REOPT-NEXT: [[SRC_SIZE:%[^ ]+]] = add nsw i64 [[SRC_SMAX]], 1
// REOPT: [[DST_SMAX:%[^ ]+]] = tail call i64 @llvm.smax.i64
// REOPT-NEXT: [[DST_SIZE:%[^ ]+]] = add nsw i64 [[DST_SMAX]], 1
// REOPT: %src1.end.ov = tail call {{.*}} i64 [[SRC_SIZE]])
// REOPT: %src2.end.ov = tail call {{.*}} i64 [[SRC_SIZE]])
// REOPT: %dst.end.ov = tail call {{.*}} i64 [[DST_SIZE]])
// REOPT-NOT: ptsan.first.low
// REOPT: ret void
//
// NOHOIST: c"POINT\00"
// NOHOIST-LABEL: define {{.*}}@x264_tiny_mc_like(
// NOHOIST-NOT: call i64 @llvm.smax.i64
// NOHOIST: call void @ptsan_complain
// NOHOIST-NOT: call i64 @llvm.smax.i64
// NOHOIST: ret void
//
// This vectorized x264 shape leaves a pointer-typed SCEV dependency in the
// derived byte delta. It must keep the low-address-bit fallback rather than
// materializing full-width ptrtoint arithmetic.
// FALLBACK: c"LOOP\00"
// FALLBACK-LABEL: define {{.*}}@mc_weight_w20_like(
// FALLBACK: ptsan.first.low
// FALLBACK: ptsan.last.low
// FALLBACK: ptsan.loop.size
// FALLBACK: call void @ptsan_complain
// FALLBACK: ret void

#include <stdint.h>
#include <stdlib.h>

volatile uint8_t sink;

static inline uint8_t clip_uint8(int x) {
  if (x & ~255)
    return (-x) >> 31;
  return x;
}

__attribute__((noinline)) void
x264_tiny_mc_like(uint8_t *dst, int dst_stride, uint8_t *src1, uint8_t *src2,
                  int src_stride, int width, int height, int scale,
                  int offset) {
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++)
      dst[x] = (uint8_t)(src1[x] + src2[x] + scale + offset);

    dst += dst_stride;
    src1 += src_stride;
    src2 += src_stride;
  }
}

typedef struct {
  int i_scale;
  int i_denom;
  int i_offset;
} weight_t;

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

int main(int argc, char **argv) {
  uint8_t dst[16 * 16] = {0};
  uint8_t src1[16 * 16] = {0};
  uint8_t src2[16 * 16] = {0};
  int iters = argc > 1 ? atoi(argv[1]) : 300000000;
  int width = argc > 2 ? atoi(argv[2]) : 1;
  int height = argc > 3 ? atoi(argv[3]) : 1;

  for (int i = 0; i < 16 * 16; i++) {
    src1[i] = (uint8_t)i;
    src2[i] = (uint8_t)(255 - i);
  }

  for (int i = 0; i < iters; i++) {
    x264_tiny_mc_like(dst, 16, src1, src2, 16, width, height, 3 + (i & 1), 1);
    sink = dst[(unsigned)i & 255];
  }
  return 0;
}
