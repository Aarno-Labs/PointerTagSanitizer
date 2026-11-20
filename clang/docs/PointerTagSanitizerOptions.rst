PointerTagSanitizer option reference
====================================

For a first build, see :doc:`PointerTagSanitizer`. Its example
``--config`` files select compatible compiler flags, headers, libraries, and
link settings. The defaults below apply when invoking the compiler directly.

.. contents::
   :local:

Clang driver options
--------------------

Pass the same mode options when compiling application objects and when linking
with the Clang driver. The driver uses them to select the matching runtime
archive. For the complete driver syntax, see :doc:`ClangCommandLineReference`.

``-fsanitize=ptsan``
   Instrument the program. Use at least ``-O1``. The default is non-strict
   checking with 16-bit IDs.
``-fsanitize-ptsan-32-bit-ids``, ``-fno-sanitize-ptsan-32-bit-ids``
   Select the 32-bit or 16-bit ID encoding. Use libraries built for the same
   encoding.
``-fsanitize-ptsan-strict-mode``, ``-fno-sanitize-ptsan-strict-mode``
   Select strict or non-strict checking. Strict mode requires instrumented
   libc, rejects untagged and fallback IDs, and fails on ID exhaustion.
``-fsanitize-ptsan-lam``, ``-fno-sanitize-ptsan-lam``
   Select the x86-64 Linear Address Masking encoding. It requires hardware and
   kernel support for LAM U48 and cannot be combined with 32-bit IDs.
``-fsanitize-ptsan-runtime-validate``, ``-fno-sanitize-ptsan-runtime-validate``
   Enable or disable internal consistency checks. Validation defaults to off
   and requires the matching validation runtime archive.

LLVM pass options
-----------------

Pass each option through Clang with its own ``-mllvm``, for example
``-mllvm -ptsan-strip-ptrtoint-function=hash_address``. With ``opt``, pass the
option directly alongside ``-passes=ptsan``. Run ``opt --help-hidden`` from
the same build to list its registered options.

The driver forwards its four mode selections as ``-ptsan-32-bit-ids``,
``-ptsan-strict-mode``, ``-ptsan-lam``, and ``-ptsan-runtime-validate``.
Use the driver spellings when selecting a runtime. PTSan emits runtime policy
as constants in the compiled program. Use consistent settings across
application objects for program-wide choices such as ID allocation and error
reporting.

Pointer encoding and compatibility
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``-ptsan-lam``
   Enable x86-64 LAM U48 for ID16. Compile with this option and link with the
   matching LAM runtime.
``-ptsan-support-indicator-bit``, ``-ptsan-may-decode-raw``
   In strict ID32, the first option reserves the high ID bit for the
   application. The second requires the first and emits mixed raw/tagged
   decoding in the translation unit being compiled. Apply it to files that
   need those conversions and use matching instrumented dependencies.
``-ptsan-32-bit-ids-origin-quantum=N``
   Preserve address bits within an N-byte ID32 origin quantum. The default is
   4096. N must be a power of two up to 2 GiB.
``-ptsan-clear-ids-on-arith``, ``-ptsan-clear-ids-on-cmp``
   Strip IDs for pointer subtraction and comparisons, respectively. Both
   default to on. Use ``=0`` to disable either.
``-ptsan-strip-ptrtoint``
   Convert pointers to raw addresses for ``ptrtoint`` and ``ptrtoaddr``
   conversions. Integer storage then loses the pointer ID. Defaults to off.
``-ptsan-strip-ptrtoint-function=name``, ``-ptsan-clear-ids-on-arith-function=name``, ``-ptsan-clear-ids-on-cmp-function=name``
   Enable the corresponding behavior in a named IR function. Each option is
   repeatable. Names are matched during PTSan discovery, after earlier
   inlining.
``-ptsan-strip-pointer-bit-twiddles=N``
   Strip IDs for address-bit operations such as shifts and constant masks.
   Masks that touch only the lowest N bits preserve the ID. N ranges from 0
   to 64. The default, -1, disables this handling.
``-ptsan-strip-ids``, ``-ptsan-strip-ids-on-cmp``
   The first schedules the strip-only pass in an optimized pipeline, for
   example when building a library that must accept tagged pointers. It strips
   memory-access operands without adding bounds checks. The second extends
   that pass to pointer comparisons. Both default to off.

Bounds, allocation, and lifetime
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``-ptsan-instrument-globals``, ``-ptsan-global-section=name``
   Global instrumentation defaults to on. To register a named ELF section as
   one object, specify its name in its owning translation unit. This requires
   global instrumentation and gives one ID whose bounds cover the whole
   section. Repeat the section option for additional sections.
``-ptsan-stack-temporal-safety``
   Enable stronger lifetime checks for stack objects. Defaults to on.
``-ptsan-use-returned-malloc-size``
   Use the allocator's usable size as the heap allocation bound. By default,
   PTSan checks against the requested size.
``-ptsan-id-bucket-size=N``, ``-ptsan-stack-id-thread-buckets=N``
   Set the number of IDs per bucket and the number of threads that can reserve
   a stack bucket. ID16 defaults to 512 IDs and 12 threads. ID32 defaults to
   65536 IDs and 128 threads. Bucket size must be a power of two of at least
   64, and all reservations must fit in the available ID space. Zero thread
   buckets disables reservations.
``-ptsan-stack-id-log-capacity=N``
   Limit outstanding stack-ID log entries per thread. The default is the
   smaller of the encoding's ID space and 2^20 entries.
``-ptsan-id-reuse-distance=N``
   Set the target pool size for choosing recycled IDs. The default is up to
   65536 candidates, limited by the ID space. Capacity pressure can shorten
   the reuse distance.
``-ptsan-randomize-ids``, ``-ptsan-random-seed=N``
   ID allocation is randomized by default. Use a nonzero fixed seed for
   repeatable tests or ``-ptsan-randomize-ids=0`` for sequential allocation.

Reporting and pass development
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``-ptsan-abort-on-error``, ``-ptsan-quiet-errors``
   Abort on a failed bounds check (default on) or suppress its diagnostic
   (default off). ID32 always enables abort-on-error.
``-ptsan-runtime-validate-arith``, ``-ptsan-print-stats``
   With runtime validation enabled, check pointer tags in arithmetic and
   comparisons, or print check statistics at program exit. Both default to
   off.
``-ptsan-stop-after=phase``, ``-ptsan-create-ir-dbg``
   Inspect intermediate IR by stopping after ``prepare``, ``discovery``,
   ``optimization``, ``metadata-lowering``, ``late-optimization``, or
   ``lowering``. The debug option adds source locations to generated IR and
   requires ``-g``.
``-ptsan-disable``, ``-ptsan-nochecks``
   Skip instrumentation, or propagate pointer metadata while omitting
   generated bounds checks. Both are off by default.
``-ptsan-patch-all-with=N``
   Select the experimental action in the runtime patch handler. Zero chooses
   the default handling, 1 terminates, and 2 jumps to the saved patch target.

Optimization experiments
^^^^^^^^^^^^^^^^^^^^^^^^

``-ptsan-reoptimize``, ``-ptsan-preanalyze-loops``
   Run another LLVM optimization pipeline after instrumentation, or analyze
   loop accesses in advance. Both default to on.
``-ptsan-no-load-hoist``, ``-ptsan-no-loop-check-hoist``
   Disable hoisting of bounds loads or loop checks. Disabling load hoisting
   also disables loop-check hoisting.
``-ptsan-no-check-merge``, ``-ptsan-no-filter-removable-checks``
   Keep compatible checks separate, or retain checks that static analysis
   proves unnecessary.
``-ptsan-no-stack-id-fast-path``, ``-ptsan-no-opts``
   The first disables fast stack-ID allocation. The second also disables PTSan
   check optimizations. Pointer conversions still run.
``-ptsan-add-overflow-checks``
   Use unsigned addition with an overflow check to compute the end of an
   access (default on). With ``=0``, the check uses the remaining span.
``-ptsan-x86-inline-strip``
   Use an inline AND for eligible scalar ID strips on x86-64 (default on).
``-ptsan-loose-loop-optimizations``
   Experimental. Relax loop legality checks to use postdominance alone. This
   can miss invalid accesses. Defaults to off.

CMake settings
--------------

Pass these settings as ``-Dname=value`` when configuring the LLVM build.

``LLVM_ENABLE_PTSAN_DEPENDENCIES``, ``PTSAN_DEPENDENCIES_DIR``
   Enable the libc and C++ dependency targets and select where they build.
   The PTSan CMake cache enables them. The default directory is ``ptsan``
   inside the LLVM build directory.
``PTSAN_MUSL_SOURCE_DIR``
   Supply a local musl source tree to build strict and non-strict instrumented
   musl dependencies. Without it, the dependency target builds only the C++
   libraries used with the host glibc runtime.
``PTSAN_LINUX_INCLUDE_DIRS``, ``PTSAN_MUSL_BUILD_JOBS``
   Override the detected Linux UAPI include directories or the parallel job
   count for each musl Make invocation.
``PTSAN_ID32_STACK_TEMPORAL_SAFETY``
   Set stack temporal safety for ID32 dependencies and generated
   configurations. The default is 1. Set it to 0 to disable the stronger stack
   lifetime checks.
``PTSAN_BUILD_BASELINE_DEPENDENCIES``, ``PTSAN_BUILD_LAM_DEPENDENCIES``
   Add uninstrumented musl/C++ dependencies for baseline comparisons, or add
   x86-64 LAM dependency stacks. Both default to off.

The default ``ptsan-dependencies`` target builds the configured dependency
families and their C++ libraries. ``ptsan-musl`` and ``ptsan-musl-cxx`` build
the configured musl families without or with their C++ libraries. The
``install-ptsan-dependencies`` target installs generated Clang configurations
beside Clang and dependencies under ``lib/ptsan``.

Runtime validation
------------------

Use ``-fsanitize-ptsan-runtime-validate`` at both compile and link time. The
ordinary ``ptsan`` runtime target builds validation archives for each supported
encoding. The generated application configurations select non-validation
runtimes by default, so pass the validation driver option after ``--config``
when invoking Clang.
