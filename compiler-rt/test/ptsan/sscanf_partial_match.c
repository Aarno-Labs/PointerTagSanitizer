// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s

//CHECK: `solve rows 10 cols 20 Xblocks Y`:  f1 returned 5
//CHECK:           rows=10 cols=20 c1=X blocks="blocks" c2=Y

//CHECK: `solve rows 5 cols 6  Zblock`:  f1 returned 4
//CHECK:           rows=5 cols=6 c1=Z blocks="block" c2=-

//CHECK: `solve rows ten cols 20 Ablocks B`:  f1 returned 0
//CHECK:           rows=0 cols=0 c1=- blocks="" c2=-

//CHECK: `completely unrelated text`:  f1 returned 0
//CHECK:           rows=0 cols=0 c1=- blocks="" c2=-

//CHECK: ``:  f1 returned -1
//CHECK:           rows=0 cols=0 c1=- blocks="" c2=-

//CHECK: `20 hello`:  f2 returned 1
//CHECK:             n = 20

//CHECK: `20 goodbye`:  f2 returned 1
//CHECK:             n = 20

//CHECK: `unrelated`:  f2 returned 0
//CHECK:             n = 0

//CHECK: ``:  f2 returned -1
//CHECK:             n = 0


/* sscanf_error_tests.c */
#include <stdio.h>

void f1() {
  const char *tests[] = {
      "solve rows 10 cols 20 Xblocks Y",
      "solve rows 5 cols 6  Zblock",          /* missing trailing %c */
      "solve rows ten cols 20 Ablocks B",      /* “ten” → not a number */
      "completely unrelated text",
      "" // Should EOF
  };

  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    unsigned num_rows = 0, num_cols = 0;
    char c1 = '\0', c2 = '\0', blocks[64] = {0};

    int t = sscanf(tests[i], "solve rows %u cols %u %c%s %c", &num_rows,
                   &num_cols, &c1, blocks, &c2);

    printf("`%s`:  f1 returned %d\n", tests[i], t);
    printf("          rows=%u cols=%u c1=%c blocks=\"%s\" c2=%c\n\n", num_rows,
           num_cols, c1 ? c1 : '-', blocks, c2 ? c2 : '-');
  }
}


void f2() {
  const char *tests[] = {
      "20 hello",
      "20 goodbye",
      "unrelated",
      "" // Should EOF
  };

  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    int n = 0;

    int t = sscanf(tests[i], "%d hello", &n);

    printf("`%s`:  f2 returned %d\n", tests[i], t);
    printf("            n = %d\n", n);
  }
}

int main(void) {
  f1();
  f2();
  return 0;
}
