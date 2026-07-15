// RUN: %clang_raw -fsanitize=ptsan -O1 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang_raw -fsanitize=ptsan -O1 -S -emit-llvm -mllvm -ptsan-stack-ids-per-bucket=7 -mllvm -ptsan-stack-id-thread-buckets=3 -o - %s | FileCheck %s --check-prefix=CUSTOM

// DEFAULT-DAG: @ClStackIDsPerBucket = weak_odr {{.*}}constant i32 512
// DEFAULT-DAG: @ClStackIDThreadBuckets = weak_odr {{.*}}constant i32 12

// CUSTOM-DAG: @ClStackIDsPerBucket = weak_odr {{.*}}constant i32 7
// CUSTOM-DAG: @ClStackIDThreadBuckets = weak_odr {{.*}}constant i32 3

int use_stack_id_bucket_flags(void) {
  int x = 1;
  return x;
}
