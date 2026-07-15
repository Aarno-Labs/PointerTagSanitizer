// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefix=STRONG
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -mllvm -ptsan-stack-temporal-safety=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=WEAK

extern void escape(char *);

__attribute__((noinline))
void one_stack_id(void) {
  char buf[17];
  escape(buf);
}

// STRONG-LABEL: define {{.*}}@one_stack_id(
// STRONG: pending.frees
// STRONG: pending.frees.inc
// STRONG: call void @llvm.memset
// STRONG: ret void

// WEAK-LABEL: define {{.*}}@one_stack_id(
// WEAK-NOT: pending.frees
// WEAK-NOT: ptsan_pending_stack_ids
// WEAK-NOT: @llvm.memset
// WEAK: store i16 {{.*}}, ptr @ptsan_next_stack_id
// WEAK: ret void
