# PTSan: A Practical Memory Safety Sanitizer with Pointer-Object Authority

This repository is a fork of LLVM 19 implementing **PTSan**, an LLVM
sanitizer that makes pointer-based memory-safety checking practical
for existing C/C++ programs developed by [Aarno
Labs](https://www.aarno-labs.com). Protected code is recompiled with
`-fsanitize=ptsan` and runs unchanged; no source modifications are
required.

PTSan stores a compact object identifier in the upper 16 bits of each
pointer and keeps every object's base and size in small, fixed runtime
tables indexed by that identifier. Before a memory access, PTSan
checks that the entire access range lies within the bounds of the
object the pointer was derived from, not merely that the target
address is mapped or unpoisoned. When an object is freed, its table
entry is cleared, so stale dereferences, double frees, and invalid
frees are also detected.

For questions please email: [ptsan@aarno-labs.com](mailto:ptsan@aarno-labs.com)

## Why pointer-object authority?

Location-based sanitizers such as AddressSanitizer ask "is this
address valid?", so an access that escapes its object and lands
inside *another live object* passes their checks. PTSan asks "is this
pointer entitled to access this range?", which catches:

- **Inter-object overflows**: offset or index errors that redirect an
  access into a different live allocation (the shape of real
  vulnerabilities such as CVE-2025-57807 in ImageMagick).
- **Non-linear out-of-bounds accesses** that jump over redzones.
- **Temporal errors**: use-after-free, double free, and invalid free,
  detected deterministically until the object's identifier is reused
  and probabilistically thereafter.

On MSET, an independent memory-safety benchmark suite, PTSan detects
all 126 inter-object spatial cases and all 40 temporal cases, matching
the strongest prior pointer-based system (the original publication of
SoftBound+CETS) at a fraction of its overhead. PTSan tracks allocation
objects (heap, stack, globals, mmap); intra-object field overflows are
out of scope.

## Performance

The measurements below describe the paper's evaluated release. They have not
been rerun for the subsequent implementation changes in this tree.

Because the identifier travels inside the pointer value, ordinary
dataflow propagates it for free, and checks remain plain LLVM IR that
the optimizer can hoist, merge, and elide:

- **SPEC CPU 2017**: 57% x86-64 and 55% ARM64 geomean runtime overhead
  in the default software-stripping configuration; 46% on x86 with
  Intel LAM hardware address masking.

- **LLVM test-suite MultiSource**
  - x86-64 (149 programs that execute > 10ms on baseline): 31.5%
    geomean; 27% with LAM.
  - ARM64 (148 programs that execute > 10ms on baseline): 41%
    geomean.

- **Memory**: 1.014x geomean peak physical memory on SPEC (worst case
  1.121x), versus 3.33x for ASan and 3.01x for RangeSanitizer. PTSan
  uses no redzones, no shadow memory, and no quarantine; its metadata
  is about 1.2 MiB of fixed tables.

- **Servers**: seven real-world server and cryptographic workloads
  (Redis, PostgreSQL, NGINX, OpenSSL, memcached, Apache, SQLite) with
  overheads ranging from near-native to 44% on x86-64, each within the
  2^16 identifier budget (peak demand 16,411).

## Requirements and configurations

- 64-bit Linux; program addresses must fit in the low 48 bits. PTSan
  is tested on Ubuntu 24.04 LTS.
- Use GNU ld (BFD) or LLD; the gold linker is unsupported.
- The default configuration supports 2^16 simultaneously live
  objects and runs on stock hardware and kernels. Peak live-object
  demand is cheap to measure in advance; workloads that exceed the
  budget can fail closed in strict mode.
- **Strict mode** is the security configuration: untagged pointers,
  the default identifier, and identifier exhaustion fail closed.
  Strict mode is currently experimental and does not work well with
  glibc. It should work with musl;
  the [build and dependency guide](clang/docs/PointerTagSanitizer.rst)
  describes the static instrumented musl configuration for strict mode.
  **Permissive mode** keeps such accesses running and is intended for
  bring-up and compatibility debugging, not for security deployments.
  Objects assigned the fallback identifier after ID exhaustion continue
  without object-specific bounds or lifetime checking.
- **PTSan-LAM** uses Intel Linear Address Masking to elide most
  software tag stripping (15-bit identifiers, 2^15 objects). It
  requires the LAM_U48 kernel variant, which is not in mainline
  Linux; our measurements use a patched Linux 6.1 kernel.
- **Optimized builds** are required for PTSan-enabled builds of your
  source code.  Please use at least `-O1`.

## Building and using PTSan

On Linux x86-64 or AArch64, build Clang, the PTSan runtime, and compatible
libc++, libc++abi, and libunwind libraries for use with the system glibc:

```sh
cmake -S llvm -B build -G Ninja \
  -C clang/cmake/caches/PTSan.cmake
ninja -C build
```

For C-only builds, use `ninja -C build clang ptsan` instead.

C++ applications require these compatible libraries to handle tagged pointers;
using the system C++ library with only `-fsanitize=ptsan` can crash at startup.
Compile with the generated configuration, which selects the matching headers,
libraries, and runtime library search path:

```sh
build/bin/clang++ --config=ptsan-glibc.cfg -O2 your_program.cpp -o your_program
```

For C applications:

```sh
build/bin/clang -O2 -fsanitize=ptsan your_program.c -o your_program
```

On a detected violation, PTSan reports the check site, the pointer's
object identifier, the access type, address, and size, then exits by default.
Compile with `-mllvm -ptsan-abort-on-error=0` to continue after reports.

Run the host-libc regression suite with:

```sh
ninja -C build check-ptsan
```

See the [build and dependency guide](clang/docs/PointerTagSanitizer.rst) for
musl, libc++, libc++abi, and libunwind recipes, matching configuration flags,
strict mode, the separate `check-ptsan-musl` suite, and current limitations.

## Paper

The design, security analysis, and full evaluation are described in:

> Eli Davis, Eric Lahtinen, and Michael Gordon.
> *[PTSan: A Practical Memory Safety Sanitizer for C/C++ with
> Pointer-Object Authority](https://arxiv.org/abs/2607.19246).* arXiv, 2026.

## License

This repository is a fork of LLVM 19. PTSan's own code — the
instrumentation passes, the runtime, and the test suite — is
Copyright (c) Aarno Labs LLC and is licensed under the same terms as
LLVM itself: the Apache License v2.0 with LLVM Exceptions
(SPDX: `Apache-2.0 WITH LLVM-exception`). These files carry SPDX
headers.

Five files in the glibc PTSan runtime are an exception. `ptsan_env.c`,
`ptsan_exec.c`, `ptsan_obstack.cpp`, `ptsan_qsort.c`, and
`ptsan_tsearch.c` — all in `compiler-rt/lib/ptsan/interceptors/` — are
derived from the GNU C Library and remain under the **GNU Lesser
General Public License v2.1 or later**, Copyright (c) Free Software
Foundation, Inc. PTSan supplies its own versions of these libc
functions because they invoke user-supplied callbacks, such as
`qsort`'s comparator, which must receive pointers that still carry
their PTSan tags. These five files are compiled into the glibc PTSan
runtime library, so binaries using that runtime contain LGPL-2.1-licensed
code and carry the corresponding obligations on redistribution. The
instrumented-musl runtime excludes these files.

We are working to create a single-license repository that does not
include the above 5 exceptions.  Please email us if the above
exceptions are an issue.

The PTSan test suite also includes cJSON
(`compiler-rt/test/ptsan/cjson/lib/`), which is under the MIT License,
Copyright (c) 2009-2017 Dave Gamble and cJSON contributors. It is used
only as a test input and is not linked into the compiler or the PTSan
runtime.

See `LICENSE.TXT` for the full license texts and the file inventory.

(below is the original LLVM root README.md)

# The LLVM Compiler Infrastructure

Welcome to the LLVM project!

This repository contains the source code for LLVM, a toolkit for the
construction of highly optimized compilers, optimizers, and run-time
environments.

The LLVM project has multiple components. The core of the project is
itself called "LLVM". This contains all of the tools, libraries, and header
files needed to process intermediate representations and convert them into
object files. Tools include an assembler, disassembler, bitcode analyzer, and
bitcode optimizer.

C-like languages use the [Clang](https://clang.llvm.org/) frontend. This
component compiles C, C++, Objective-C, and Objective-C++ code into LLVM bitcode
-- and from there into object files, using LLVM.

Other components include:
the [libc++ C++ standard library](https://libcxx.llvm.org),
the [LLD linker](https://lld.llvm.org), and more.

## Getting the Source Code and Building LLVM

Consult the
[Getting Started with LLVM](https://llvm.org/docs/GettingStarted.html#getting-the-source-code-and-building-llvm)
page for information on building and running LLVM.

For information on how to contribute to the LLVM project, please take a look at
the [Contributing to LLVM](https://llvm.org/docs/Contributing.html) guide.

## Getting in touch

Join the [LLVM Discourse forums](https://discourse.llvm.org/), [Discord
chat](https://discord.gg/xS7Z362),
[LLVM Office Hours](https://llvm.org/docs/GettingInvolved.html#office-hours) or
[Regular sync-ups](https://llvm.org/docs/GettingInvolved.html#online-sync-ups).

The LLVM project has adopted a [code of conduct](https://llvm.org/docs/CodeOfConduct.html) for
participants to all modes of communication within the project.
