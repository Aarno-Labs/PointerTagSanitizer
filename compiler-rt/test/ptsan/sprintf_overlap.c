// This is UB according to the standard, but spec 507 (cactuBSSN)
// relies on it, so better make it work!

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: Size: 1 3 5 2

#include <stdlib.h>
#include <stdio.h>

__attribute__((noinline)) char * first(int n, int *buf) {
  char *mess = calloc(200, sizeof(char));
  sprintf(mess, "Size:");
  for (int i = 0; i < n; i++) {
    sprintf(mess, "%s %d", mess, buf[i]);
  }
  return mess;
}

// This is more UB-y UB, so I'm not checking for it. But note
// that currently glibc gives a different result than ptsan.
__attribute__((noinline)) char * last(int n, int *buf) {
  char *mess = calloc(200, sizeof(char));
  sprintf(mess, "Size:");
  for (int i = 0; i < n; i++) {
    sprintf(mess, "%d %s", buf[i], mess);
  }
  return mess;
}

int main() {
  int ns[4] = {1, 3, 5 ,2};
  char *mess = first(4, ns);
  printf("%s\n", mess);
  free(mess);

  mess = last(4, ns);
  printf("%s\n", mess);
  free(mess);
}