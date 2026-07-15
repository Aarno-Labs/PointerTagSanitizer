#pragma once

#include "ptsan.h"
#include "sanitizer_common/sanitizer_internal_defs.h"

#include <setjmp.h>



void ptsan_handle_error(u32 check_id, u64 function_id, u64 ptr_id,
                  const char * type);

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClPatchAllWith;
