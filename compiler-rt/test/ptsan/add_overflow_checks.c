// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -mllvm -ptsan-add-overflow-checks=0 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -mllvm -ptsan-add-overflow-checks -S -emit-llvm -o - %s | FileCheck %s --check-prefix=OVERFLOW
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -mllvm -ptsan-no-opts -mllvm -ptsan-add-overflow-checks -S -emit-llvm -o - %s | FileCheck %s --check-prefix=NOOPTS

volatile char Sink;

__attribute__((noinline))
void indexed_access(char *buf, unsigned long idx) {
  char *ptr = buf + idx;
  Sink = *ptr;
}

// DEFAULT-LABEL: define {{.*}}@indexed_access(
// DEFAULT: .szChk
// DEFAULT: .slack
// DEFAULT: .inRange
// DEFAULT-NOT: llvm.uadd.with.overflow
// DEFAULT: ret void

// OVERFLOW-LABEL: define {{.*}}@indexed_access(
// OVERFLOW-NOT: .szChk
// OVERFLOW-NOT: .slack
// OVERFLOW: call {{.*}} @llvm.uadd.with.overflow
// OVERFLOW: extractvalue
// OVERFLOW: extractvalue
// OVERFLOW: .oor
// OVERFLOW: ret void

// NOOPTS-LABEL: define {{.*}}@indexed_access(
// NOOPTS: call {{.*}} @llvm.uadd.with.overflow
// NOOPTS: .oor
// NOOPTS: ret void
