#type: ignore
# -*- Python -*-

import os
import subprocess
import sys

# Setup config name.
config.name = "PointerTagSanitizer" + config.name_suffix

# Setup source root.
config.test_source_root = os.path.dirname(__file__)
config.scripts_root = os.path.join(config.test_source_root, "../../lib/ptsan/scripts")


# Setup default compiler flags used with -fsanitize=ptsan option.
clang_ptsan_cflags = [
    "-fsanitize=ptsan", "-g", "-mllvm", "-ptsan-runtime-validate",
    "-fno-discard-value-names", "-mllvm", "-ptsan-print-stats"] + [config.target_cflags]

clang_ptsan_cxxflags = config.cxx_mode_flags + clang_ptsan_cflags


def build_invocation(compile_flags):
    return " " + " ".join([config.clang] + compile_flags) + " "

config.substitutions.append(("%clang_raw ", config.clang))
config.substitutions.append(("%clangxx_raw ", build_invocation(config.cxx_mode_flags)))
config.substitutions.append(("%clang_ptsan ", build_invocation(clang_ptsan_cflags)))
config.substitutions.append(("%clangxx_ptsan ", build_invocation(clang_ptsan_cxxflags)))
config.substitutions.append(
    ("%opt ", " -O2 "))
config.substitutions.append(("%exe", "a.out"))
config.substitutions.append(("%ptsan_patcher", os.path.join(config.scripts_root, "ptsan_patcher.py")))
config.substitutions.append(("%opt_ptsan ",
                             "opt -passes='ptsan' -S "))
config.substitutions.append(("%llc", "llc "))

# Default test suffixes.
config.suffixes = [".c", ".cpp", ".S"]
config.excludes.add('ManualTests')
config.excludes.add('Inputs')
config.excludes.add('lib')

def host_supports_x86_lam_u48():
    if config.host_os != "Linux":
        return False
    if config.host_arch != "x86_64":
        return False

    probe = r"""
import ctypes
import sys

SYS_arch_prctl = 158
ARCH_GET_MAX_TAG_BITS = 0x4003

libc = ctypes.CDLL(None, use_errno=True)
max_bits = ctypes.c_ulong(0)
ret = libc.syscall(SYS_arch_prctl, ARCH_GET_MAX_TAG_BITS, ctypes.byref(max_bits))
sys.exit(0 if ret == 0 and max_bits.value >= 15 else 1)
"""

    try:
        return subprocess.run(
            [sys.executable, "-c", probe],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode == 0
    except Exception:
        return False

if host_supports_x86_lam_u48():
    config.available_features.add("ptsan-lam-u48-runtime")

# PTSan tests are currently supported on Linux only.
if not (config.host_os in ["Linux"] and config.target_arch in ["aarch64", "x86_64", "loongarch64"]):
    config.unsupported = True
