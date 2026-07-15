
#pragma once
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_libc.h"

// We enforce that these match in ptsan_init
//#define PTSAN_NO_IDS
SANITIZER_INTERFACE_ATTRIBUTE
extern int ClNoIDs;

//#define PTSAN_NO_STRIP
SANITIZER_INTERFACE_ATTRIBUTE
extern int ClNoStrip;

//#define PTSAN_LAM
SANITIZER_INTERFACE_ATTRIBUTE
extern int ClLAM;

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClValidate;

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClPrintStats;

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClStrictMode;

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClUseReturnedMallocSize;

SANITIZER_INTERFACE_ATTRIBUTE
extern unsigned ClStackIDsPerBucket;

SANITIZER_INTERFACE_ATTRIBUTE
extern unsigned ClStackIDThreadBuckets;

using namespace __sanitizer;

extern "C" {

namespace __ptsan {
  extern bool ptsan_initialized;
  extern bool ptsan_init_is_running;
} // namespace __ptsan

// Initialise ptsan interceptors.
// A call to this method is added to the preinit array on Linux systems.
SANITIZER_INTERFACE_ATTRIBUTE void __ptsan_init();

SANITIZER_INTERFACE_ATTRIBUTE int ptsan_enable_lam_u48();

void InitializeIDs();

} //extern "C"
