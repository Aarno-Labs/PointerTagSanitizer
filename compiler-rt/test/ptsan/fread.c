// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK-NOT: unable to read data

#define _GNU_SOURCE    /* for fmemopen() on glibc */
#include <stdio.h>
#include <stdlib.h>

const char *g_program;

__attribute__((noinline))
char *load_file() {
  char *data;

  static char input[] = "Hello, world!  This comes from memory.";
  size_t size = sizeof(input) - 1;          /* bytes to read */

  FILE *fp = fmemopen(input, size, "r");    /* memory-backed FILE* */

  fseek(fp, 0, SEEK_SET);

  /* Allocate a buffer for the data. */
  data = malloc(size + 1);

  /* Read in the file contents. */
  data[size] = 0;
  int out = fread(data, size, 1, fp);
  if (out != 1) {
    fprintf(stderr, "%s: error: unable to read data for '%s'. Got %d\n",
            g_program, "mem", out);
    exit(2);
  }

  /* Close the file and return the data. */
  fclose(fp);
  return data;
}


int main(int argc, char * const argv[]) {
  g_program = argv[0];
  char *data = load_file();
  printf("Got %s\n", data);
  free(data);
}
