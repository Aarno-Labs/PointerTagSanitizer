// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -I%crt_src/include -I%crt_src/lib -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// ERR: Unreclaimed IDs at exit: 0

// Thanks chatgpt!
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>

static void dump(const char *tag,
                 char       *buf,
                 size_t      buf_sz,
                 ssize_t     len)
{
    /* Printing the pointer value prevents the optimiser from discarding it. */
    printf("%-6s | ptr=%p  size=%zu  len=%zd  text=\"%.*s\"\n",
           tag, (void *)buf, buf_sz, len,
           (int)(len > 0 ? len : 0), buf);
}

int main(void)
{
    /* The test data contains both commas and new‑lines. */
    const char *src =
        "aaa,bbbbb,cccccccc\n"
        "lastline\n";

    /* Open an in‑memory FILE* so we can use stdio calls. */
    FILE *fp = fmemopen((void *)src, strlen(src), "r");
    if (!fp) { perror("fmemopen"); return 1; }

    /* ---------------  Case A : *lineptr == NULL  ---------------- */
    char   *buf  = NULL;          /* Line buffer               */
    size_t  sz   = 0;             /* Allocated size            */
    ssize_t len  = getdelim(&buf, &sz, ',', fp);       /* malloc inside */
    dump("CASE A", buf, sz, len);
    print_id(buf);
    assert(ptsan_get_size(get_id(buf)) == sz);
    uint16_t buf_id = get_id(buf);

    /* ---------------  Case B : buffer big enough  --------------- */
    len = getdelim(&buf, &sz, ',', fp);                /* re‑use buffer */
    dump("CASE B", buf, sz, len);
    print_id(buf);
    assert(ptsan_get_size(get_id(buf)) == sz);
    assert(buf_id == get_id(buf));

    /* ---------------  Case C : buffer too small  ---------------- */
    /*  Force a tiny buffer so getdelim() must realloc().          */
    free(buf);
    buf = (char *)malloc(2);      /* tiny buffer               */
    char *old_buf = buf;
    assert(buf_id + 1 == get_id(buf));
    sz  = 2;
    len = getdelim(&buf, &sz, ',', fp);                /* triggers realloc */
    dump("CASE C", buf, sz, len);
    print_id(buf);
    assert(buf_id + 2 == get_id(buf) || buf == old_buf);

    /* ---------------  Case D : use getline() (newline delim) ---- */
    len = getline(&buf, &sz, fp);                      /* may reuse / realloc */
    dump("CASE D", buf, sz, len);
    print_id(buf);

    /* Clean up */
    free(buf);
    fclose(fp);
    return 0;
}
