// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -I%crt_src/include -I%crt_src/lib -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// ERR: Unreclaimed IDs at exit: 0

// Thanks chatgpt!
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>

static void touch(char *p, size_t n, char fill) {
  /* make sure the memory really gets used */
  for (size_t i = 0; i < n; ++i)
    p[i] = (char)(fill + i);
  volatile unsigned char sink = p[0]; /* read‑back to avoid DCE */
  (void)sink;
}

int main(void) {
  /* ----------  Case A: realloc(NULL, n)  ---------- */
  char *pA = (char *)realloc(NULL, 32);
  if (!pA) {
    perror("realloc NULL");
    return 1;
  }
  touch(pA, 32, 'A');
  printf("Case A  malloc-via-realloc  ptr=%p  size=%zu  first=%c\n", (void *)pA,
         32, pA[0]);
  print_id(pA);
  assert(ptsan_get_size(get_id(pA)) == 32);

  /* ----------  Case B: shrink ‑ should stay in place  ---------- */
  char *pB = (char *)malloc(64);
  touch(pB, 64, 'B');
  assert(ptsan_get_size(get_id(pB)) == 64);

  char *pB_shrink = (char *)realloc(pB, 16); /* shrink */
  printf("Case B  shrink          old=%p  new=%p\n", (void *)pB,
         (void *)pB_shrink);

  assert(get_id(pB) == get_id(pB_shrink));
  assert(ptsan_get_size(get_id(pB)) == 16);

  /* ----------  Case C: grow ‑ likely moves  ---------- */
  char *pB_grow = (char *)realloc(pB_shrink, 128); /* grow */
  printf("Case C  grow            old=%p  new=%p\n", (void *)pB_shrink,
         (void *)pB_grow);
  if (!pB_grow) {
    perror("realloc grow");
    return 1;
  }
  touch(pB_grow, 128, 'C');
  assert(get_id(pB) != get_id(pB_grow));
  assert(ptsan_get_size(get_id(pB)) == 0);
  assert(ptsan_get_size(get_id(pB_grow)) == 128);


  /* ----------  Case D: forced failure path  ---------- */
  size_t huge = 0xaFFFFFFFFFFFFFFF; /* almost certainly fails */
  char *p_fail = (char *)realloc(pB_grow, huge);
  assert(p_fail == NULL);
  printf("Case D  failure-path   realloc returned NULL, "
           "original ptr still %p, first=%c\n",
           (void *)pB_grow, pB_grow[0]);
  assert(ptsan_get_size(get_id(pB_grow)) == 128);

  /* ----------  Clean‑up  ---------- */
  free(pA);
  free(pB_grow);

  return 0;
}
