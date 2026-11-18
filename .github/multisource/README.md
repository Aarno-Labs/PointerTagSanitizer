# MultiSource correctness CI

After the compiler and musl/C++ dependencies are ready, CI configures clean
upstream `llvmorg-18.1.8` for glibc, non-strict musl, and strict musl. One build
step then compiles all three trees concurrently with a shared GNU Make jobserver
using every allowed CPU. The three configurations do not each get an independent
`-j` budget. SPEC has its own separate shared build phase.

CMake uses `Unix Makefiles` and `/usr/bin/make` for MultiSource. This replaces
Ninja so the installed tools can share slots across build trees. Compiler flags,
train inputs, the 167-case inventory, and expected failures are unchanged. The
native `fpcmp` and `timeit` tools also build within the shared slot budget.

Correctness and performance use the same [CMake cache](ptsan.cmake) and
[configuration helper](../scripts/ptsan_ci_config.py). The cache owns the common
test-suite options; the helper supplies paths and compiler configurations from
the shared [mode table](../scripts/ptsan_ci_modes.py). Correctness adds runtime
validation; performance omits it. Both explicitly enable strong stack safety
for instrumented applications. Baselines have no PTSan options. Execution
parallelism, timeouts, and repetitions remain separate runner policies.

The benchmark harness uses its own pinned `lit` 18.1.8 environment, matching
`llvmorg-18.1.8`. LLVM 23's bundled lit removed APIs used by this test suite.
Both correctness and performance workflows install the same
[requirements](requirements.txt); manual runs prepare the environment likewise.

The [CMake compatibility hook](compat.cmake) downgrades only
`-Wincompatible-pointer-types` in ClamAV's `libclamav_cvd.c` and `shared_cdiff.c`
and the two GSM `toast.c` files when compiling with Clang 23 or later. Other
translation units retain the normal diagnostics; upstream sources stay clean.

```sh
python -m venv "$BUILD_DIR/test-suite-venv"
"$BUILD_DIR/test-suite-venv/bin/python" -m pip install -r .github/multisource/requirements.txt
python .github/scripts/ptsan-multisource.py build --modes software
python .github/scripts/ptsan-multisource.py validate glibc --modes software
python .github/scripts/ptsan-multisource.py validate musl-nonstrict --modes software
python .github/scripts/ptsan-multisource.py validate musl-strict --modes software
```

All builds finish before the three validation steps start. Each validation step
runs lit with all allowed CPUs, so these are correctness results, not performance
measurements. Validation does not rebuild. A saved build plan and completion
record connect the phases; stale build roots, repeated validation, changed
expectations, and missing completion records are rejected.

The build checker accepts musl CLAMR's expected target-specific compilation
failure. It recognizes Make's leaf failures and their ancestor propagation,
without pinning compiler diagnostic text. Failures in other targets, unknown Make
errors, missing expected failures, and abnormal exit statuses fail the build step.
Normal compilation failures still let other targets finish and all three
validation steps run. Validation applies the existing diagnostic-aware checker,
including requiring NOEXE for the unbuilt CLAMR executable.

The checker requires the generated and executed tests to match the complete
[167-case inventory](../expectations/ptsan-multisource-tests.txt), with no duplicate
results. The [expectations](../expectations/ptsan-multisource.json) distinguish
ID exhaustion (the exact exhaustion diagnostic and child exit 1), PTSan access
complaints (child exit 1, without pinning check numbers or function IDs), and
other output mismatches (normal child exit 0, failed output comparison, and no
PTSan diagnostic). Missing evidence, a different failure category, unexpected
passes, and unrelated crashes or harness errors fail correctness validation.
[Performance result policies](../README-performance.md#validation-and-reports)
allow broader failures for already-excluded workloads and define timing exclusions
and ratio calculations.

A worker timeout or disappearance aborts the entire pool to avoid hanging on lost
jobserver tokens; validation refuses an aborted batch. Each configuration's
build worker has a one-hour limit, the shared build step has a 90-minute limit,
and each validation step has a 60-minute limit. The shared scheduler terminates
its process group on cancellation. Configure failures also prevent validation
from starting with partial artifacts.

Build trees remain in `$BUILD_DIR/multisource/<mode>` until the next CI run clears
`BUILD_DIR`. Logs and reports live in `ci-results/multisource/<mode>`, with shared
`build-progress.json`, `build-phase.json`, `build-phase.log`, and
`build-summary.md` directly under `ci-results/multisource`. Each mode retains its
raw build exit status, Make logs, effective CMake configuration, lit results,
execution records, and failure diagnostics.

## LAM configurations

Select **lam** in the **PTSan CI** workflow to run `glibc-lam`,
`musl-nonstrict-lam`, and `musl-strict-lam`. It uses `--modes lam` for both build
and validation, sharing CPU slots across those three trees only. Its build
root is `/home/ptsan-ci/builds/PointerTagSanitizer-private/lam`; software uses
`/home/ptsan-ci/builds/PointerTagSanitizer-private/software` and ID32 uses
`/home/ptsan-ci/builds/PointerTagSanitizer-private/id32`. All groups share the
performance workflow's repository concurrency group. A single runner worker
also prevents overlap with CI from other repositories.

The LAM job checks kernel support before building, then builds matched LAM
libc/C++ dependencies. LAM uses 15 usable ID bits; reviewed strict-LAM-only
exhaustion cases are recorded explicitly in the expectation manifest. Changes
in failure category and unexpected passes still require review.
