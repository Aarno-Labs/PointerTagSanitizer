// REQUIRES: x86_64-target-arch
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefix=NORMAL
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -mllvm -ptsan-lam -S -emit-llvm -o - %s | FileCheck %s --check-prefix=LAM --implicit-check-not='asm "andq'

#include <stdatomic.h>

extern void escape(int *);

int __attribute__((noinline)) direct_ops(int *p, int *q) {
  int x = *p;
  *q = x + 1;
  atomic_fetch_add_explicit((_Atomic int *)q, 1, memory_order_relaxed);
  int expected = x + 2;
  atomic_compare_exchange_strong_explicit((_Atomic int *)q, &expected, x,
                                          memory_order_relaxed,
                                          memory_order_relaxed);
  escape(p);
  return *q;
}

// NORMAL-LABEL: define {{.*}} @direct_ops(
// NORMAL-DAG: [[QINT:%[^ ]+]] = ptrtoint ptr %q to i64
// NORMAL-DAG: [[QBITS:%[^ ]+]] = {{.*}}call i64 asm "andq $2, $0", "=r,0,r,~{flags}"(i64 [[QINT]], i64 281474976710655)
// NORMAL-DAG: [[QTRUE:%[^ ]+]] = inttoptr i64 [[QBITS]] to ptr
// NORMAL-DAG: [[PINT:%[^ ]+]] = ptrtoint ptr %p to i64
// NORMAL-DAG: [[PBITS:%[^ ]+]] = {{.*}}call i64 asm "andq $2, $0", "=r,0,r,~{flags}"(i64 [[PINT]], i64 281474976710655)
// NORMAL-DAG: [[PTRUE:%[^ ]+]] = inttoptr i64 [[PBITS]] to ptr
// NORMAL: load i32, ptr [[PTRUE]]
// NORMAL: store i32 {{.*}}, ptr [[QTRUE]]
// NORMAL: atomicrmw add ptr [[QTRUE]]
// NORMAL: cmpxchg ptr [[QTRUE]]
// NORMAL: load i32, ptr [[QTRUE]]

// LAM-LABEL: define {{.*}} @direct_ops(
// LAM-SAME: ptr {{[^%]*}}[[P:%[A-Za-z0-9_.]+]], ptr {{[^%]*}}[[Q:%[A-Za-z0-9_.]+]]
// LAM-NOT: @llvm.ptrmask
// LAM: load i32, ptr [[P]]
// LAM-NOT: @llvm.ptrmask
// LAM: store i32 {{.*}}, ptr [[Q]]
// LAM-NOT: @llvm.ptrmask
// LAM: atomicrmw add ptr [[Q]]
// LAM-NOT: @llvm.ptrmask
// LAM: cmpxchg ptr [[Q]]
// LAM-NOT: @llvm.ptrmask
// LAM: load i32, ptr [[Q]]
// LAM-NOT: @llvm.ptrmask
// LAM: ret i32
