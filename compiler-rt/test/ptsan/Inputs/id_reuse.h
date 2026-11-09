//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Drive both direct and batched allocators until the requested ID is reused.
// Keep the loop bounded so a lost ID fails instead of hanging the test.
#include "ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>

static void *allocate_reusing_id(PTSanID id) {
  for (unsigned i = 0; i != 32768; ++i) {
    void *p = malloc(16);
    assert(p);
    if (get_id(p) == id) {
      return p;
    }
    free(p);
  }
  assert(!"retired ID was not reused");
  return NULL;
}
