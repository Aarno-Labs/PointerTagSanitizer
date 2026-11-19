// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,DEFAULT
// RUN: %clang_ptsan -O1 -g -o %t %s %S/Inputs/extern_global_direct_bounds_def.c
// RUN: %t

// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O1 -g -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,ID32

extern int G[16];
extern int Incomplete[];
extern int *GlobalPtr;
volatile int Sink;

__attribute__((noinline))
void use_extern_global(int i) {
  Sink = G[i];
}

__attribute__((noinline))
void use_incomplete_extern_global(int i) {
  Sink = Incomplete[i];
}

__attribute__((noinline))
void use_extern_global_pointer(int i) {
  Sink = GlobalPtr[i];
}

int main(void) {
  use_extern_global(3);
  use_incomplete_extern_global(3);
  use_extern_global_pointer(3);
  return 0;
}

// CHECK-LABEL: define {{.*}}@use_extern_global(
// CHECK-NOT: @ptsan_bounds
// CHECK-DAG: [[TAGGED:%[A-Za-z0-9_.]+]] = load ptr, ptr @G.ptsan.ptr
// CHECK-DAG: icmp ugt i64 {{%[A-Za-z0-9_.]+}}, 60
// CHECK-NOT: @ptsan_bounds
// CHECK: ptrtoint ptr [[TAGGED]] to i64
// CHECK-NOT: @ptsan_bounds
// CHECK: call void @ptsan_complain
// CHECK-NOT: @ptsan_bounds
// CHECK: ret void

// CHECK-LABEL: define {{.*}}@use_incomplete_extern_global(
// CHECK: [[TAGGED:%[A-Za-z0-9_.]+]] = load ptr, ptr @Incomplete.ptsan.ptr
// CHECK: @ptsan_bounds
// DEFAULT: getelementptr i8, ptr [[TAGGED]]
// DEFAULT: ptrtoint ptr {{%[A-Za-z0-9_.]+}} to i64
// ID32: add i64 {{%.*}}, {{%.*}}
// CHECK: call void @ptsan_complain
// CHECK: ret void

// CHECK-LABEL: define {{.*}}@use_extern_global_pointer(
// CHECK-NOT: @llvm.ptrmask
// CHECK: load ptr, ptr @GlobalPtr
// CHECK: @ptsan_bounds
// CHECK: ret void
