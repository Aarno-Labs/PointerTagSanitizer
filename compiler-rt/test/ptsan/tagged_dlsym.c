// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// Don't crash!
// RUN: ./%exe

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    /* ----- symbol string obtained via malloc() ----- */
    const char *sym_text = "printf";
    char *symbol = malloc(strlen(sym_text) + 1);
    if (!symbol) {
        perror("malloc");
        return 1;
    }
    strcpy(symbol, sym_text);

    /* Use RTLD_DEFAULT so we don’t need an explicit dlopen() handle */
    void *addr = dlsym(RTLD_DEFAULT, symbol);
    if (!addr) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        free(symbol);
        return 1;
    }

    /* Cast and call */
    int (*real_printf)(const char *, ...) = (int (*)(const char *, ...))addr;
    real_printf("dlsym resolved %%s at %p\n", symbol, (void *)real_printf);

    free(symbol);
    return 0;
}
