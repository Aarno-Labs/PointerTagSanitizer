/* Make sure all stack getId routines work. We have
 * different ones for each 1-4, and a general case
 * for more.
*/

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -mllvm -ptsan-instrument-globals=0 -I%crt_src/include -O2 -o %exe %s
// RUN: ./%exe > out.txt 2> err.txt
// RUN: cat err.txt | FileCheck %s

// CHECK: Unreclaimed IDs at exit: 0
// CHECK: Functions by slow-path alloca amount:
// CHECK:   1: 1
// CHECK:   2: 1
// CHECK:   3: 1
// CHECK:   4: 1
// CHECK:   n: 1

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

#define u16 uint16_t
#define u32 uint32_t
#define u64 uint64_t

extern u16 ptsan_next_stack_id;
extern u16 ptsan_stack_id_limit;
extern u64 __ptsan_used_ids;

extern uint64_t ptsan_get_bound(uint16_t id);
extern uint64_t ptsan_get_base(uint16_t id);
extern uint64_t ptsan_get_size(uint16_t id);

inline u16 get_id(char* ptr) {
    return (u16)((u64)ptr >> 48);
}

u64 get_addr(char* ptr) {
    return ((u64)ptr & 0x0000ffffffffffff);
}

__attribute__((noinline))
void init(char *buf) {
  //printf("Initializing buffer at 0x%lx\n", (u64)buf);
  buf[0] = 'A';
  buf[1] = 'B';
  buf[2] = '\0';
}

__attribute__((noinline))
void doAlloc1() {
  char b1[23];
  printf("Alloc1\n");

  init(b1);
  printf("b1 = %s\n", b1);

  assert(__ptsan_used_ids == 1);
  assert(b1 == ptsan_get_base(get_id(b1)));
  assert(23 == ptsan_get_size(get_id(b1)));
}

__attribute__((noinline))
void doAlloc2() {
  char b1[23];
  char b2[43];

  printf("Alloc2\n");

  init(b1);
  init(b2);
  printf("b1 = %s\n", b1);
  printf("b2 = %s\n", b2);

  assert(__ptsan_used_ids == 2);
  assert(b1 == ptsan_get_base(get_id(b1)));
  assert(23 == ptsan_get_size(get_id(b1)));
  assert(b2 == ptsan_get_base(get_id(b2)));
  assert(43 == ptsan_get_size(get_id(b2)));
}

__attribute__((noinline))
void doAlloc3() {
  char b1[23];
  char b2[43];
  char b3[453];

  printf("Alloc3\n");

  init(b1);
  init(b2);
  init(b3);
  printf("b1 = %s\n", b1);
  printf("b2 = %s\n", b2);
  printf("b3 = %s\n", b3);

  assert(__ptsan_used_ids == 3);
  assert(b1 == ptsan_get_base(get_id(b1)));
  assert(23 == ptsan_get_size(get_id(b1)));
  assert(b2 == ptsan_get_base(get_id(b2)));
  assert(43 == ptsan_get_size(get_id(b2)));
  assert(b3 == ptsan_get_base(get_id(b3)));
  assert(453 == ptsan_get_size(get_id(b3)));
}

__attribute__((noinline))
void doAlloc4() {
  char b1[23];
  char b2[43];
  char b3[453];
  char b4[64];

  printf("Alloc4\n");

  init(b1);
  init(b2);
  init(b3);
  init(b4);
  printf("b1 = %s\n", b1);
  printf("b2 = %s\n", b2);
  printf("b3 = %s\n", b3);
  printf("b4 = %s\n", b4);

  assert(__ptsan_used_ids == 4);
  assert(b1 == ptsan_get_base(get_id(b1)));
  assert(23 == ptsan_get_size(get_id(b1)));
  assert(b2 == ptsan_get_base(get_id(b2)));
  assert(43 == ptsan_get_size(get_id(b2)));
  assert(b3 == ptsan_get_base(get_id(b3)));
  assert(453 == ptsan_get_size(get_id(b3)));
  assert(b4 == ptsan_get_base(get_id(b4)));
  assert(64 == ptsan_get_size(get_id(b4)));
}

__attribute__((noinline))
void doAlloc5() {
  char b1[23];
  char b2[43];
  char b3[453];
  char b4[64];
  char b5[67];

  printf("Alloc5\n");

  init(b1);
  init(b2);
  init(b3);
  init(b4);
  init(b5);
  printf("b1 = %s\n", b1);
  printf("b2 = %s\n", b2);
  printf("b3 = %s\n", b3);
  printf("b4 = %s\n", b4);
  printf("b5 = %s\n", b5);

  assert(__ptsan_used_ids == 5);
  assert(b1 == ptsan_get_base(get_id(b1)));
  assert(23 == ptsan_get_size(get_id(b1)));
  assert(b2 == ptsan_get_base(get_id(b2)));
  assert(43 == ptsan_get_size(get_id(b2)));
  assert(b3 == ptsan_get_base(get_id(b3)));
  assert(453 == ptsan_get_size(get_id(b3)));
  assert(b4 == ptsan_get_base(get_id(b4)));
  assert(64 == ptsan_get_size(get_id(b4)));
  assert(b5 == ptsan_get_base(get_id(b5)));
  assert(67 == ptsan_get_size(get_id(b5)));
}

int main() {
  ptsan_next_stack_id = ptsan_stack_id_limit;

  doAlloc1();
  assert(__ptsan_used_ids == 0);
  doAlloc2();
  assert(__ptsan_used_ids == 0);
  doAlloc3();
  assert(__ptsan_used_ids == 0);
  doAlloc4();
  assert(__ptsan_used_ids == 0);
  doAlloc5();
}