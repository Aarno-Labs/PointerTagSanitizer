/*
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/* PTSan replays pointer initializers before protecting these pages itself.
   Keep them out of loader-managed RELRO, which is protected before ctors. */
SECTIONS
{
  .ptsan_init_ro ALIGN(CONSTANT(MAXPAGESIZE)) :
  {
    HIDDEN(__ptsan_init_ro_start = .);
    *(.ptsan_init_ro .ptsan_init_ro.*)
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    HIDDEN(__ptsan_init_ro_end = .);
  }
}
INSERT AFTER .data;
