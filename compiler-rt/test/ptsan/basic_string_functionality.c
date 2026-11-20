// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -I%crt_src/include -I%crt_src/lib -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// ERR: Unreclaimed IDs at exit: 0

// Thanks chatGPT!
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>

static void banner(const char *title)
{
    puts("------------------------------------------------------------");
    puts(title);
}

/* 1. strlen / strcpy ------------------------------------------------ */
static void test_strlen_strcpy(void)
{
    banner("test_strlen_strcpy");

    const char *src = "Hello, strlen / strcpy!";
    size_t len  = strlen(src);
    char   buf[64];

    char *out = strcpy(buf, src);
    assert(ptsan_get_size(get_id(buf)) == 64);
    assert(get_id(out) == get_id(buf));

    printf("strlen(\"%s\") = %zu\n", buf, len);
}

/* 2. strcat --------------------------------------------------------- */
static void test_strcat(void)
{
    banner("test_strcat");

    char bufA[64] = "The answer is ";
    char bufB[128] = "42";
    char *out = strcat(bufA, bufB);

    assert(ptsan_get_size(get_id(out)) == 64);
    assert(get_id(bufA) == get_id(out));
    assert(get_id(bufB) != get_id(out));

    printf("strcat result \"%s\"\n", bufA);
}

/* 3. strstr (found / not‑found) ------------------------------------ */
static void test_strstr(void)
{
    banner("test_strstr");

    char hay[48] = "one‑two‑three‑four";
    const char *needle_ok   = "two";
    const char *needle_fail = "five";

    const char *pos_ok   = strstr(hay, needle_ok);
    const char *pos_fail = strstr(hay, needle_fail);

    assert(ptsan_get_size(get_id(pos_ok)) == 48);
    assert(pos_fail == 0);
    assert(get_id(pos_ok) == get_id(hay));

    printf("strstr(\"%s\",\"%s\") -> %s\n",
           hay, needle_ok, pos_ok   ? pos_ok   : "(null)");

    printf("strstr(\"%s\",\"%s\") -> %s\n",
           hay, needle_fail, pos_fail ? pos_fail : "(null)");
}

/* 4. strchr / strrchr ---------------------------------------------- */
static void test_strchr_family(void)
{
    banner("test_strchr / strrchr");

    char s[64] = "/usr/local/bin:/usr/bin:/bin";
    const char *first_colon = strchr(s, ':');
    const char *last_colon  = strrchr(s, ':');

    assert(ptsan_get_size(get_id(s)) == 64);
    assert(get_id(first_colon) == get_id(s));
    assert(get_id(last_colon) == get_id(s));

    printf("first ':' @ offset %td\n", first_colon - s);
    printf("last  ':' @ offset %td\n", last_colon  - s);
}

/* 5. memcpy vs memmove --------------------------------------------- */
static void test_memcpy_memmove(void)
{
    banner("test_memcpy / memmove (overlap)");

    char src[] = "123456789";
    char dst[16];

    void *cpy = memcpy(dst, src, sizeof(src));   /* normal copy */
    printf("memcpy dst = \"%s\"\n", dst);

    /* overlapping move inside same buffer */
    void *move = memmove(src + 2, src, 5);        /* produces "121234569" */
    printf("memmove overlapped src = \"%s\"\n", src);

    assert(get_id(cpy) == get_id(dst));
    assert(get_id(move) == get_id(src));
}

/* ------------------------------------------------------------------ */
int main(void)
{
    test_strlen_strcpy();
    test_strcat();
    test_strstr();
    test_strchr_family();
    test_memcpy_memmove();

    puts("All basic‑string tests completed.");
    return 0;
}
