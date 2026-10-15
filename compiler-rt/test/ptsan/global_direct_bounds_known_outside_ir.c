// XFAIL: *
// TODO: This deliberate object overread may fit in the shared constant segment,
// but the compiler does not know that segment's bounds and cannot fold away the
// runtime bounds check.
// RUN: %clang_raw -fsanitize=ptsan -O2 -g -fno-discard-value-names -mllvm -ptsan-runtime-validate -S -emit-llvm -o - %s | FileCheck %s

static const unsigned char tiny[12] = {0, 1, 2, 3, 4, 5,
                                       6, 7, 8, 9, 10, 11};
unsigned char dst[256];

__attribute__((noinline))
void copy_too_much(void) {
  __builtin_memcpy(dst, tiny, sizeof(dst));
}

// CHECK-LABEL: define {{.*}}@copy_too_much(
// CHECK-NOT: @ptsan_bounds
// CHECK-NOT: call void @ptsan_complain
// CHECK: ret void
