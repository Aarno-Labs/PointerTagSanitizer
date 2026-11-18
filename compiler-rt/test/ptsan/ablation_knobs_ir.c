// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -mllvm -ptsan-reoptimize=0 -mllvm -ptsan-preanalyze-loops=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=LOOPHOIST
// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -mllvm -ptsan-reoptimize=0 -mllvm -ptsan-preanalyze-loops=0 -mllvm -ptsan-no-loop-check-hoist -S -emit-llvm -o - %s | FileCheck %s --check-prefix=NOLOOPHOIST
// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -mllvm -ptsan-reoptimize=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=MERGE
// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -mllvm -ptsan-reoptimize=0 -mllvm -ptsan-no-check-merge -S -emit-llvm -o - %s | FileCheck %s --check-prefix=NOMERGE
// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -mllvm -ptsan-reoptimize=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=FILTER
// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -mllvm -ptsan-reoptimize=0 -mllvm -ptsan-no-filter-removable-checks -S -emit-llvm -o - %s | FileCheck %s --check-prefix=NOFILTER
// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -mllvm -ptsan-reoptimize=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefixes=STACKDEFAULT,STACK-DEFAULT
// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -mllvm -ptsan-reoptimize=0 -mllvm -ptsan-no-stack-id-fast-path -S -emit-llvm -o - %s | FileCheck %s --check-prefix=NOSTACK
// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names -mllvm -ptsan-reoptimize=0 -mllvm -ptsan-no-opts -S -emit-llvm -o - %s | FileCheck %s --check-prefix=NOOPTSSTACK

// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O1 -g -fno-discard-value-names -mllvm -ptsan-reoptimize=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefixes=STACKDEFAULT,STACK-ID32

volatile char Sink;
static const char ConstGlobal[16] = "abcdefghijklmn";

__attribute__((noinline))
void loop_access(char *buf, int n) {
  volatile char *v = buf;
  for (int i = 0; i < n; i++)
    v[i] = 1;
}

// LOOPHOIST-LABEL: define {{.*}}@loop_access(
// LOOPHOIST: for.body.preheader:
// LOOPHOIST: call void @ptsan_complain
// LOOPHOIST: for.body:

// NOLOOPHOIST-LABEL: define {{.*}}@loop_access(
// NOLOOPHOIST: for.body.preheader:
// NOLOOPHOIST-NOT: call void @ptsan_complain
// NOLOOPHOIST: for.body:
// NOLOOPHOIST: call void @ptsan_complain

__attribute__((noinline))
void merge_accesses(char *buf, int idx) {
  Sink = buf[idx];
  Sink = buf[idx + 5];
}

// MERGE-LABEL: define {{.*}}@merge_accesses(
// MERGE: call void @ptsan_complain
// MERGE-NOT: call void @ptsan_complain
// MERGE: ret void

// NOMERGE-LABEL: define {{.*}}@merge_accesses(
// NOMERGE: call void @ptsan_complain
// NOMERGE: call void @ptsan_complain
// NOMERGE-NOT: call void @ptsan_complain
// NOMERGE: ret void

__attribute__((noinline))
void removable_stack(int idx) {
  volatile char buf[20] = {0};
  int j = idx & 15;
  Sink = buf[j];
}

__attribute__((noinline))
void removable_const_global(int idx) {
  int j = idx & 15;
  Sink = ConstGlobal[j];
}

// FILTER-LABEL: define {{.*}}@removable_stack(
// FILTER-NOT: call void @ptsan_complain
// FILTER: ret void

// NOFILTER-LABEL: define {{.*}}@removable_stack(
// NOFILTER: call void @ptsan_complain
// NOFILTER: ret void

// FILTER-LABEL: define {{.*}}@removable_const_global(
// FILTER-NOT: call void @ptsan_complain
// FILTER: ret void

// NOFILTER-LABEL: define {{.*}}@removable_const_global(
// NOFILTER: call void @ptsan_complain
// NOFILTER: ret void

__attribute__((noinline))
void one_stack(int i) {
  volatile char buf[20] = {0};
  Sink = buf[i];
}

// STACKDEFAULT-LABEL: define {{.*}}@one_stack(
// STACK-DEFAULT: has.stack.id.room
// STACK-DEFAULT: call void @ptsan_get_stack_ids
// STACK-ID32: has.stack.id.room
// STACK-ID32: call void @ptsan_get_stack_ids

// NOSTACK-LABEL: define {{.*}}@one_stack(
// NOSTACK-NOT: has.stack.id.room
// NOSTACK: call void @ptsan_get_stack_ids
// NOSTACK: call void @ptsan_free_stack_ids
// NOSTACK-NOT: has.stack.id.room
// NOSTACK: ret void

// NOOPTSSTACK-LABEL: define {{.*}}@one_stack(
// NOOPTSSTACK-NOT: has.stack.id.room
// NOOPTSSTACK: call void @ptsan_get_stack_ids
// NOOPTSSTACK: call void @ptsan_free_stack_ids
// NOOPTSSTACK-NOT: has.stack.id.room
// NOOPTSSTACK: ret void
