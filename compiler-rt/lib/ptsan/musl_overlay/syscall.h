//===-- syscall.h - PTSan overlay for musl syscalls -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header is injected while building x86-64 or AArch64 musl. It routes raw Linux
// syscalls through a fixed-width PTSan boundary. Cancellable calls retain IDs
// through musl's C dispatch: its fallback reaches this overlay, while its
// assembly path is intercepted by the __syscall_cp_asm linker wrapper.
//
// Linux has at most six syscall argument registers on both x86-64 and AArch64.
// Keeping this boundary fixed-width makes eventual AArch64 support a local
// architecture change rather than an ABI change.
//
//===----------------------------------------------------------------------===//

#ifndef PTSAN_MUSL_OVERLAY_SYSCALL_H
#define PTSAN_MUSL_OVERLAY_SYSCALL_H

#include_next "syscall.h"

// Musl's vDSO entry points are kernel-provided, uninstrumented code. They
// cannot dereference PTSan-tagged output pointers, so use the intercepted
// syscall fallbacks in instrumented musl for now.
#undef VDSO_CGT_SYM
#undef VDSO_GETCPU_SYM

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "PTSan instrumented-musl syscall overlay currently supports x86-64 and AArch64"
#endif

typedef long (*ptsan_musl_syscall_executor)(
    syscall_arg_t number, syscall_arg_t argument1, syscall_arg_t argument2,
    syscall_arg_t argument3, syscall_arg_t argument4, syscall_arg_t argument5,
    syscall_arg_t argument6);

long ptsan_musl_syscall(ptsan_musl_syscall_executor executor,
                        syscall_arg_t number, syscall_arg_t argument1,
                        syscall_arg_t argument2, syscall_arg_t argument3,
                        syscall_arg_t argument4, syscall_arg_t argument5,
                        syscall_arg_t argument6);

#define __ptsan_syscall0(n)                                                    \
  ptsan_musl_syscall((__syscall6), (n), 0, 0, 0, 0, 0, 0)
#define __ptsan_syscall1(n, a)                                                 \
  ptsan_musl_syscall((__syscall6), (n), __scc(a), 0, 0, 0, 0, 0)
#define __ptsan_syscall2(n, a, b)                                              \
  ptsan_musl_syscall((__syscall6), (n), __scc(a), __scc(b), 0, 0, 0, 0)
#define __ptsan_syscall3(n, a, b, c)                                           \
  ptsan_musl_syscall((__syscall6), (n), __scc(a), __scc(b), __scc(c), 0, 0, 0)
#define __ptsan_syscall4(n, a, b, c, d)                                        \
  ptsan_musl_syscall((__syscall6), (n), __scc(a), __scc(b), __scc(c),          \
                     __scc(d), 0, 0)
#define __ptsan_syscall5(n, a, b, c, d, e)                                     \
  ptsan_musl_syscall((__syscall6), (n), __scc(a), __scc(b), __scc(c),          \
                     __scc(d), __scc(e), 0)
#define __ptsan_syscall6(n, a, b, c, d, e, f)                                  \
  ptsan_musl_syscall((__syscall6), (n), __scc(a), __scc(b), __scc(c),          \
                     __scc(d), __scc(e), __scc(f))

// Linux has no seven-argument syscall ABI. Keep musl's generic compatibility
// macro intact; it is not a real syscall route on x86-64 or AArch64 Linux.
#define __ptsan_syscall7(n, a, b, c, d, e, f, g)                               \
  (__syscall7)(n, __scc(a), __scc(b), __scc(c), __scc(d), __scc(e), __scc(f),  \
               __scc(g))

// Keep musl's __syscall_cpN macros: checking belongs at the outgoing syscall,
// not before the cancellation machinery chooses its execution path.

static inline long __ptsan_alt_socketcall(
    int syscall_number, int socketcall_number, int cancellable,
    syscall_arg_t argument1, syscall_arg_t argument2, syscall_arg_t argument3,
    syscall_arg_t argument4, syscall_arg_t argument5, syscall_arg_t argument6) {
  long result = cancellable
      ? (__syscall_cp)(syscall_number, argument1, argument2, argument3,
                       argument4, argument5, argument6)
      : ptsan_musl_syscall((__syscall6), syscall_number, argument1, argument2,
                           argument3, argument4, argument5, argument6);
  if (result != -ENOSYS)
    return result;
  return __alt_socketcall(syscall_number, socketcall_number, cancellable,
                          argument1, argument2, argument3, argument4, argument5,
                          argument6);
}

#undef __socketcall
#define __socketcall(nm, a, b, c, d, e, f)                                     \
  __ptsan_alt_socketcall(SYS_##nm, __SC_##nm, 0, __scc(a), __scc(b), __scc(c), \
                         __scc(d), __scc(e), __scc(f))
#undef __socketcall_cp
#define __socketcall_cp(nm, a, b, c, d, e, f)                                  \
  __ptsan_alt_socketcall(SYS_##nm, __SC_##nm, 1, __scc(a), __scc(b), __scc(c), \
                         __scc(d), __scc(e), __scc(f))

#undef __syscall
#define __syscall(...) __SYSCALL_DISP(__ptsan_syscall, __VA_ARGS__)

#endif // PTSAN_MUSL_OVERLAY_SYSCALL_H
