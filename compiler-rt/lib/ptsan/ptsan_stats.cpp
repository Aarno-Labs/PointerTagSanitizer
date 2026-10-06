//===-- ptsan_stats.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// PTSan runtime statistics counters.
//
//===----------------------------------------------------------------------===//

#include "ptsan_bounds.h"
#include "ptsan_drift.h"
#include "ptsan_thread.h"
#include "ptsan_ptr_utils.h"
#include "ptsan_stats.h"


#ifdef PTSAN_VALIDATE
volatile u64 ptsan_local_allocas = 0;
volatile u64 ptsan_total_allocas = 0;
volatile u64 ptsan_called_checks[END_TYPE];
volatile u64 ptsan_n_alloca_fn_calls[5];

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_log_alloca_info(char *FnName, u16 id, bool IsLocal, bool ShortPath) {
  // Update starts
  __atomic_fetch_add(&ptsan_total_allocas, 1, __ATOMIC_RELAXED);
  if (IsLocal) {
    __atomic_fetch_add(&ptsan_local_allocas, 1, __ATOMIC_RELAXED);
  }

  if (id == 0) {
    Printf("Allocas should not have id = 0!\n");
    internal__exit(1);
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_validate_function_start() {
  if(ptsan_in_libc != 0) {
    Printf("All user functions should be called with ptsan_in_libc = 0\n");
    internal__exit(1);
  }
}


extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_log_check_called(u32 CheckType) {
  __atomic_fetch_add(&(ptsan_called_checks[CheckType]), 1, __ATOMIC_RELAXED);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE NOINLINE __attribute__((optnone))
void ptsan_validate_arithmetic(u64 A, u64 B) {
  u16 idA = get_id(A);
  u16 idB = get_id(B);
  if (A == 0 || B == 0 || A == (u64)-1 || B == (u64)-1) {
    return;
  }
  if (idA != idB) {
    Printf("WARNING: Pointer-arith-or-compare pointers with different IDs: (%d and %d) "
           "You will want to run this app with -mllvm -ptsan-clear-id-on-arith\n",
           idA, idB);
  }
}
#endif
