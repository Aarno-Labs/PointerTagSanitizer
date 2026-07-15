// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -c %S/cjson_app.c
// RUN: %clang_ptsan -g -O2 -c %S/lib/cJSON.c
// RUN: %clang_ptsan -g -O2 cjson_app.o cJSON.o -o %exe
// RUN: ./%exe %S/../Inputs/cjson_ch.json 1 2> err.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// Not doing anything with this automatically, but it makes it
// *way* easier to check our overhead:
// RUN: %clang_raw -g -O2 -c %S/cjson_app.c -o cjson_app_raw.o
// RUN: %clang_raw -g -O2 -c %S/lib/cJSON.c -o cJSON_raw.o
// RUN: %clang_raw -g -O2 cjson_app_raw.o cJSON_raw.o -o app_raw

// ERR: Unreclaimed IDs at exit: 0

#define _GNU_SOURCE             /* for MAP_POPULATE */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lib/cJSON.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <json-file> <iterations>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *path = argv[1];
    int iterations = atoi(argv[2]);

    /* ---------- open + size + mmap ---------- */
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return EXIT_FAILURE;
    }

    struct stat st;
    if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "%s: not a regular file\n", path);
        close(fd);
        return EXIT_FAILURE;
    }
    size_t size = (size_t)st.st_size;
    if (size == 0) {
        fprintf(stderr, "%s: file is empty\n", path);
        close(fd);
        return EXIT_FAILURE;
    }

    void *mapping = mmap(NULL, size, PROT_READ,
                         MAP_PRIVATE | MAP_POPULATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }

    const char *json_text = (const char *)mapping;

    // Iterate the parsing so we can get semi-reasonable
    // perf numbers
    printf("About to run %d times:\n", iterations);
    for (int i = 0; i < iterations; i++) {
      cJSON *root = cJSON_ParseWithLength(json_text, size);
      if (!root) {
          fprintf(stderr, "cJSON_Parse error: %s\n", cJSON_GetErrorPtr());
          munmap(mapping, size);
          return EXIT_FAILURE;
      }

      char *printed = cJSON_Print(root);
      if (printed) {
          //puts(printed);
          free(printed);
      }

      cJSON_Delete(root);
    }

    munmap(mapping, size);
    return 0;
}
