// The first 12 threads (including main) use reserved stack-ID buckets of
// 512 slots each, with ID zero reserved. Additional threads use runtime IDs
// from the shared cache.
// Check bucket reuse and slow-path allocation without assuming runtime ID order.

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-id-bucket-size=512 -mllvm -ptsan-stack-id-thread-buckets=12 -o %exe %s

// only main thread
// RUN: ./%exe 0 0 2> main.err
// RUN: cat main.err | FileCheck %s --check-prefix=MAIN

// MAIN-DAG: buf.id = 0x1ff
// MAIN: Max used stack IDs at once: 0 (0x0)

// One pthread, executed twice
// RUN: ./%exe 1 2 2> one.err
// RUN: cat one.err | FileCheck %s --check-prefix=ONE

// ONE-DAG: buf.id = 0x1ff
// ONE-DAG: buf.id = 0x3ff
// ONE-DAG: buf.id = 0x3ff
// ONE: Max used stack IDs at once: 0 (0x0)

// Eleven pthreads, executed twice
// RUN: ./%exe 11 2 2> eleven.err
// RUN: cat eleven.err | FileCheck %s --check-prefix=ELEVEN

// ELEVEN-DAG: buf.id = 0x1ff
// ELEVEN-DAG: buf.id = 0x3ff
// ELEVEN-DAG: buf.id = 0x7ff
// ELEVEN-DAG: buf.id = 0x9ff
// ELEVEN-DAG: buf.id = 0x5ff
// ELEVEN-DAG: buf.id = 0xbff
// ELEVEN-DAG: buf.id = 0xdff
// ELEVEN-DAG: buf.id = 0xfff
// ELEVEN-DAG: buf.id = 0x11ff
// ELEVEN-DAG: buf.id = 0x13ff
// ELEVEN-DAG: buf.id = 0x15ff
// ELEVEN-DAG: buf.id = 0x17ff
// ELEVEN-DAG: buf.id = 0x7ff
// ELEVEN-DAG: buf.id = 0x5ff
// ELEVEN-DAG: buf.id = 0x9ff
// ELEVEN-DAG: buf.id = 0xbff
// ELEVEN-DAG: buf.id = 0x3ff
// ELEVEN-DAG: buf.id = 0xdff
// ELEVEN-DAG: buf.id = 0x11ff
// ELEVEN-DAG: buf.id = 0x13ff
// ELEVEN-DAG: buf.id = 0xfff
// ELEVEN-DAG: buf.id = 0x15ff
// ELEVEN-DAG: buf.id = 0x17ff
// ELEVEN: Max used stack IDs at once: 0 (0x0)

// Twelve pthreads, executed three times
// RUN: ./%exe 12 3 2> twelve.err
// RUN: cat twelve.err | FileCheck %s --check-prefix=TWELVE

// RUN: %clang_ptsan_id32 -g -O2 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-id-bucket-size=512 -mllvm -ptsan-stack-id-thread-buckets=12 -mllvm -ptsan-stack-temporal-safety=0 -o %exe %s
// RUN: ./%exe 0 0 2> main.err
// RUN: cat main.err | FileCheck %s --check-prefix=MAIN
// RUN: ./%exe 1 2 2> one.err
// RUN: cat one.err | FileCheck %s --check-prefix=ONE
// RUN: ./%exe 11 2 2> eleven.err
// RUN: cat eleven.err | FileCheck %s --check-prefix=ELEVEN
// RUN: ./%exe 12 3 2> twelve.err
// RUN: cat twelve.err | FileCheck %s --check-prefix=TWELVE

// TWELVE: Global IDs: [[#GLOBALS:]]
// TWELVE-DAG: buf.id = 0x1ff
// TWELVE-DAG: buf.id = 0x5ff
// TWELVE-DAG: buf.id = 0x3ff
// TWELVE-DAG: buf.id = 0xbff
// TWELVE-DAG: buf.id = 0x9ff
// TWELVE-DAG: buf.id = 0x7ff
// TWELVE-DAG: buf.id = 0xdff
// TWELVE-DAG: buf.id = 0x11ff
// TWELVE-DAG: buf.id = 0x13ff
// TWELVE-DAG: buf.id = 0xfff
// TWELVE-DAG: buf.id = 0x15ff
// TWELVE-DAG: buf.id = 0x17ff
// TWELVE-DAG: buf.id = slow
// TWELVE-DAG: buf.id = 0x3ff
// TWELVE-DAG: buf.id = 0x9ff
// TWELVE-DAG: buf.id = 0xbff
// TWELVE-DAG: buf.id = 0xdff
// TWELVE-DAG: buf.id = 0x11ff
// TWELVE-DAG: buf.id = 0x5ff
// TWELVE-DAG: buf.id = 0x7ff
// TWELVE-DAG: buf.id = 0xfff
// TWELVE-DAG: buf.id = 0x15ff
// TWELVE-DAG: buf.id = 0x17ff
// TWELVE-DAG: buf.id = slow
// TWELVE-DAG: buf.id = 0x13ff
// TWELVE-DAG: buf.id = 0x5ff
// TWELVE-DAG: buf.id = 0x9ff
// TWELVE-DAG: buf.id = 0xbff
// TWELVE-DAG: buf.id = 0x7ff
// TWELVE-DAG: buf.id = 0xfff
// TWELVE-DAG: buf.id = 0x11ff
// TWELVE-DAG: buf.id = 0x15ff
// TWELVE-DAG: buf.id = 0xdff
// TWELVE-DAG: buf.id = 0x3ff
// TWELVE-DAG: buf.id = 0x13ff
// TWELVE-DAG: buf.id = slow
// TWELVE-DAG: buf.id = 0x17ff
// TWELVE: Max used stack IDs at once: 512 (0x200)

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <assert.h>
#include "Inputs/ptsan_test_interface.h"
#include<unistd.h>

extern volatile uint64_t __ptsan_used_global_ids;

__attribute__((noinline))
void inner(char *buf) {
  PTSanID id = get_id(buf);
  assert(id != 0 && id != ptsan_default_id());
#ifdef PTSAN_TEST_32_BIT_IDS
  // Check bucket spacing relative to the high-half namespace used by ID32.
  assert(id > UINT32_C(0x80000000));
  id -= UINT32_C(0x80000000);
#endif
  if (id >= 0x1800) {
    fprintf(stderr, "buf.id = slow\n");
  } else {
    fprintf(stderr, "buf.id = 0x%x\n", id);
  }
}

__attribute__((noinline))
void outer(int depth) {
  char buf[20];
  for (int i = 0; i < 20; i++) {
    buf[i] = 'A' + (i % 26);
  }

  if (depth == 1) {
    inner(buf);
  } else {
    outer(depth - 1);
  }
}

__attribute__((noinline))
void *worker(void *arg) {
  outer(512);
  sleep(1);
  return NULL;
}

__attribute__((noinline))
int main(int argc, char *argv[]) {
  fprintf(stderr, "Global IDs: %lu\n", __ptsan_used_global_ids);
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <index> <N threads> <M repeats>\n", argv[0]);
    return 1;
  }

  int N = atoi(argv[1]);
  int M = atoi(argv[2]);

  if (N < 0 || M < 0) {
    fprintf(stderr, "N and M must be positive.\n");
    return 1;
  }

  // Main consumes one ID; the first ID16 bucket also excludes zero.
  outer(ptsan_id_bits() == 32 ? 511 : 510);

  pthread_t *ths = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)N);

  for (int rep = 0; rep < M; rep++) {
    for (int i = 0; i < N; i++) {
      int rc = pthread_create(&ths[i], NULL, worker, NULL);
      assert(rc == 0);
    }

    for (int i = 0; i < N; i++) {
      pthread_join(ths[i], NULL);
    }
  }

  free(ths);
  return 0;
}
