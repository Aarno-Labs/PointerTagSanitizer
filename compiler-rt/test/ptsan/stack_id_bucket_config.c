// Bucket ownership is exclusive; zero reserved buckets uses the slow allocator.
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan_plain -g -O2 -pthread -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=2 -o two %s
// RUN: ./two two > two.out 2> two.err
// RUN: FileCheck %s --check-prefix=TWO < two.out
// RUN: %clang_ptsan_plain -g -O2 -pthread -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-id-thread-buckets=0 -o zero %s
// RUN: ./zero zero > zero.out 2> zero.err
// RUN: FileCheck %s --check-prefix=ZERO < zero.out
// RUN: %clang_ptsan_plain -g -O2 -pthread -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-stack-id-thread-buckets=0 -o zero_sequential %s
// RUN: ./zero_sequential zero | FileCheck %s --check-prefix=ZERO
// RUN: %clang_ptsan_plain -g -O2 -pthread -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=129 -o many %s
// RUN: ./many many | FileCheck %s --check-prefix=MANY
// RUN: %clang_ptsan_plain -g -O2 -pthread -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=129 -o many_sequential %s
// RUN: ./many_sequential many | FileCheck %s --check-prefix=MANY
// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -g -O2 -pthread -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=129 -o many_id32 %s
// RUN: ./many_id32 many | FileCheck %s --check-prefix=MANY
// RUN: %clang_ptsan_plain -g -O2 -pthread -mllvm -ptsan-id-bucket-size=131072 -mllvm -ptsan-stack-id-thread-buckets=0 -o no_chunks %s
// A whole ID16 namespace fits one bucket, with only zero and fallback skipped.
// RUN: %clang_ptsan_plain -g -O2 -pthread -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-id-bucket-size=65536 -mllvm -ptsan-stack-id-thread-buckets=0 -o full_namespace %s
// RUN: ./full_namespace zero | FileCheck %s --check-prefix=ZERO
// RUN: not ./no_chunks 2> no_chunks.err
// RUN: FileCheck %s --check-prefix=NO-CHUNKS < no_chunks.err

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Snapshot {
  uint64_t next;
  uint64_t limit;
};

struct ThreadArg {
  struct Snapshot *snapshot;
};

enum { MaxThreads = 129 };
static struct Snapshot main_snapshot;
static struct Snapshot worker_snapshots[MaxThreads];
static struct ThreadArg args[MaxThreads];
static volatile int ready_count;
static volatile int release_threads;

__attribute__((disable_sanitizer_instrumentation))
static void take_snapshot(struct Snapshot *S) {
  S->next = ptsan_get_stack_cursor();
  S->limit = ptsan_get_stack_limit();
}

__attribute__((disable_sanitizer_instrumentation))
static void *worker(void *Arg) {
  struct ThreadArg *A = (struct ThreadArg *)Arg;
  take_snapshot(A->snapshot);
  __atomic_fetch_add(&ready_count, 1, __ATOMIC_SEQ_CST);
  while (!__atomic_load_n(&release_threads, __ATOMIC_SEQ_CST)) {
    sched_yield();
  }
  return 0;
}

static int is_fast(struct Snapshot S) {
  return S.next < S.limit;
}

static void run_bucket_case(unsigned Count) {
  pthread_t threads[MaxThreads];
  ready_count = 0;
  release_threads = 0;
  take_snapshot(&main_snapshot);
  for (unsigned I = 0; I < Count; ++I) {
    args[I].snapshot = &worker_snapshots[I];
    assert(pthread_create(&threads[I], NULL, worker, &args[I]) == 0);
  }
  while (__atomic_load_n(&ready_count, __ATOMIC_SEQ_CST) != Count) {
    sched_yield();
  }
  __atomic_store_n(&release_threads, 1, __ATOMIC_SEQ_CST);
  int fast = is_fast(main_snapshot);
  for (unsigned I = 0; I < Count; ++I) {
    assert(pthread_join(threads[I], NULL) == 0);
    fast += is_fast(worker_snapshots[I]);
  }
  printf("buckets fast=%d slow=%d\n", fast, (int)Count + 1 - fast);
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "many") == 0) {
    // Fill all 129 reservations and exercise fallback for one extra thread.
    // Repeat after joining to check that every descriptor can be reused.
    run_bucket_case(MaxThreads);
    run_bucket_case(MaxThreads);
  } else {
    run_bucket_case(argc > 1 && strcmp(argv[1], "zero") == 0 ? 1 : 2);
  }
  return 0;
}

// TWO: buckets fast=2 slow=1
// ZERO: buckets fast=0 slow=2
// MANY-COUNT-2: buckets fast=129 slow=1
// NO-CHUNKS: PTSan ID bucket size exceeds usable ID space
