#include "ptsan.h"
#include "ptsan_ptr_utils.h"
#include "sanitizer_common/sanitizer_internal_defs.h"

#include <ucontext.h>


#define NUM_SAVED_IDS 32
// Saved IDs per function. Index is arg index when relevant.
thread_local u16 SavedIDs[NUM_SAVED_IDS];

#define SAVE_AND_CLEAR(elt, idx) \
  CHECK(idx < NUM_SAVED_IDS); \
  \
  SavedIDs[idx] = get_id(elt); \
  elt = clear_id(elt); \

#define RESTORE_ID(elt, idx) \
  (elt) = set_id(elt, SavedIDs[idx]);


extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_strip_makecontext_before(
  ucontext_t *__ucp, void (*__func) (void), int __argc, ...) {
  SAVE_AND_CLEAR(clear_id(__ucp)->uc_stack.ss_sp, 0);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_strip_makecontext_after(
  ucontext_t *__ucp, void (*__func) (void), int __argc, ...) {
  RESTORE_ID(clear_id(__ucp)->uc_stack.ss_sp, 0);
}
