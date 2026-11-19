//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.dir
// REQUIRES: x86_64-target-arch
// RUN: %ptsan_musl_strict -I%S/.. -O2 -mllvm -ptsan-abort-on-error -mllvm -ptsan-global-section=ptsan_records -mllvm -ptsan-global-section=ptsan_dummy -c %t.dir/owner.c -o %t.owner.o
// RUN: %ptsan_musl_strict -I%S/.. -O2 -mllvm -ptsan-abort-on-error %t.dir/main.c %t.dir/read.c %t.owner.o -o %t
// RUN: %run %t 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %run %t after 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %run %t before 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %run %t member 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %run %t direct 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %ptsan_musl_strict -I%S/.. -mllvm -ptsan-global-section=ptsan_records -c %t.dir/duplicate.c -o %t.duplicate.o
// RUN: not %ptsan_musl_strict -I%S/.. %t.owner.o %t.duplicate.o %t.dir/main.c %t.dir/read.c -o %t.duplicate 2>&1 | FileCheck %s --check-prefix=DUPLICATE
//
// RUN: %ptsan_musl_id32_strict -I%S/.. -O2 -mllvm -ptsan-abort-on-error -mllvm -ptsan-global-section=ptsan_records -mllvm -ptsan-global-section=ptsan_dummy -c %t.dir/owner.c -o %t.id32.owner.o
// RUN: %ptsan_musl_id32_strict -I%S/.. -O2 -mllvm -ptsan-abort-on-error %t.dir/main.c %t.dir/read.c %t.id32.owner.o -o %t.id32
// RUN: %run %t.id32 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %run %t.id32 after 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %run %t.id32 before 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %run %t.id32 member 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %run %t.id32 direct 2>&1 | FileCheck %s --check-prefix=BAD

// GOOD-NOT: Found bad
// GOOD: global-section-ok
// GOOD-NOT: Found bad
// BAD: Found bad POINT access
// BAD-NOT: invalid-read-completed
// DUPLICATE: {{multiple definition|duplicate symbol}}{{.*}}__start_ptsan_records.ptsan.ptr

//--- owner.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stddef.h>

static unsigned char First[16] __attribute__((section("ptsan_records"), used)) = {11};
extern const unsigned char __start_ptsan_records;
extern const unsigned char __stop_ptsan_records;

const unsigned char *first_object(void) { return First; }
const unsigned char *owner_start(void) { return &__start_ptsan_records; }
const unsigned char *owner_stop(void) { return &__stop_ptsan_records; }

// Match Protobuf's single dummy entry and its backward scan in a constructor.
struct Tail {
  const void **target;
  size_t size;
};
struct Dummy {
  const void *instance;
  struct Tail tail;
};
static struct Dummy Dummy __attribute__((section("ptsan_dummy"), used)) = {
    0, {&Dummy.instance, sizeof(Dummy)}};
extern const char __start_ptsan_dummy;
extern const char __stop_ptsan_dummy;

__attribute__((constructor(101))) static void initialize_dummy(void) {
  const char *start = &__start_ptsan_dummy;
  const char *end = &__stop_ptsan_dummy;
  unsigned count = 0;
  while (start != end) {
    const struct Tail *tail = (const struct Tail *)end - 1;
    end -= tail->size;
    *tail->target = end;
    ++count;
  }
  assert(count == 1);
}

void check_dummy(void) {
  assert(get_id(Dummy.instance) == get_id(&__start_ptsan_dummy));
  assert(get_id(Dummy.instance) != 0);
  assert(ptsan_get_address(Dummy.instance) == ptsan_get_address(&Dummy));
  assert(ptsan_get_size(get_id(Dummy.instance)) == sizeof(Dummy));
}

//--- main.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stddef.h>

static unsigned char Second[16] __attribute__((section("ptsan_records"), used)) = {22};
extern const unsigned char __start_ptsan_records;
extern const unsigned char __stop_ptsan_records;
const unsigned char *first_object(void);
const unsigned char *owner_start(void);
const unsigned char *owner_stop(void);
unsigned char read_offset(const unsigned char *, ptrdiff_t);
void check_dummy(void);

int main(int argc, char **argv) {
  const unsigned char *start = &__start_ptsan_records;
  const unsigned char *stop = &__stop_ptsan_records;
  unsigned id = get_id(start);
  assert(id != 0 && id != ptsan_default_id());
  assert(id == get_id(stop));
  assert(id == get_id(owner_start()) && id == get_id(owner_stop()));
  assert(ptsan_get_size(id) == 32);
  assert(ptsan_get_address(stop) - ptsan_get_address(start) == 32);
  assert(get_id(Second) != id && get_id(first_object()) != id);
  assert(ptsan_get_size(get_id(Second)) == sizeof(Second));
  assert(ptsan_get_size(get_id(first_object())) == 16);
  ptrdiff_t first = ptsan_get_address(first_object()) - ptsan_get_address(start);
  ptrdiff_t second = ptsan_get_address(Second) - ptsan_get_address(start);
  assert(read_offset(start, first) == 11);
  assert(read_offset(start, second) == 22);
  assert(read_offset(stop, -1) == 0);
  check_dummy();
  if (argc > 1) {
    if (argv[1][0] == 'a') {
      (void)read_offset(stop, 0);
    } else if (argv[1][0] == 'b') {
      (void)read_offset(start, -1);
    } else if (argv[1][0] == 'd') {
      // The boundary's C declaration must not justify eliding this check.
      (void)*(const volatile unsigned char *)&__stop_ptsan_records;
    } else {
      (void)read_offset(first_object(), 16);
    }
    puts("invalid-read-completed");
  }
  puts("global-section-ok");
}

//--- read.c
#include <stddef.h>
unsigned char read_offset(const unsigned char *base, ptrdiff_t offset) {
  return *(const volatile unsigned char *)(base + offset);
}

//--- duplicate.c
// No boundary declarations: the option must introduce the linker references.
