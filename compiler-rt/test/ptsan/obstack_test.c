// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt 2> err.txt
// RUN: cat out.txt | FileCheck %s
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// CHECK: BEGIN
// CHECK: A=[[A:0x[0-9A-Fa-f]+]]
// CHECK: B=[[B:0x[0-9A-Fa-f]+]]
// CHECK: C=[[C:0x[0-9A-Fa-f]+]]
// CHECK: FREE_TO_B
// CHECK: D=[[B]]
// CHECK: GROW_FINISH_START
// CHECK: F=[[F:0x[0-9A-Fa-f]+]]
// CHECK: FREE_TO_F
// CHECK: H=[[F]]
// CHECK: WHOLE_FREE
// CHECK: REINIT
// CHECK: J=0x
// CHECK: MULTI_CHUNK_START
// CHECK: A2=[[A2:0x[0-9A-Fa-f]+]]
// CHECK: BIG=[[BIG:0x[0-9A-Fa-f]+]]
// CHECK: FREE_TO_A2
// CHECK: K=[[A2]]
// CHECK: DONE

// ERR: Unreclaimed IDs at exit: 0


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
/* Use system malloc/free for chunks (default obstack behavior). */
#define obstack_chunk_alloc malloc
#define obstack_chunk_free free
#include <obstack.h>

__attribute__((noinline))
static void *alloc_n(struct obstack *ob, size_t n, int fill) {
  void *p = obstack_alloc(ob, n);
  if (p != NULL) {
    unsigned char *q = (unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
      q[i] = (unsigned char)fill;
    }
  }
  return p;
}

__attribute__((noinline))
static void *grow_and_finish(struct obstack *ob, const char *s) {
  for (const char *p = s; *p != '\0'; p++) {
    obstack_1grow(ob, *p);
  }
  obstack_1grow(ob, '\0');
  return obstack_finish(ob);
}

__attribute__((noinline))
static void force_big_alloc(struct obstack *ob, size_t n, int fill, void **out) {
  *out = alloc_n(ob, n, fill);
}

__attribute__((noinline))
int main(void) {
  struct obstack ob;
  obstack_init(&ob);

  puts("BEGIN");

  /* --- Basic bump allocations --- */
  void *A = alloc_n(&ob, 32, 0xAA);
  void *B = alloc_n(&ob, 64, 0xBB);
  void *C = alloc_n(&ob, 16, 0xCC);
  printf("A=%p\n", A);
  printf("B=%p\n", B);
  printf("C=%p\n", C);

  /* Free back to B: this should invalidate C and set next_free=B. */
  puts("FREE_TO_B");
  obstack_free(&ob, B);
  void *D = alloc_n(&ob, 16, 0xDD);
  printf("D=%p\n", D);  /* Should equal B */

  /* --- grow/finish path --- */
  puts("GROW_FINISH_START");
  void *F = grow_and_finish(&ob, "hello, obstack!");
  printf("F=%p\n", F);

  puts("FREE_TO_F");
  obstack_free(&ob, F);
  void *H = alloc_n(&ob, 8, 0xEE);
  printf("H=%p\n", H);  /* Should equal F */

  /* --- whole obstack free --- */
  puts("WHOLE_FREE");
  obstack_free(&ob, NULL);

  /* Re-initialize the control block before reusing it. */
  puts("REINIT");
  obstack_init(&ob);

  /* After reinit, fresh allocation succeeds (address arbitrary). */
  void *J = alloc_n(&ob, 32, 0xAB);
  printf("J=%p\n", J);

  /* --- multi-chunk scenario --- */
  puts("MULTI_CHUNK_START");
  void *A2 = alloc_n(&ob, 64, 0xA2);
  printf("A2=%p\n", A2);

  /* Force a new (large) chunk; 16 KiB will spill well past typical chunk_size. */
  void *BIG = NULL;
  force_big_alloc(&ob, 16 * 1024, 0xB1, &BIG);
  printf("BIG=%p\n", BIG);

  puts("FREE_TO_A2");
  obstack_free(&ob, A2);
  void *K = alloc_n(&ob, 32, 0xC3);
  printf("K=%p\n", K);  /* Should equal A2 */

  puts("DONE");

  obstack_free(&ob, NULL);
  return 0;
}
