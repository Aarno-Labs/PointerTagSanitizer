// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: #define MyType__alloc() \
// CHECK: ((MyType*) dds_alloc (sizeof (MyType)));
// CHECK: #define MyType_free(d,o) \
// CHECK: dds_sample_free ((d), &MyType_desc, (o))

// tprintf_demo.c
#include <stdio.h>
#include <stdarg.h>

#define NOINLINE __attribute__((noinline))

static NOINLINE FILE *get_debugfp(void) {
  return stdout; // or stderr
}

// Trace printf.
static NOINLINE void tprintf(const char *format, ...) {
  FILE *f = get_debugfp();
  va_list args;           // variable args
  va_start(args, format); // variable list
  vfprintf(f, format, args);
  va_end(args);
}

int NOINLINE main(void) {
  const char *fmt =
    "extern const dds_topic_descriptor_t %1$s_desc;\n"
    "\n"
    "#define %1$s__alloc() \\\n"
    "((%1$s*) dds_alloc (sizeof (%1$s)));\n"
    "\n"
    "#define %1$s_free(d,o) \\\n"
    "dds_sample_free ((d), &%1$s_desc, (o))\n"
    "\n";

  tprintf(fmt, "MyType");   // %1$s expands to "MyType" everywhere
  return 0;
}
