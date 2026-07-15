// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s

//CHECK: ints (reordered): 22 11 -33
//CHECK: reused string:    alpha alpha
//CHECK: mixed scalars:    123 4444444444 11 beef 12345
//CHECK: chars reused:     Z Z
//CHECK: pointer:          0x
//CHECK: floats reorder:   12345 3.125000 1.234500e+04

#include <stdio.h>

#define NOINLINE __attribute__((noinline))

static NOINLINE void run_positional(void) {
  /* Format uses indices repeatedly (e.g. %5$s twice), no width/precision. */
  const char *fmt =
      "ints (reordered): %2$d %1$d %3$d\n"
      "reused string:    %5$s %5$s\n"
      "mixed scalars:    %6$u %4$ld %1$d %7$x %8$o\n"
      "chars reused:     %9$c %9$c\n"
      "pointer:          %10$p\n"
      "floats reorder:   %12$g %11$f %12$e\n";

  /* Supply arguments in ordinary order; indices in the format pick them. */
  int          a1  = 11;              /* %1$d  */
  int          a2  = 22;              /* %2$d  */
  int          a3  = -33;             /* %3$d  */
  long         a4  = 4444444444L;     /* %4$ld */
  unsigned     a5u = 0u;              /* (unused, keeps slot free if you want) */
  const char  *a5  = "alpha";         /* %5$s  */
  unsigned     a6  = 123u;            /* %6$u  */
  unsigned     a7  = 0xBEEF;          /* %7$x  */
  unsigned     a8  = 012345u;         /* %8$o  */
  int          a9c = 'Z';             /* %9$c  */
  void        *a10 = (void*)&a1;      /* %10$p */
  double       a11 = 3.125;           /* %11$f */
  double       a12 = 12345.0;         /* %12$g and %12$e */

  /* Print once; no width/precision stars anywhere. */
  printf(fmt,
         a1,   /* 1 */
         a2,   /* 2 */
         a3,   /* 3 */
         a4,   /* 4 */
         a5,   /* 5 */
         a6,   /* 6 */
         a7,   /* 7 */
         a8,   /* 8 */
         a9c,  /* 9 */
         a10,  /* 10 */
         a11,  /* 11 */
         a12   /* 12 */
  );
}

int NOINLINE main(void) {
  run_positional();
  return 0;
}