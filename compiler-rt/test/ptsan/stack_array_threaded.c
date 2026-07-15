// Make sure IDs are right threaded. The first 12 threads (including main,
// so 11 extra) use stack ID caches. The rest don't. The stack ID caches
// are multiples of 0x200 (512). The non-stack-ID ids should end in 1 b/c
// `ths` is malloc-ed, which eats one 1. The stack IDs should be reused, but
// the malloc ones should not, since they're LIFO.

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s

// only main thread
// RUN: ./%exe 0 0 2> main.err
// RUN: cat main.err | FileCheck %s --check-prefix=MAIN

// MAIN-DAG: buf.id = 0x200
// MAIN: Max used stack IDs at once: 0 (0x0)

// One pthread, executed twice
// RUN: ./%exe 1 2 2> one.err
// RUN: cat one.err | FileCheck %s --check-prefix=ONE

// ONE-DAG: buf.id = 0x200
// ONE-DAG: buf.id = 0x400
// ONE-DAG: buf.id = 0x400
// ONE: Max used stack IDs at once: 0 (0x0)

// Eleven pthreads, executed twice
// RUN: ./%exe 11 2 2> eleven.err
// RUN: cat eleven.err | FileCheck %s --check-prefix=ELEVEN

// ELEVEN-DAG: buf.id = 0x200
// ELEVEN-DAG: buf.id = 0x400
// ELEVEN-DAG: buf.id = 0x800
// ELEVEN-DAG: buf.id = 0xa00
// ELEVEN-DAG: buf.id = 0x600
// ELEVEN-DAG: buf.id = 0xc00
// ELEVEN-DAG: buf.id = 0xe00
// ELEVEN-DAG: buf.id = 0x1000
// ELEVEN-DAG: buf.id = 0x1200
// ELEVEN-DAG: buf.id = 0x1400
// ELEVEN-DAG: buf.id = 0x1600
// ELEVEN-DAG: buf.id = 0x1800
// ELEVEN-DAG: buf.id = 0x800
// ELEVEN-DAG: buf.id = 0x600
// ELEVEN-DAG: buf.id = 0xa00
// ELEVEN-DAG: buf.id = 0xc00
// ELEVEN-DAG: buf.id = 0x400
// ELEVEN-DAG: buf.id = 0xe00
// ELEVEN-DAG: buf.id = 0x1200
// ELEVEN-DAG: buf.id = 0x1400
// ELEVEN-DAG: buf.id = 0x1000
// ELEVEN-DAG: buf.id = 0x1600
// ELEVEN-DAG: buf.id = 0x1800
// ELEVEN: Max used stack IDs at once: 0 (0x0)

// Twelve pthreads, executed three times
// RUN: ./%exe 12 3 2> twelve.err
// RUN: cat twelve.err | FileCheck %s --check-prefix=TWELVE

// TWELVE-DAG: buf.id = 0x200
// TWELVE-DAG: buf.id = 0x600
// TWELVE-DAG: buf.id = 0x400
// TWELVE-DAG: buf.id = 0xc00
// TWELVE-DAG: buf.id = 0xa00
// TWELVE-DAG: buf.id = 0x800
// TWELVE-DAG: buf.id = 0xe00
// TWELVE-DAG: buf.id = 0x1200
// TWELVE-DAG: buf.id = 0x1400
// TWELVE-DAG: buf.id = 0x1000
// TWELVE-DAG: buf.id = 0x1600
// TWELVE-DAG: buf.id = 0x1800
// TWELVE-DAG: buf.id = 0x1a01
// TWELVE-DAG: buf.id = 0x400
// TWELVE-DAG: buf.id = 0xa00
// TWELVE-DAG: buf.id = 0xc00
// TWELVE-DAG: buf.id = 0xe00
// TWELVE-DAG: buf.id = 0x1200
// TWELVE-DAG: buf.id = 0x600
// TWELVE-DAG: buf.id = 0x800
// TWELVE-DAG: buf.id = 0x1000
// TWELVE-DAG: buf.id = 0x1600
// TWELVE-DAG: buf.id = 0x1800
// TWELVE-DAG: buf.id = 0x1c01
// TWELVE-DAG: buf.id = 0x1400
// TWELVE-DAG: buf.id = 0x600
// TWELVE-DAG: buf.id = 0xa00
// TWELVE-DAG: buf.id = 0xc00
// TWELVE-DAG: buf.id = 0x800
// TWELVE-DAG: buf.id = 0x1000
// TWELVE-DAG: buf.id = 0x1200
// TWELVE-DAG: buf.id = 0x1600
// TWELVE-DAG: buf.id = 0xe00
// TWELVE-DAG: buf.id = 0x400
// TWELVE-DAG: buf.id = 0x1400
// TWELVE-DAG: buf.id = 0x1e01
// TWELVE-DAG: buf.id = 0x1800
// TWELVE: Max used stack IDs at once: 512 (0x200)

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <assert.h>
#include<unistd.h>


__attribute__((noinline))
void inner(char *buf) {
  fprintf(stderr, "buf.id = 0x%lx\n", (uint64_t)buf >> 48);
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

  outer(511);

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
