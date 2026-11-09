//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Unlike ID16 stripping, ID32 preserves this particular opaque sentinel.
// Keep this passing case separate from the ID16 expected failure.
// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O1 -g -fno-discard-value-names \
// RUN:   -mllvm -ptsan-reoptimize=0 -o %t %S/strip_opaque_all_ones_false_positive.c
// RUN: %run %t
