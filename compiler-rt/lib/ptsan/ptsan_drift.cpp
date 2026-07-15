/* Drift-specific ptsan machinery

This lives in a seperate file so I can easily remove it for the paper.
*/
#include "ptsan.h"
#include "ptsan_drift.h"

#include "sanitizer_common/sanitizer_stacktrace.h"


void __sanitizer::BufferedStackTrace::UnwindImpl(uptr pc, uptr bp,
                                                 void *context,
                                                 bool request_fast,
                                                 u32 max_depth) {
    return Unwind(max_depth, pc, bp, context, 0, 0, false);
  }

extern "C" {
  SANITIZER_INTERFACE_ATTRIBUTE
  thread_local jmp_buf ptsan_jmp_env;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
jmp_buf *ptsan_get_jmp_env() {
  static bool announced_patch = false;
  if (!announced_patch) {
    Printf("Ptsan's get jmp env called. This is the patched version!\n");
    announced_patch = true;
  }

  return &ptsan_jmp_env;
}


void ptsan_handle_error(u32 check_id, u64 function_id, u64 ptr_id,
                  const char * type) {
  switch (ClPatchAllWith){
    case 0:
      // By default we don't do anything here
      return;
    case 1:
      Printf("Patch applied. Exiting!\n");
      Die();
    case 2:
      Printf("Patch applied. Jumping!\n");
      longjmp(ptsan_jmp_env, 1);
    default:
      Printf("Unknown patch type!. Exiting!\n");
      Abort();
  }
}
