#!/usr/bin/env python3
# ===- compiler-rt/lib/ptsan/scripts/ptsan_patcher.py ----------------------===#
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# ===------------------------------------------------------------------------===#
"""
patch_id.py : replace one PatchID entry in-place inside a binary.

    PatchID  { u32 check_id; u64 function_id; }   // C struct
              └─4─┘            └──── 8  ───┘
              (little-endian)   (little-endian)

The script exits with status 0 if exactly one occurrence was replaced,
status >0 otherwise (not found / multiple matches).

"""
import argparse
import sys
import struct
import json
from dataclasses import dataclass
from typing import Any, Dict, Sequence

MaxPatchedCalls = 8
BaseCallID = 0xdea0
BaseFnId = 0xdeadbee0

@dataclass
class PatchSpec:
  check: int
  type: int
  function: int

  def to_bytes(self):
    return struct.pack('<IIQ', self.check, self.type, self.function)


def replace_patch(blob: bytearray, old: PatchSpec, new: PatchSpec) -> bytearray:
  old_bytes = old.to_bytes()
  new_bytes = new.to_bytes()

  idx = blob.find(old_bytes)
  if idx == -1:
    print("pattern not found", file=sys.stderr)
    return None
  if blob.find(old_bytes, idx + 1) != -1:
    raise RuntimeError("pattern occurs more than once -- aborting")

  # replace
  blob[idx:idx+len(old_bytes)] = new_bytes
  return blob

def patch_bug(blob: bytearray, spec: PatchSpec)-> bytearray:
  for i in range(MaxPatchedCalls):
    oldSpec = PatchSpec(BaseCallID + i, BaseCallID + 1, BaseFnId + i)
    newBlob = replace_patch(blob, oldSpec, spec)
    if (newBlob is not None):
      print(f"Successfully patched with slot {i}")
      return blob
  raise RuntimeError("Failed to find any patch points")

def parse_args():
  ap = argparse.ArgumentParser()
  ap.add_argument('binary', help='binary to patch')
  ap.add_argument('bugs', help='binary to patch')
  ap.add_argument('--patch-type', help='type. 1 is die, 2 is jmp', type=int, required=True)
  ap.add_argument(
    "--patch-all",
    help="Patch all, not just the first",
    action=argparse.BooleanOptionalAction,
    default=False
  )
  return ap.parse_args()

def main(args: argparse.Namespace):
  with open(args.bugs, 'r') as f:
    bugs = json.load(f)["bugs"]

  # read entire file
  with open(args.binary, 'rb') as f:
    blob = bytearray(f.read())

  for bug in bugs:
    spec = PatchSpec(int(bug["call_id"]), args.patch_type, int(bug["func_id"]))
    blob = patch_bug(blob, spec)
    if (not args.patch_all):
      break

  with open(args.binary, 'r+b') as f:
    f.seek(0)
    f.write(blob)

  return 0

if __name__ == '__main__':
  args = parse_args()
  sys.exit(main(args))
