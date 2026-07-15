// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -I%crt_src/include -I%crt_src/lib -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// ERR: Unreclaimed IDs at exit: 0

// Thanks chatGPT!
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>


// Need to undef this so we can noinline it for optimization-fighting purposes
#undef get_id
uint64_t __attribute__((noinline)) get_id(void *ptr) {
  return (uint64_t)ptr >> 48;
}

/* ──────────────────────────────────────────────────────────── */
/* 1) strtol / strtoul / strtoll / strtoull                     */
/* ──────────────────────────────────────────────────────────── */
__attribute__((noinline))
static void test_strtol_literals(void)
{
  puts("=== strtol / strtoul ===");

  char s1[] = "12345xyz";
  char *end;
  uint16_t s1ID = get_id(s1);
  assert(s1ID > 0);
  long l = strtol(s1, &end, 10);
  assert(s1ID == get_id(s1) && s1ID == get_id(end));
  printf("strtol  : l  = %ld,           tail=\"%s\"\n", l, end);
  // CHECK: strtol  : l  = 12345,           tail="xyz"

  char s2[] = "0xFFdead+beef";
  uint16_t s2ID = get_id(s2);
  assert(s2ID > 0);
  unsigned long ul = strtoul(s2, &end, 0);
  assert(s2ID == get_id(s2) && s2ID == get_id(end));
  printf("strtoul : ul = 0x%lx,        tail=\"%s\"\n", ul, end);
  // CHECK: strtoul : ul = 0xffdead,        tail="+beef"

  char s3[] = "-0777junk";
  uint16_t s3ID = get_id(s3);
  assert(s3ID > 0);
  long long ll = strtoll(s3, &end, 0);
  assert(s3ID == get_id(s3) && s3ID == get_id(end));
  printf("strtoll : ll = %lld (oct),  tail=\"%s\"\n", ll, end);
  // CHECK: strtoll : ll = -511 (oct),  tail="junk"

  char s4[] = "18446744073709551615END";
  uint16_t s4ID = get_id(s4);
  assert(s4ID > 0);
  unsigned long long ull = strtoull(s4, &end, 10);
  assert(s4ID == get_id(s4) && s4ID == get_id(end));
  printf("strtoull: ull = %llu,       tail=\"%s\"\n", ull, end);
  // CHECK: strtoull: ull = 18446744073709551615,       tail="END"
}

/* ──────────────────────────────────────────────────────────── */
/* 2) strtod / strtof                                           */
/* ──────────────────────────────────────────────────────────── */
__attribute__((noinline))
static void test_strtod_variants(void)
{
  puts("\n=== strtod / strtof ===");

  char s5[] = "3.14159pi";
  char *end;
  uint16_t id5 = get_id(s5);
  assert(id5 > 0);
  double d = strtod(s5, &end);
  assert(id5 == get_id(s5) && id5 == get_id(end));
  printf("strtod  : d = %.5f, tail=\"%s\"\n", d, end);
  // CHECK: strtod  : d = 3.14159, tail="pi"

  char s6[] = "6.02e23junk";
  uint16_t id6 = get_id(s6);
  assert(id6 > 0);
  float f = strtof(s6, &end);
  assert(id6 == get_id(s6) && id6 == get_id(end));
  printf("strtof  : f = %.3e, tail=\"%s\"\n", f, end);
  // CHECK: strtof  : f = 6.020e+23, tail="junk"
}

/* ──────────────────────────────────────────────────────────── */
/* 4) chained conversions                                       */
/* ──────────────────────────────────────────────────────────── */
__attribute__((noinline))
static void test_chained_strtol_add(void)
{
  puts("\n=== chained strtol():  \"123+456\" ===");

  char expr[] = "123+456";
  uint16_t id = get_id(expr);
  assert(id > 0);
  char *p    = NULL;

  long a = strtol(expr, &p, 10);
  assert(id == get_id(expr) && id == get_id(p)); /* after first call */

  if (*p == '+') ++p;

  long b = strtol(p, &p, 10);
  assert(id == get_id(expr) && id == get_id(p)); /* after second call */

  long sum = a + b;
  printf("a=%ld, b=%ld, sum=%ld, tail=\"%s\"\n", a, b, sum, p);
  // CHECK: a=123, b=456, sum=579, tail=""
}

__attribute__((noinline))
static void test_mixed_bases_strtoull(void)
{
  puts("\n=== chained strtoull() with mixed bases ===");

  char buf[] = "0xFF,0755,42";
  uint16_t id = get_id(buf);
  assert(id > 0);
  char *p = buf;

  unsigned long long v1 = strtoull(p, &p, 0);
  assert(id == get_id(buf) && id == get_id(p));
  if (*p == ',') ++p;

  unsigned long long v2 = strtoull(p, &p, 0);
  assert(id == get_id(buf) && id == get_id(p));
  if (*p == ',') ++p;

  unsigned long long v3 = strtoull(p, &p, 0);
  assert(id == get_id(buf) && id == get_id(p));

  unsigned long long total = v1 + v2 + v3;
  printf("v1=%llu, v2=%llu, v3=%llu, total=%llu, tail=\"%s\"\n",
         v1, v2, v3, total, p);
  // CHECK: v1=255, v2=493, v3=42, total=790, tail=""
}

/* ──────────────────────────────────────────────────────────── */
int main(void)
{
  test_strtol_literals();
  test_strtod_variants();
  test_chained_strtol_add();
  test_mixed_bases_strtoull();
  return 0;
}
