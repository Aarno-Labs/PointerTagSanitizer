//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Both the argv array and its string pointers must receive IDs. Read each
// string in a separate invocation to cover both startup paths in strict mode.
// Volatile preserves the character access without a string-library call.
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 2)
    _Exit(2);
#ifdef READ_ARGV1
  unsigned char c = *(volatile unsigned char *)argv[1];
  _Exit(c != 'a');
#else
  unsigned char c = *(volatile unsigned char *)argv[0];
  _Exit(c == '\0');
#endif
}
