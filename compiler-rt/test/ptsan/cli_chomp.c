// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK-NOT: Found bad access

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  The function under test                                            */
/* ------------------------------------------------------------------ */
__attribute__((noinline))
int cli_chomp(char *string, int l)
{
    l--;

    while ((l >= 0) && ((string[l] == '\n'))) {
      string[l] = '\0';
      l--;
    }

    return l + 1;
}

int main(void)
{
  char s1[] = "hello, world\n";
  cli_chomp(s1, strlen(s1));
  fprintf(stderr, "post-chomp 1: %s\n", s1);
}
