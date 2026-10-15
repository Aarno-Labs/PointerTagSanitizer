// pointer_diff_test.c
// RUN: rm -rf %t && mkdir -p %t && cd %t

// No fix
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-runtime-validate-arith -mllvm -ptsan-clear-ids-on-arith=0 -o nofix %s
// RUN: ./nofix 2> err.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=UNFIXED

// Fixed
// RUN: %clang_ptsan -g -O2 -o fix -mllvm -ptsan-clear-ids-on-arith=1 %s
// RUN: ./fix > out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=FIXED

//UNFIXED: WARNING: Pointer-arith-or-compare pointers with different IDs

//FIXED: === test_ptr_minus_int ===
//FIXED: p - (p-2) in elements = 2
//FIXED: p - (p-2) in bytes    = 8
//FIXED: === test_same_buffer_ptr_diff ===
//FIXED: a - b in elements      = 15
//FIXED: a - b in bytes         = 60
//FIXED: === test_stack_ptr_diff_ub ===
//FIXED: element difference matches untagged addresses: 1
//FIXED: byte difference matches untagged addresses: 1



#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

__attribute__((noinline))
int *do_p_i_sub(int *p, int n) {
  return p - n;
}

__attribute__((noinline))
int *do_p_i_add(int *p, int n) {
  return p + n;
}

__attribute__((noinline))
long do_p_p_sub_int(int *p1, int *p2) {
  return p1 - p2;
}

__attribute__((noinline))
long do_p_p_sub_char(char *p1, char *p2) {
  return p1 - p2;
}

__attribute__((noinline))
void test_ptr_minus_int(int n, int m) {
  int buf[16];
  int *p = &buf[8];

  printf("=== test_ptr_minus_int ===\n");
  printf("buf      = 0x%lx\n", (uint64_t)buf);
  printf("p (buf+8)= 0x%lx\n", (uint64_t)p);

  int *p_minus = do_p_i_sub(p, n);    // subtracting an integer from a pointer
  int *p_plus  = do_p_i_add(p, m);    // and a plus for good measure

  printf("p-%d      = 0x%lx\n", n, (uint64_t)p_minus);
  printf("p+%d      = 0x%lx\n", m, (uint64_t)p_plus);

  ptrdiff_t diff_elems = p - p_minus;
  ptrdiff_t diff_bytes = (char *)p - (char *)p_minus;

  printf("p - (p-%d) in elements = %td\n", n, diff_elems);
  printf("p - (p-%d) in bytes    = %td\n", n, diff_bytes);
  printf("\n");
}

__attribute__((noinline))
void test_same_buffer_ptr_diff(int n, int m) {
  int buf[32];

  int *a = &buf[n];
  int *b = &buf[m];

  printf("=== test_same_buffer_ptr_diff ===\n");
  printf("buf      = 0x%lx\n", (uint64_t)buf);
  printf("a (buf+%d)= 0x%lx\n", n, (uint64_t)a);
  printf("b (buf+%d) = 0x%lx\n", m, (uint64_t)b);

  ptrdiff_t diff_elems = do_p_p_sub_int(a, b);
  ptrdiff_t diff_bytes = do_p_p_sub_char((char *)a, (char *)b);

  printf("a - b in elements      = %td\n", diff_elems);
  printf("a - b in bytes         = %td\n", diff_bytes);
  printf("\n");
}

__attribute__((noinline))
void test_stack_ptr_diff_ub(int n, int m) {
  int stack_buf1[4];
  int stack_buf2[8];

  int *p1 = &stack_buf1[n];
  int *p2 = &stack_buf2[m];

  printf("=== test_stack_ptr_diff_ub ===\n");
  printf("stack_buf1      = 0x%lx\n", (uint64_t)stack_buf1);
  printf("stack_buf2      = 0x%lx\n", (uint64_t)stack_buf2);
  printf("p1 (&stack_buf1[%d]) = 0x%lx\n", n, (uint64_t)p1);
  printf("p2 (&stack_buf2[%d]) = 0x%lx\n", m, (uint64_t)p2);

  // Subtracting pointers to different objects is formally UB, but PTSan's
  // arithmetic option is intended to clear their IDs for this operation.
  ptrdiff_t diff_elems = do_p_p_sub_int(p2, p1);
  ptrdiff_t diff_bytes = do_p_p_sub_char((char *)p2, (char *)p1);

  // Compute the reference with integer arithmetic on the low 48 address bits.
  // Signed subtraction handles either stack ordering without depending on the
  // distance between these objects or repeating the pointer subtraction above.
  intptr_t address1 = (intptr_t)((uintptr_t)p1 & UINT64_C(0x0000ffffffffffff));
  intptr_t address2 = (intptr_t)((uintptr_t)p2 & UINT64_C(0x0000ffffffffffff));
  ptrdiff_t expected_bytes = address2 - address1;
  ptrdiff_t expected_elems = expected_bytes / (ptrdiff_t)sizeof(*p1);

  printf("p2 - p1 in elements (UB) = %td\n", diff_elems);
  printf("p2 - p1 in bytes    (UB) = %td\n", diff_bytes);
  printf("element difference matches untagged addresses: %d\n",
         diff_elems == expected_elems);
  printf("byte difference matches untagged addresses: %d\n",
         diff_bytes == expected_bytes);
  printf("\n");
}

__attribute__((noinline))
int main(void) {
  test_ptr_minus_int(2, 3);
  test_same_buffer_ptr_diff(20, 5);
  test_stack_ptr_diff_ub(1, 3);
  return 0;
}
