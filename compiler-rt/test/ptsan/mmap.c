// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=OUT
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// OUT: Mapped byte = 0x42
// OUT: Invalid access =

// ERR: Startup IDs: [[#STARTUP:]]
// ERR: Found bad POINT access
// ERR: Max used IDs at once: [[#STARTUP + 1]]


#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

extern volatile uint64_t __ptsan_used_ids;

int main(void) {
    fprintf(stderr, "Startup IDs: %lu\n", __ptsan_used_ids);
    /* 1. figure out a page size */
    size_t length = sysconf(_SC_PAGESIZE);
    if (length == (size_t)-1) {
        perror("sysconf");
        return EXIT_FAILURE;
    }

    /* 2. mmap an anonymous, read‑write page */
    void *addr = mmap(NULL, length,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      /* no file: */ -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }

    printf("mmap id is 0x%lx\n", (uint64_t)addr >> 48);

    /* 3. use it — write then read back via a volatile pointer so
       the compiler must actually emit the store/load: */
    volatile char *p = (volatile char*)addr;
    p[0] = 0x42;
    printf("Mapped byte = 0x%02x\n", (unsigned char)p[0]);

    p+= length + 10;
    printf("Invalid access = 0x%02x\n", (unsigned char)p[0]);

    /* 4. munmap the page */
    if (munmap(addr, length) != 0) {
        perror("munmap");
        return EXIT_FAILURE;
    }

    void *newMem = malloc(4);
    printf("After munmap, id is 0x%lx\n", (uint64_t)newMem >> 48);

    return 0;
}