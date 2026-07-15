// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// ERR: Unreclaimed IDs at exit: 0

// CHECK: init ret = 0

// Thanks ChatGPT!
#include <pthread.h>
#include <stdio.h>

/* Keep the call visible in IR/asm for easier debugging. */
__attribute__((noinline)) static void run_test(void) {

  pthread_mutex_t g_mtx;
  /* Initialize the mutex with default attributes. */
  int rc = pthread_mutex_init(&g_mtx, /*attr=*/NULL);
  printf("init ret = %d\n", rc);

  /* Use the mutex so we exercise lock/unlock interceptors too. */
  pthread_mutex_lock(&g_mtx);
  puts("locked");
  pthread_mutex_unlock(&g_mtx);
}

int main(void) {
  run_test();
  return 0;
}
