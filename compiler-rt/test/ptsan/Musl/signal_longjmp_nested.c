//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t 0
// RUN: %run %t 1
// RUN: %ptsan_musl_id32_strict %s -o %t.id32
// RUN: %run %t.id32 0
// RUN: %run %t.id32 1
#define JUMP_TARGET 2
#include "Inputs/signal_longjmp.h"
