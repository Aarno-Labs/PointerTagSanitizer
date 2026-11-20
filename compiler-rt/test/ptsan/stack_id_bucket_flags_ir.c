// RUN: %clang_ptsan_plain -O1 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang_ptsan_plain -O1 -S -emit-llvm -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=3 -o - %s | FileCheck %s --check-prefix=CUSTOM
// RUN: not %clang_ptsan_plain -O1 -S -emit-llvm -mllvm -ptsan-id-bucket-size=0 -o - %s 2>&1 | FileCheck %s --check-prefix=BAD-SIZE
// RUN: not %clang_ptsan_plain -O1 -S -emit-llvm -mllvm -ptsan-id-bucket-size=65 -o - %s 2>&1 | FileCheck %s --check-prefix=BAD-SIZE
// RUN: not %clang_ptsan_plain -O1 -S -emit-llvm -mllvm -ptsan-id-bucket-size=1 -mllvm -ptsan-stack-id-thread-buckets=0 -o - %s 2>&1 | FileCheck %s --check-prefix=BAD-SIZE
// RUN: not %clang_ptsan_plain -O1 -S -emit-llvm -mllvm -ptsan-id-bucket-size=32 -o - %s 2>&1 | FileCheck %s --check-prefix=BAD-SIZE
// RUN: not %clang_ptsan_plain -O1 -S -emit-llvm -mllvm -ptsan-id-bucket-size=63 -o - %s 2>&1 | FileCheck %s --check-prefix=BAD-SIZE
// RUN: not %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -O1 -S -emit-llvm -mllvm -ptsan-id-bucket-size=32 -o - %s 2>&1 | FileCheck %s --check-prefix=BAD-SIZE

// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O1 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=ID32
// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -O1 -S -emit-llvm -mllvm -ptsan-id-bucket-size=512 -mllvm -ptsan-stack-id-thread-buckets=12 -o - %s | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -O1 -S -emit-llvm -mllvm -ptsan-stack-id-thread-buckets=0 -o - %s | FileCheck %s --check-prefix=ZERO

// Every bucket can be reserved; only requests beyond the namespace fail.
// RUN: not %clang_ptsan_plain -O1 -S -emit-llvm -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=1025 -o - %s 2>&1 | FileCheck %s --check-prefix=BAD-RANGE
// RUN: %clang_ptsan_plain -O1 -S -emit-llvm -mllvm -ptsan-id-bucket-size=32768 -mllvm -ptsan-stack-id-thread-buckets=1 -o /dev/null %s
// RUN: not %clang_ptsan_plain -O1 -S -emit-llvm -mllvm -ptsan-stack-id-thread-buckets=129 -o - %s 2>&1 | FileCheck %s --check-prefix=BAD-RANGE
// RUN: %clang_ptsan_plain -O1 -S -emit-llvm -mllvm -ptsan-stack-id-thread-buckets=128 -o - %s | FileCheck %s --check-prefix=MAX-ID16
// RUN: not %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -O1 -S -emit-llvm -mllvm -ptsan-stack-id-thread-buckets=32769 -o - %s 2>&1 | FileCheck %s --check-prefix=BAD-RANGE
// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -O1 -S -emit-llvm -mllvm -ptsan-stack-id-thread-buckets=32768 -o - %s | FileCheck %s --check-prefix=MAX-ID32
// RUN: not %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -O1 -S -emit-llvm -mllvm -ptsan-strict-mode -mllvm -ptsan-stack-id-thread-buckets=65537 -o - %s 2>&1 | FileCheck %s --check-prefix=BAD-RANGE
// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -O1 -S -emit-llvm -mllvm -ptsan-strict-mode -mllvm -ptsan-stack-id-thread-buckets=65536 -o - %s | FileCheck %s --check-prefix=MAX-STRICT-ID32
// RUN: not %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -O1 -S -emit-llvm -mllvm -ptsan-strict-mode -mllvm -ptsan-support-indicator-bit -mllvm -ptsan-stack-id-thread-buckets=32769 -o - %s 2>&1 | FileCheck %s --check-prefix=BAD-RANGE
// RUN: %clang_ptsan_plain -O1 -S -emit-llvm -mllvm -ptsan-id-bucket-size=32768 -mllvm -ptsan-stack-id-thread-buckets=0 -o - %s | FileCheck %s --check-prefix=WIDE-ZERO

// Explicit sizes override mode defaults; disabling reservations keeps the size.

// DEFAULT-DAG: @ClIDBucketSize = weak_odr {{.*}}constant i32 512
// DEFAULT-DAG: @ClStackIDThreadBuckets = weak_odr {{.*}}constant i32 12
// ID32-DAG: @ClIDBucketSize = weak_odr {{.*}}constant i32 65536
// ID32-DAG: @ClStackIDThreadBuckets = weak_odr {{.*}}constant i32 128

// CUSTOM-DAG: @ClIDBucketSize = weak_odr {{.*}}constant i32 64
// CUSTOM-DAG: @ClStackIDThreadBuckets = weak_odr {{.*}}constant i32 3
// ZERO-DAG: @ClIDBucketSize = weak_odr {{.*}}constant i32 65536
// ZERO-DAG: @ClStackIDThreadBuckets = weak_odr {{.*}}constant i32 0
// BAD-SIZE: PTSan ID bucket size must be a power of two and at least 64
// BAD-RANGE: PTSan reserved stack ID buckets exceed ID space
// MAX-ID16: @ClStackIDThreadBuckets = weak_odr {{.*}}constant i32 128
// MAX-ID32: @ClStackIDThreadBuckets = weak_odr {{.*}}constant i32 32768
// MAX-STRICT-ID32: @ClStackIDThreadBuckets = weak_odr {{.*}}constant i32 65536
// WIDE-ZERO-DAG: @ClIDBucketSize = weak_odr {{.*}}constant i32 32768
// WIDE-ZERO-DAG: @ClStackIDThreadBuckets = weak_odr {{.*}}constant i32 0

int use_stack_id_bucket_flags(void) {
  int x = 1;
  return x;
}
