// Configuration errors are diagnosed without needing a LAM-capable machine.
// RUN: %clang_raw -fsanitize=ptsan -O1 -S -emit-llvm -fsanitize-ptsan-lam -mllvm -ptsan-id-bucket-size=32768 -mllvm -ptsan-stack-id-thread-buckets=1 -o /dev/null %s
// RUN: not %clang_raw -fsanitize=ptsan -O1 -S -emit-llvm -fsanitize-ptsan-lam -mllvm -ptsan-id-bucket-size=32768 -mllvm -ptsan-stack-id-thread-buckets=2 -o - %s 2>&1 | FileCheck %s

int main(void) {
  return 0;
}

// CHECK: PTSan reserved stack ID buckets exceed ID space
