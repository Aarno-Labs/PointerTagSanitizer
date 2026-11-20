// Sanity check -- this was crashing at one point

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s -S -emit-llvm

#include <assert.h>   /* assert()                       */
#include <ctype.h>    /* isdigit()                      */
#include <stdbool.h>  /* bool / true / false            */
#include <string.h>   /* strlen()                       */
#include <stdint.h>   /* fixed-width integer typedefs   */

/* ---------------------------------------------------------------------- */
bool parse_md5(const char *src, char dest[16]) {
  for (int i = 0; i < 16; ++i) {

    /* ---- high nibble ------------------------------------------------ */
    int c = (unsigned char)(*src++);
    if (isdigit(c))
      dest[i] = (char)c;

  }

  return true;
}
