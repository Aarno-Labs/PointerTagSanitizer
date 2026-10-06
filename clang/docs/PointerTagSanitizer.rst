===================
PointerTagSanitizer
===================

.. contents::
   :local:

Introduction
============

PointerTagSanitizer (PTSan) checks memory accesses using object identifiers
stored in pointers. Compile and link with ``-fsanitize=ptsan``. PTSan expects
mem2reg, so applications should be compiled with optimization (at least ``-O1``).

PTSan supports non-strict mode with host glibc, and strict or non-strict mode
with instrumented musl. Strict mode rejects untagged pointers, fallback
identifiers, and identifier exhaustion. Non-strict mode permits these cases
for compatibility. The examples below target Linux x86-64.

Build setup
===========

The builds below use Ninja and follow the
`LLVM 19 build requirements
<https://releases.llvm.org/19.1.0/docs/GettingStarted.html#software>`_.
The default build supports C and C++ with glibc. Supplying musl sources also
builds strict and non-strict musl support alongside glibc.

.. code-block:: bash

   LLVM_SRC=/absolute/path/to/PointerTagSanitizer
   LLVM_BUILD=/absolute/path/to/ptsan-build

The supplied CMake configuration builds an optimized compiler. Dependency
libraries are built and installed under ``$LLVM_BUILD/ptsan``.
The examples assume this build's ``clang`` and ``clang++`` are on ``PATH``.

The builds provide Clang configuration files alongside the compiler. These
enable PTSan and select the matching headers and libraries. Custom dependency
builds can use their own configuration files or explicit compiler flags.

Using PTSan with glibc
======================

This configuration uses the system glibc and supports non-strict mode.
PTSan wraps calls into glibc; calls without wrappers can cause segmentation
faults.

The following commands build Clang with PTSan support and the libraries needed
by C++ applications. The C++ libraries handle tagged pointers, but their own
memory accesses are not fully checked by PTSan.

.. code-block:: bash

   cmake -S "$LLVM_SRC/llvm" -B "$LLVM_BUILD" -G Ninja \
     -C "$LLVM_SRC/clang/cmake/caches/PTSan.cmake"
   ninja -C "$LLVM_BUILD"

For C-only builds, use ``ninja -C "$LLVM_BUILD" clang ptsan``.

Compile a C++ application using ptsan:

.. code-block:: bash

   clang++ --config=ptsan-glibc.cfg -O2 app.cpp -o app

The configuration also sets the runtime library search path to use the correct C++
stdlib when the application runs.

C applications can be compiled with
``clang -O2 -fsanitize=ptsan app.c -o app``.

Using PTSan with musl
=====================

Building musl with PTSan allows checks inside libc. This configuration supports
strict and non-strict mode and uses static linking.

Obtain musl 1.2.6, the version tested with PTSan:

.. code-block:: bash

   MUSL_SRC="$LLVM_BUILD/musl-src"
   git clone https://git.musl-libc.org/git/musl "$MUSL_SRC"
   git -C "$MUSL_SRC" checkout v1.2.6

The musl build requires GNU Make and Linux UAPI headers. musl's build scripts
require source and build paths without spaces.

The build uses Linux UAPI headers from ``/usr/include/x86_64-linux-gnu`` on
x86-64 or ``/usr/include/aarch64-linux-gnu`` on AArch64, plus ``/usr/include``
on both. For a different header layout, set ``PTSAN_LINUX_INCLUDE_DIRS`` to a
semicolon-separated list of include directories.

Build the compiler and the dependencies for glibc and both musl modes:

.. code-block:: bash

   cmake -S "$LLVM_SRC/llvm" -B "$LLVM_BUILD" -G Ninja \
     -C "$LLVM_SRC/clang/cmake/caches/PTSan.cmake" \
     -DPTSAN_MUSL_SOURCE_DIR="$MUSL_SRC"
   ninja -C "$LLVM_BUILD"

For C-only musl support, build ``ptsan-musl`` instead of the default target.

Choose strict or non-strict mode when compiling each application. Both
configurations use the same compiler:

.. code-block:: bash

   clang++ --config=ptsan-musl-strict.cfg -O2 app.cpp -o app

Or, for non-strict mode:

.. code-block:: bash

   clang++ --config=ptsan-musl-nonstrict.cfg -O2 app.cpp -o app

Each configuration selects its instrumented musl and C++ libraries, the musl
PTSan runtime, and static linking. The static libc++ library includes libc++abi
and libunwind.

For C applications, use ``clang`` with ``--config=ptsan-musl-strict-c.cfg`` or
``--config=ptsan-musl-nonstrict-c.cfg``.

Limitations
===========

PTSan checks allocation bounds, not individual fields within an allocation.
The identifier space is finite; strict mode fails on exhaustion.

Instrumented musl does not cover every kernel interface, including some
``ioctl`` requests. Bounds checks are disabled in selected low-level libc
helpers, in libunwind, and in selected libc++abi exception-handling and RTTI code.
Some TLS and late-loaded-module lifecycle cases, and within-frame stack
lifetime retirement, remain unsupported or incompletely checked.

Testing
=======

Run the host-libc regression suite with:

.. code-block:: bash

   ninja -C "$LLVM_BUILD" check-ptsan

Run the strict musl suite and the separate non-strict regression tests with:

.. code-block:: bash

   ninja -C "$LLVM_BUILD" check-ptsan-musl

``check-ptsan-musl`` (but not ``check-ptsan``) is excluded from ``check-all``.
