// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,DEFAULT
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-reoptimize=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=NOGLOBALS
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -mllvm -ptsan-strict-mode -S -emit-llvm -o - %s | FileCheck %s --check-prefix=STRICT

int G[8];
int Scalar;
unsigned char ConstantCopyDst[256];
extern int *GlobalPtr;
extern void consume(unsigned char *);
volatile int Sink;
static const unsigned char SmallConstant[12] =
  {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

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

__attribute__((noinline))
void copy_constant_to_global(void) {
  __builtin_memcpy(ConstantCopyDst, SmallConstant, 256);
  consume(ConstantCopyDst);
}

__attribute__((noinline))
unsigned char read_constant_masked(unsigned i) {
  return SmallConstant[i & 255];
}

// CHECK-LABEL: define {{.*}}@use_stack_unknown(
// CHECK: [[BUF:%[A-Za-z0-9_.]+]] = alloca [20 x i8]
// CHECK: [[BASE:%[A-Za-z0-9_.]+]] = ptrtoint ptr [[BUF]] to i64
// CHECK: call i16 @ptsan_get_stack_ids_1(ptr nonnull [[BUF]], i64 20)
// CHECK: [[TAGGED_BASE:%[A-Za-z0-9_.-]+]] = phi i64
// CHECK: [[ACCESS:%[A-Za-z0-9_.]+]] = ptrtoint ptr {{%[A-Za-z0-9_.]+}} to i64
// CHECK: sub i64 [[ACCESS]], [[TAGGED_BASE]]
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
// NOGLOBALS: lshr i64 ptrtoint (ptr @G to i64), 48
// NOGLOBALS-NOT: @ptsan_bounds
// NOGLOBALS: [[OFFSET:%[A-Za-z0-9_.]+]] = sub i64 {{%[A-Za-z0-9_.]+}}, 65536
// NOGLOBALS: call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 [[OFFSET]], i64 4)
// NOGLOBALS: icmp ugt i64 {{%[A-Za-z0-9_.]+}}, 281474976710655
// NOGLOBALS-NOT: @ptsan_bounds
// NOGLOBALS: call void @ptsan_complain
// NOGLOBALS-NOT: @ptsan_bounds
// NOGLOBALS: ret void

// CHECK-LABEL: define {{.*}}@use_scalar(
// CHECK-NOT: @ptsan_bounds
// CHECK: ptrtoint (ptr @Scalar to i64)
// CHECK-NOT: @ptsan_bounds
// CHECK: call void @ptsan_complain
// CHECK-NOT: @ptsan_bounds
// CHECK: ret void

// CHECK-LABEL: define {{.*}}@use_global_pointer(
// CHECK: [[GLOBALPTR_TRUE:%[A-Za-z0-9_.]+]] = {{.*}}@llvm.ptrmask{{.*}}@GlobalPtr
// CHECK: load ptr, ptr [[GLOBALPTR_TRUE]]
// CHECK: @ptsan_bounds
// CHECK: ret void

// DEFAULT-LABEL: define {{.*}}@copy_constant_to_global(
// DEFAULT-NOT: @ptsan_bounds
// DEFAULT-NOT: call void @ptsan_complain
// DEFAULT: ret void

// DEFAULT-LABEL: define {{.*}}@read_constant_masked(
// DEFAULT-NOT: @ptsan_bounds
// DEFAULT-NOT: call void @ptsan_complain
// DEFAULT: ret

// STRICT-LABEL: define {{.*}}@copy_constant_to_global(
// STRICT: call void @ptsan_complain
// STRICT: ret void
