// Constant-segment folding expectations live separately in
// constant_global_bounds_folding_xfail.c so they cannot mask regressions here.
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefix=CHECK
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-reoptimize=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=NOGLOBALS

int G[8];
int Scalar;
extern int *GlobalPtr;
volatile int Sink;

__attribute__((noinline))
void use_stack_unknown(int i) {
  char buf[20];
  Sink = buf[i];
}

__attribute__((noinline))
void use_stack_masked(int i) {
  char buf[20];
  int j = i & 15;
  Sink = buf[j];
}

__attribute__((noinline))
void use_global(int i) {
  Sink = G[i];
}

__attribute__((noinline))
void use_scalar(int i) {
  char *p = (char *)&Scalar;
  Sink = *(int *)(p + i);
}

__attribute__((noinline))
void use_global_pointer(int i) {
  Sink = GlobalPtr[i];
}

// CHECK-LABEL: define {{.*}}@use_stack_unknown(
// CHECK: [[BUF:%[A-Za-z0-9_.]+]] = alloca [20 x i8]
// CHECK: [[BASE:%[A-Za-z0-9_.]+]] = ptrtoint ptr [[BUF]] to i64
// CHECK: call i16 @ptsan_get_stack_ids_1(ptr nonnull [[BUF]], i64 20)
// CHECK: [[ID:%[A-Za-z0-9_.-]+]] = phi i16
// CHECK: icmp ugt i32 %i, 19
// CHECK: call void @ptsan_complain
// CHECK: ret void

// CHECK-LABEL: define {{.*}}@use_stack_masked(
// CHECK-NOT: call void @ptsan_complain
// CHECK: ret void

// CHECK-LABEL: define {{.*}}@use_global(
// CHECK: [[TAGGED:%[A-Za-z0-9_.]+]] = load ptr, ptr @G.ptsan.ptr
// CHECK-NOT: @ptsan_bounds
// CHECK: icmp ugt i64 {{%[A-Za-z0-9_.]+}}, 28
// CHECK-NOT: @ptsan_bounds
// CHECK: ptrtoint ptr [[TAGGED]] to i64
// CHECK-NOT: @ptsan_bounds
// CHECK: call void @ptsan_complain
// CHECK-NOT: @ptsan_bounds
// CHECK: ret void

// NOGLOBALS-LABEL: define {{.*}}@use_global(
// NOGLOBALS-NOT: @G.ptsan.ptr
// NOGLOBALS-NOT: lshr
// NOGLOBALS-NOT: @ptsan_bounds
// NOGLOBALS: [[OFFSET:%[A-Za-z0-9_.]+]] = sub i64 {{%[A-Za-z0-9_.]+}}, 65536
// NOGLOBALS: call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 [[OFFSET]], i64 4)
// NOGLOBALS: icmp ugt i64 {{%[A-Za-z0-9_.]+}}, 281474976710655
// NOGLOBALS-NOT: @ptsan_bounds
// NOGLOBALS: call void @ptsan_complain({{.*}}i64 0,
// NOGLOBALS-NOT: @ptsan_bounds
// NOGLOBALS: ret void

// CHECK-LABEL: define {{.*}}@use_scalar(
// CHECK-NOT: @ptsan_bounds
// CHECK: [[SCALAR:%[A-Za-z0-9_.]+]] = load ptr, ptr @Scalar.ptsan.ptr
// CHECK: icmp eq i32 %i, 0
// CHECK: ptrtoint ptr [[SCALAR]] to i64
// CHECK-NOT: @ptsan_bounds
// CHECK: call void @ptsan_complain
// CHECK-NOT: @ptsan_bounds
// CHECK: ret void

// CHECK-LABEL: define {{.*}}@use_global_pointer(
// CHECK-NOT: @llvm.ptrmask
// CHECK: load ptr, ptr @GlobalPtr
// CHECK: @ptsan_bounds
// CHECK: ret void
