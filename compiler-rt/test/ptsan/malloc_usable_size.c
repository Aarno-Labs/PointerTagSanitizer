// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=NORMAL

// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-use-returned-malloc-size -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=FIXED

// Should be a few of these
// NORMAL: Found bad

// FIXED-NOT: Found bad


// malloc_usable_size_test.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>  // for malloc_usable_size

__attribute__((noinline))
static void touch_region(unsigned char *p, size_t n, unsigned char value, const char *label) {
  printf("  touching %s region of size %zu with 0x%02x\n",
         label, n, (unsigned int)value);
  for (size_t i = 0; i < n; i++) {
    p[i] = value;
  }
}

__attribute__((noinline))
static void test_malloc_and_realloc(void) {
  printf("=== test_malloc_and_realloc ===\n");

  size_t requested1 = 97;
  void *p = malloc(requested1);
  if (p == NULL) {
    printf("malloc(%zu) failed\n", requested1);
    exit(1);
  }

  size_t usable1 = malloc_usable_size(p);
  printf("malloc: requested=%zu usable=%zu ptr=%p\n",
         requested1, usable1, p);
  touch_region((unsigned char *)p, usable1, 0xAA, "malloc");

  size_t requested2 = 983;
  void *p2 = realloc(p, requested2);
  if (p2 == NULL) {
    printf("realloc(%p, %zu) failed\n", p, requested2);
    free(p);
    exit(1);
  }
  p = p2;

  size_t usable2 = malloc_usable_size(p);
  printf("realloc: requested=%zu usable=%zu ptr=%p\n",
         requested2, usable2, p);
  touch_region((unsigned char *)p, usable2, 0xBB, "realloc");

  free(p);
}

__attribute__((noinline))
static void test_calloc(void) {
  printf("=== test_calloc ===\n");

  size_t nmemb = 1;
  size_t size = 8;
  void *p = calloc(nmemb, size);
  if (p == NULL) {
    printf("calloc(%zu, %zu) failed\n", nmemb, size);
    exit(1);
  }

  size_t requested = nmemb * size;
  size_t usable = malloc_usable_size(p);
  printf("calloc: requested=%zu usable=%zu ptr=%p\n",
         requested, usable, p);

  /* Only the first requested bytes are guaranteed zeroed, but we can
   * write up to 'usable' bytes safely. */
  touch_region((unsigned char *)p, usable, 0xCC, "calloc");

  free(p);
}

__attribute__((noinline))
static void test_strdup(void) {
  printf("=== test_strdup ===\n");

  const char *src = "hello malloc_usable_size and strdup";
  char *p = strdup(src);
  if (p == NULL) {
    printf("strdup failed\n");
    exit(1);
  }

  size_t requested = strlen(src) + 1;  // includes NUL
  size_t usable = malloc_usable_size(p);
  printf("strdup: requested=%zu usable=%zu ptr=%p\n",
         requested, usable, p);

  touch_region((unsigned char *)p, usable, 0xDD, "strdup");

  free(p);
}

__attribute__((noinline))
static void test_posix_memalign(void) {
  printf("=== test_posix_memalign ===\n");

  void *p = NULL;
  size_t alignment = 64;
  size_t requested = 421;

  int rc = posix_memalign(&p, alignment, requested);
  if (rc != 0) {
    printf("posix_memalign(&p, %zu, %zu) failed: rc=%d\n",
           alignment, requested, rc);
    exit(1);
  }

  size_t usable = malloc_usable_size(p);
  printf("posix_memalign: requested=%zu usable=%zu ptr=%p (alignment=%zu)\n",
         requested, usable, p, alignment);

  /* The returned pointer should be alignment-aligned. */
  if (((uintptr_t)p % alignment) != 0) {
    printf("WARNING: pointer %p is not %zu-byte aligned\n", p, alignment);
  }

  touch_region((unsigned char *)p, usable, 0xEE, "posix_memalign");

  free(p);
}

__attribute__((noinline))
int main(void) {
  test_malloc_and_realloc();
  test_calloc();
  test_strdup();
  test_posix_memalign();

  printf("All malloc_usable_size tests completed.\n");
  return 0;
}
