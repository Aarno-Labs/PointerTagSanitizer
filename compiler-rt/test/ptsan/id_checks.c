/* Check to make sure the ids are working right:
 * - `ptrs` uses the first malloc ID after the permanent global IDs.
 * - FuncA()/FuncB() use and return cached stack IDs independently.
 * - prebuf uses the next malloc ID, then returns it to the back of the list.
 * - Subsequent mallocs consume consecutive IDs through the maximum ID.
 *   The final malloc then reuses prebuf's ID.
*/

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -mllvm -ptsan-instrument-globals=0 -I%crt_src/include -O2 -o %exe %s
// RUN: ./%exe > out.txt 2> err.txt
// RUN: cat out.txt | FileCheck %s
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// CHECK: ptrs id: 0x[[#%x,FIRST:]]
// CHECK: max id: [[MAXID:[0-9a-f]+]]
// CHECK: ptrs[0] malloc id:  0x[[#FIRST + 2]]
// CHECK: ptrs[1] malloc id:  0x[[#FIRST + 3]]
// CHECK: ptrs[-4] malloc id: 0xfffc
// CHECK: ptrs[-3] malloc id: 0xfffd
// CHECK: ptrs[-2] malloc id: 0x[[MAXID]]
// CHECK: ptrs[-1] malloc id: 0x[[#FIRST + 1]]
// CHECK: Func id: 0x10002
// CHECK: prebuf id: 0x[[#FIRST + 1]]
// ERR: Unreclaimed IDs at exit: 0

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "Inputs/ptsan_test_interface.h"

extern volatile uint64_t __ptsan_used_ids;
extern volatile uint64_t __ptsan_used_global_ids;
#define p_n_ids() \
  printf("%d: used ids: %lu (0x%lx)\n", \
  __LINE__, __ptsan_used_ids, __ptsan_used_ids)

int __attribute__((noinline)) use(int *buf) {
  return buf[5];
}

uint64_t __attribute__((noinline)) funcB(char *buf) {
  char dest[20];
  uint64_t idA = (uint64_t)dest >> 48;
  uint64_t idB = (uint64_t)buf >> 48;
  memcpy(dest, buf, 20);
  return idA + (idB << 16);
}

uint64_t __attribute__((noinline)) funcA() {
  char str[20];
  return funcB(str);
}

void run() {
  printf("checking basic id functionality for %d ptr bits\n", ptsan_id_bits());
  // `- 2` because ptrs eats an ID, and max/default id is reserved
  // `- 0x1800` because those are reserved for stack IDs
  uint32_t N =
      ptsan_num_ids() - 2 - 0x1800 - __ptsan_used_global_ids;
  int **ptrs = (int **)malloc(N * sizeof(int*));

  assert(__ptsan_used_ids == __ptsan_used_global_ids + 1);

  printf("ptrs id: 0x%lx\n", (uint64_t)ptrs >> 48);
  assert(get_id(ptrs) == 0x1801 + __ptsan_used_global_ids);

  uint64_t funcID = funcA();

  assert(__ptsan_used_ids == __ptsan_used_global_ids + 1);

  int *prebuf = (int *)malloc(20 * sizeof(int));


  use(prebuf);
  uint64_t prebufID = (uint64_t)prebuf >> 48;

  assert(__ptsan_used_ids == __ptsan_used_global_ids + 2);

  free(prebuf);

  assert(__ptsan_used_ids == __ptsan_used_global_ids + 1);

  for (int i = 0; i < N; i++) {
    int *buf = (int *)malloc(20 * sizeof(int));

    if (i % 2000 == 0) {
      funcID = funcID | funcA();
    }

    use(buf);
    ptrs[i] = buf;
  }

  printf("max id: %lx\n", ptsan_max_id());
  p_n_ids();
  // - 0x1800 b/c stack IDs aren't removed from this
  assert(__ptsan_used_ids == ptsan_max_id() - 0x1800);

  printf("ptrs[0] malloc id:  0x%lx\n", (uint64_t)ptrs[0] >> 48);
  printf("ptrs[1] malloc id:  0x%lx\n", (uint64_t)ptrs[1] >> 48);
  printf("ptrs[-4] malloc id: 0x%lx\n", (uint64_t)ptrs[N - 4] >> 48);
  printf("ptrs[-3] malloc id: 0x%lx\n", (uint64_t)ptrs[N - 3] >> 48);
  printf("ptrs[-2] malloc id: 0x%lx\n", (uint64_t)ptrs[N - 2] >> 48);
  printf("ptrs[-1] malloc id: 0x%lx\n", (uint64_t)ptrs[N - 1] >> 48);

  printf("Func id: 0x%lx\n", funcID);
  printf("prebuf id: 0x%lx\n", prebufID);

  for (int i = 0; i < N; i++) {
    free(ptrs[i]);
  }

  free(ptrs);
  assert(__ptsan_used_ids == __ptsan_used_global_ids);
}

int main() {
  run();
  return 0;
}
