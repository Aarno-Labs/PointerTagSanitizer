// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe %S/Inputs/NNET.DAT 2> out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK-NOT: bad header

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char inpath[256];

/* ----------- original function (verbatim) ----------- */
__attribute__((noinline))
static int read_data_file(void) {
  FILE *infile;
  int xinsize, yinsize, youtsize;
  int patt, element, i, row;
  int vals_read;

  infile = fopen(inpath, "r");
  if (!infile) {
    perror("fopen");
    return -1;
  }

  printf("About to scanf\n");
  vals_read = fscanf(infile, "%d%d%d", &xinsize, &yinsize, &youtsize);
  printf("Done\n");
  if (vals_read != 3) {
    fprintf(stderr, "bad header. Read %d values\n", vals_read);
    return -1;
  }

  fclose(infile);
  return 0;
}
/* -------- end of original code -------- */

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <datafile>\n", argv[0]);
    return EXIT_FAILURE;
  }
  strncpy(inpath, argv[1], sizeof(inpath) - 1);
  inpath[sizeof(inpath) - 1] = '\0';

  if (read_data_file() != 0) {
    fprintf(stderr, "read_data_file failed\n");
    return EXIT_FAILURE;
  }

  return 0;
}
