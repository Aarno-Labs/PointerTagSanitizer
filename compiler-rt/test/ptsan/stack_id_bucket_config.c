// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -pthread -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-ids-per-bucket=8 -mllvm -ptsan-stack-id-thread-buckets=2 -o two %s
// RUN: ./two two > two.out 2> two.err
// RUN: FileCheck %s --check-prefix=TWO < two.out
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -pthread -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-ids-per-bucket=0 -mllvm -ptsan-stack-id-thread-buckets=0 -o zero %s
// RUN: ./zero zero > zero.out 2> zero.err
// RUN: FileCheck %s --check-prefix=ZERO < zero.out
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -pthread -mllvm -ptsan-stack-id-thread-buckets=129 -o too_many %s
// RUN: not --crash ./too_many 2> too_many.err
// RUN: FileCheck %s --check-prefix=TOO-MANY < too_many.err
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -pthread -mllvm -ptsan-stack-ids-per-bucket=32767 -mllvm -ptsan-stack-id-thread-buckets=1 -o too_wide %s
// RUN: not --crash ./too_wide 2> too_wide.err
// RUN: FileCheck %s --check-prefix=TOO-WIDE < too_wide.err

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint16_t u16;

extern __thread u16 ptsan_next_stack_id;
extern __thread u16 ptsan_stack_id_limit;

struct Snapshot {
  u16 next;
  u16 limit;
};

struct ThreadArg {
  struct Snapshot *snapshot;
};

static struct Snapshot main_snapshot;
static struct Snapshot worker_snapshots[2];
static struct ThreadArg args[2] = {
    {&worker_snapshots[0]},
    {&worker_snapshots[1]},
};
static volatile int ready_count;
static volatile int release_threads;

__attribute__((disable_sanitizer_instrumentation))
static void take_snapshot(struct Snapshot *S) {
  S->next = ptsan_next_stack_id;
  S->limit = ptsan_stack_id_limit;
}

__attribute__((disable_sanitizer_instrumentation))
static void *worker(void *Arg) {
  struct ThreadArg *A = (struct ThreadArg *)Arg;
  take_snapshot(A->snapshot);
  __atomic_fetch_add(&ready_count, 1, __ATOMIC_SEQ_CST);
  while (!__atomic_load_n(&release_threads, __ATOMIC_SEQ_CST)) {
  }
  return 0;
}

static int is_fast(struct Snapshot S) {
  return S.limit != 0;
}

static void run_two_bucket_case(void) {
  pthread_t threads[2];

  ready_count = 0;
  release_threads = 0;
  take_snapshot(&main_snapshot);
  pthread_create(&threads[0], 0, worker, &args[0]);
  while (__atomic_load_n(&ready_count, __ATOMIC_SEQ_CST) < 1) {
  }
  pthread_create(&threads[1], 0, worker, &args[1]);
  while (__atomic_load_n(&ready_count, __ATOMIC_SEQ_CST) < 2) {
  }
  __atomic_store_n(&release_threads, 1, __ATOMIC_SEQ_CST);
  pthread_join(threads[0], 0);
  pthread_join(threads[1], 0);

  int fast = is_fast(main_snapshot) + is_fast(worker_snapshots[0]) +
             is_fast(worker_snapshots[1]);
  int slow = 3 - fast;
  printf("two-bucket fast=%d slow=%d\n", fast, slow);
}

static void run_zero_bucket_case(void) {
  pthread_t thread;

  ready_count = 0;
  release_threads = 0;
  take_snapshot(&main_snapshot);
  pthread_create(&thread, 0, worker, &args[0]);
  while (__atomic_load_n(&ready_count, __ATOMIC_SEQ_CST) < 1) {
  }
  __atomic_store_n(&release_threads, 1, __ATOMIC_SEQ_CST);
  pthread_join(thread, 0);

  int fast = is_fast(main_snapshot) + is_fast(worker_snapshots[0]);
  int slow = 2 - fast;
  printf("zero-bucket fast=%d slow=%d\n", fast, slow);
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "zero") == 0) {
    run_zero_bucket_case();
    return 0;
  }
  run_two_bucket_case();
  return 0;
}

// TWO: two-bucket fast=2 slow=1
// ZERO: zero-bucket fast=0 slow=2
// TOO-MANY: PTSan stack ID configuration is invalid: requested too many stack ID buckets
// TOO-WIDE: PTSan stack ID configuration is invalid: reserved stack IDs exceed fast-path-safe u16 range
