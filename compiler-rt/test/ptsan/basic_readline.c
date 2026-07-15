// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -I%crt_src/include -I%crt_src/lib -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=OUT
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// OUT: line id: 0x1801
// OUT: dup id: 0x1802
// OUT: line id: 0x1801
// OUT: dup id: 0x1803
// OUT: resolved id: 0x1804

// ERR: Unreclaimed IDs at exit: 0

// Thanks chatgpt!
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>

int main(void) {
    // 1) fmemopen a small in-memory buffer containing multiple lines.
    const char data[] = "Hello, World!\nAnother line.\n";
    size_t data_len = sizeof(data) - 1; // exclude trailing '\0'

    FILE *fp = fmemopen((void *)data, data_len, "r");
    if (!fp) {
        perror("fmemopen");
        return 1;
    }

    print_id(fp);

    // 2) Use getline, which calls malloc/realloc internally.
    char *line = NULL;
    size_t capacity = 0;
    while (1) {
        ssize_t nread = getline(&line, &capacity, fp);
        if (nread == -1) {
            break; // EOF or error
        }
        print_id(line);

        // 3) Duplicate the line via strdup (calls malloc internally).
        char *dup = strdup(line);
        if (!dup) {
            perror("strdup");
            free(line);
            fclose(fp);
            return 1;
        }

        print_id(dup);

        free(dup);
    }
    free(line);
    fclose(fp);

    // 4) realpath with second arg == NULL calls malloc internally
    //    to store the resolved path.
    char *resolved = realpath(".", NULL);
    if (!resolved) {
        perror("realpath");
        return 1;
    }

    print_id(resolved);

    printf("Current directory resolved to: %s\n", resolved);
    free(resolved);

    return 0;
}
