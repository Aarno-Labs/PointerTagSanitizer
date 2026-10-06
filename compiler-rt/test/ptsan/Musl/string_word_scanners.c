//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise public string and memory APIs whose musl implementations
// deliberately use word-at-a-time reads beyond an early terminating byte.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -fno-builtin -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-strlen 2> %t.strlen
// RUN: FileCheck %s < %t.strlen
// RUN: not %run %t bad-stpcpy-source 2> %t.stpcpy-source
// RUN: FileCheck %s < %t.stpcpy-source
// RUN: not %run %t bad-strcpy-dest 2> %t.strcpy-dest
// RUN: FileCheck %s < %t.strcpy-dest
// RUN: not %run %t bad-strchrnul 2> %t.strchrnul
// RUN: FileCheck %s < %t.strchrnul
// RUN: not %run %t bad-strchr 2> %t.strchr
// RUN: FileCheck %s < %t.strchr
// RUN: not %run %t bad-strlcpy-source 2> %t.strlcpy-source
// RUN: FileCheck %s < %t.strlcpy-source
// RUN: not %run %t bad-strlcpy-dest 2> %t.strlcpy-dest
// RUN: FileCheck %s < %t.strlcpy-dest
// RUN: not %run %t bad-memchr 2> %t.memchr
// RUN: FileCheck %s < %t.memchr
// RUN: not %run %t bad-memccpy-source 2> %t.memccpy-source
// RUN: FileCheck %s < %t.memccpy-source
// RUN: not %run %t bad-memccpy-dest 2> %t.memccpy-dest
// RUN: FileCheck %s < %t.memccpy-dest
// RUN: not %run %t bad-stpncpy-source 2> %t.stpncpy-source
// RUN: FileCheck %s < %t.stpncpy-source
// RUN: not %run %t bad-strncpy-dest 2> %t.strncpy-dest
// RUN: FileCheck %s < %t.strncpy-dest
// RUN: not %run %t bad-memcmp-left 2> %t.memcmp-left
// RUN: FileCheck %s < %t.memcmp-left
// RUN: not %run %t bad-memcmp-right 2> %t.memcmp-right
// RUN: FileCheck %s < %t.memcmp-right
// RUN: not %run %t bad-strcmp-left 2> %t.strcmp-left
// RUN: FileCheck %s < %t.strcmp-left
// RUN: not %run %t bad-strcmp-right 2> %t.strcmp-right
// RUN: FileCheck %s < %t.strcmp-right
// RUN: not %run %t bad-strncmp-left 2> %t.strncmp-left
// RUN: FileCheck %s < %t.strncmp-left
// RUN: not %run %t bad-strncmp-right 2> %t.strncmp-right
// RUN: FileCheck %s < %t.strncmp-right
//
//
// CHECK: Found bad

#define _GNU_SOURCE

#include "../Inputs/ptsan_test_interface.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static char *makeString(void) {
  char *string = (char *)malloc(2);
  if (string) {
    string[0] = 'A';
    string[1] = '\0';
  }
  return string;
}

static char *makeUnterminatedString(void) {
  char *string = (char *)malloc(1);
  if (string) {
    string[0] = 'A';
  }
  return string;
}

static int testGood(void) {
  char *source = makeString();
  char *dest = (char *)malloc(2);
  if (!source || !dest) {
    return 1;
  }

  if (strlen(source) != 1) {
    return 2;
  }

  char *end = stpcpy(dest, source);
  if (end != dest + 1 || get_id(end) != get_id(dest) || end[0] != '\0') {
    return 3;
  }

  dest[0] = '\0';
  if (strcpy(dest, source) != dest || dest[0] != 'A' || dest[1] != '\0') {
    return 4;
  }

  if (strchrnul(source, 'Z') != source + 1) {
    return 5;
  }
  if (strchr(source, 'Z') != NULL) {
    return 6;
  }

  dest[0] = '\0';
  if (strlcpy(dest, source, 2) != 1 || dest[0] != 'A' || dest[1] != '\0') {
    return 7;
  }

  if (memchr(source, 'A', 16) != source) {
    return 8;
  }

  dest[0] = '\0';
  end = memccpy(dest, source, 'A', 16);
  if (end != dest + 1 || get_id(end) != get_id(dest) || dest[0] != 'A') {
    return 9;
  }

  char padded[8];
  end = stpncpy(padded, source, sizeof(padded));
  if (end != padded + 1 || get_id(end) != get_id(padded) ||
      padded[0] != 'A' || padded[1] != '\0' || padded[7] != '\0') {
    return 10;
  }
  if (strncpy(padded, source, sizeof(padded)) != padded || padded[0] != 'A' ||
      padded[1] != '\0' || padded[7] != '\0') {
    return 11;
  }

  char *same = makeString();
  char *different = makeUnterminatedString();
  if (!same || !different) {
    return 12;
  }
  different[0] = 'B';

  if (memcmp(source, same, 2) != 0 || memcmp(source, different, 1) >= 0) {
    return 13;
  }
  if (strcmp(source, same) != 0 || strcmp(source, different) >= 0) {
    return 14;
  }
  if (strncmp(source, same, 2) != 0 || strncmp(source, different, 8) >= 0) {
    return 15;
  }

  const unsigned char high_memory[] = {0x80};
  const unsigned char low_memory[] = {0x7f};
  const char high_string[] = {(char)0x80, '\0'};
  const char low_string[] = {(char)0x7f, '\0'};
  if (memcmp(high_memory, low_memory, 1) <= 0 ||
      strcmp(high_string, low_string) <= 0 ||
      strncmp(high_string, low_string, 1) <= 0) {
    return 16;
  }

  free(different);
  free(same);
  free(dest);
  free(source);
  return 0;
}

static int testBadStrlen(void) {
  char *source = makeUnterminatedString();
  if (!source) {
    return 1;
  }
  (void)strlen(source);
  return 0;
}

static int testBadStpcpySource(void) {
  char *source = makeUnterminatedString();
  char *dest = (char *)malloc(16);
  if (!source || !dest) {
    return 1;
  }
  (void)stpcpy(dest, source);
  return 0;
}

static int testBadStrcpyDest(void) {
  char *source = makeString();
  char *dest = (char *)malloc(1);
  if (!source || !dest) {
    return 1;
  }
  (void)strcpy(dest, source);
  return 0;
}

static int testBadStrchrnul(void) {
  char *source = makeUnterminatedString();
  if (!source) {
    return 1;
  }
  (void)strchrnul(source, 'Z');
  return 0;
}

static int testBadStrchr(void) {
  char *source = makeUnterminatedString();
  if (!source) {
    return 1;
  }
  (void)strchr(source, 'Z');
  return 0;
}

static int testBadStrlcpySource(void) {
  char *source = makeUnterminatedString();
  if (!source) {
    return 1;
  }
  (void)strlcpy(NULL, source, 0);
  return 0;
}

static int testBadStrlcpyDest(void) {
  char *source = makeString();
  char *dest = (char *)malloc(1);
  if (!source || !dest) {
    return 1;
  }
  (void)strlcpy(dest, source, 2);
  return 0;
}

static int testBadMemchr(void) {
  char *source = makeUnterminatedString();
  if (!source) {
    return 1;
  }
  (void)memchr(source, 'Z', 2);
  return 0;
}

static int testBadMemccpySource(void) {
  char *source = makeUnterminatedString();
  char *dest = (char *)malloc(2);
  if (!source || !dest) {
    return 1;
  }
  (void)memccpy(dest, source, 'Z', 2);
  return 0;
}

static int testBadMemccpyDest(void) {
  char *source = makeString();
  char *dest = (char *)malloc(1);
  if (!source || !dest) {
    return 1;
  }
  (void)memccpy(dest, source, '\0', 2);
  return 0;
}

static int testBadStpncpySource(void) {
  char *source = makeUnterminatedString();
  char *dest = (char *)malloc(2);
  if (!source || !dest) {
    return 1;
  }
  (void)stpncpy(dest, source, 2);
  return 0;
}

static int testBadStrncpyDest(void) {
  char *source = makeString();
  char *dest = (char *)malloc(1);
  if (!source || !dest) {
    return 1;
  }
  (void)strncpy(dest, source, 2);
  return 0;
}

static int testBadMemcmp(bool bad_left) {
  char *short_buffer = makeUnterminatedString();
  char *long_buffer = makeString();
  if (!short_buffer || !long_buffer) {
    return 1;
  }
  if (bad_left) {
    (void)memcmp(short_buffer, long_buffer, 2);
  } else {
    (void)memcmp(long_buffer, short_buffer, 2);
  }
  return 0;
}

static int testBadStrcmp(bool bad_left) {
  char *unterminated = makeUnterminatedString();
  char *terminated = makeString();
  if (!unterminated || !terminated) {
    return 1;
  }
  if (bad_left) {
    (void)strcmp(unterminated, terminated);
  } else {
    (void)strcmp(terminated, unterminated);
  }
  return 0;
}

static int testBadStrncmp(bool bad_left) {
  char *unterminated = makeUnterminatedString();
  char *terminated = makeString();
  if (!unterminated || !terminated) {
    return 1;
  }
  if (bad_left) {
    (void)strncmp(unterminated, terminated, 2);
  } else {
    (void)strncmp(terminated, unterminated, 2);
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (strcmp(argv[1], "good") == 0) {
    return testGood();
  }
  if (strcmp(argv[1], "bad-strlen") == 0) {
    return testBadStrlen();
  }
  if (strcmp(argv[1], "bad-stpcpy-source") == 0) {
    return testBadStpcpySource();
  }
  if (strcmp(argv[1], "bad-strcpy-dest") == 0) {
    return testBadStrcpyDest();
  }
  if (strcmp(argv[1], "bad-strchrnul") == 0) {
    return testBadStrchrnul();
  }
  if (strcmp(argv[1], "bad-strchr") == 0) {
    return testBadStrchr();
  }
  if (strcmp(argv[1], "bad-strlcpy-source") == 0) {
    return testBadStrlcpySource();
  }
  if (strcmp(argv[1], "bad-strlcpy-dest") == 0) {
    return testBadStrlcpyDest();
  }
  if (strcmp(argv[1], "bad-memchr") == 0) {
    return testBadMemchr();
  }
  if (strcmp(argv[1], "bad-memccpy-source") == 0) {
    return testBadMemccpySource();
  }
  if (strcmp(argv[1], "bad-memccpy-dest") == 0) {
    return testBadMemccpyDest();
  }
  if (strcmp(argv[1], "bad-stpncpy-source") == 0) {
    return testBadStpncpySource();
  }
  if (strcmp(argv[1], "bad-strncpy-dest") == 0) {
    return testBadStrncpyDest();
  }
  if (strcmp(argv[1], "bad-memcmp-left") == 0) {
    return testBadMemcmp(true);
  }
  if (strcmp(argv[1], "bad-memcmp-right") == 0) {
    return testBadMemcmp(false);
  }
  if (strcmp(argv[1], "bad-strcmp-left") == 0) {
    return testBadStrcmp(true);
  }
  if (strcmp(argv[1], "bad-strcmp-right") == 0) {
    return testBadStrcmp(false);
  }
  if (strcmp(argv[1], "bad-strncmp-left") == 0) {
    return testBadStrncmp(true);
  }
  if (strcmp(argv[1], "bad-strncmp-right") == 0) {
    return testBadStrncmp(false);
  }
  return 2;
}
