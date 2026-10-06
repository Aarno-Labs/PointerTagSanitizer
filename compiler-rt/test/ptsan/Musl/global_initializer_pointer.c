//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// cdecl's generated lexer stores pointers into yycrank in the global yysvec
// initializer, then mixes a loaded initializer pointer with a direct yycrank
// reference. PTSan must replay the initializer after assigning global tags so
// that both pointers carry the same ID.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

struct Transition {
  short verify;
  short advance;
};

struct State {
  struct Transition *offset;
};

struct Transition transitions[4] = {
    {1, 10},
    {2, 20},
    {3, 30},
    {4, 40},
};

struct State states[2] = {
    {transitions - 1},
    {transitions},
};

volatile unsigned state_index;
volatile int result;

int main(void) {
  // Keep the selected initializer opaque to the optimizer, as yylook does.
  struct Transition *embedded = states[state_index].offset;

  // This is the pointer expression used by cdecl's generated lexer.
  struct Transition *computed = transitions + (transitions - embedded);
  result = computed->advance;
  return result != 20;
}
