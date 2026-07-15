#include "sanitizer_common/sanitizer_internal_defs.h"
#include "ptsan.h"

#if SANITIZER_CAN_USE_PREINIT_ARRAY

// This section is linked into the main executable when -fsanitize=realtime is
// specified to perform initialization at a very early stage.
__attribute__((section(".preinit_array"), used)) static auto preinit =
    __ptsan_init;

#endif
