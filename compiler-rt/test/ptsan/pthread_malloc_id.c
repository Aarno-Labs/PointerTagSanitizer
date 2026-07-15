// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 30 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 21 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 20 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 19 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD

// BAD: Found bad POINT access
// GOOD-NOT: Found bad POINT access


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <assert.h>

#define NUM_THREADS  1

void __attribute__((noinline)) inner(int idx, char *buf) {
  char c = buf[idx];
  printf("buf[%d] = %c\n", idx, c);
}

void *__attribute__((noinline)) outer(void *ptr) {
  int idx = *((int*) ptr);
  char *buf = malloc(20);
  for (int i = 0; i < 20; i++) {
    buf[i] = 'A' + (i % 26);
  }
  inner(idx, buf);
  free(buf);
  return NULL;
}

int main(int argc, char *argv[]) {
  pthread_t threads[NUM_THREADS];

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  int idx = atoi(argv[1]);

  for (int i = 0; i < NUM_THREADS; i++) {
      pthread_create(&threads[i], NULL, outer, &idx);
  }

  /* Wait for all threads to finish (or crash). */
  for (int i = 0; i < NUM_THREADS; i++) {
      pthread_join(threads[i], NULL);
  }

  return 0;
}