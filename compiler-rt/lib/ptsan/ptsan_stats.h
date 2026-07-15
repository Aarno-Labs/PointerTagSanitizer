#pragma once

#include "ptsan.h"
#include "ptsan_constants.h"

#ifdef PTSAN_VALIDATE
// Keep this in sync with PTSanUtils.h
#define PTSAN_CHK_TY_LIST(X) \
  X(NO_CHECK,         0)     \
  X(POINT,            1)     \
  X(RANGE,            2)     \
  X(LOOP,             3)     \
  X(PREANALYZED_LOOP, 4)     \
  X(COMPOUND,         5)     \
  X(EXOTIC,           6)     \
  X(END_TYPE,         7)

enum CHK_TY {
#define X(name, value) name = value,
PTSAN_CHK_TY_LIST(X)
#undef X
};

#define X(name, value)                         \
  case name:                                   \
    return #name;

static inline const char * ChkTyToString(CHK_TY Ty) {
  switch (Ty) {
    PTSAN_CHK_TY_LIST(X)
  }
}
#undef X

extern volatile u64 ptsan_local_allocas;
extern volatile u64 ptsan_total_allocas;
extern volatile u64 ptsan_n_alloca_fn_calls[5];
extern volatile u64 ptsan_called_checks[END_TYPE];

#endif