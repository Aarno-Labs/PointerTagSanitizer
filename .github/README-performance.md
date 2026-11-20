# PTSan performance CI

The **PTSan CI** correctness workflow accepts a benchmark-suite selector:
`all` (default), `multisource`, `spec`, or `fleetbench`. A focused run
still runs the full default compiler/dependency build, but skips
source setup, builds, and validation for the other benchmark suites. Fleetbench
is available for the `software` and `id32` groups, not `lam`.

Dispatch **PTSan performance** on the branch being measured. Select `software`
(default), `lam`, `id32`, or `glibc-stack-off` to run one group. Select `all` to
run all four groups sequentially, starting with `id32`; a failure in
one group does not prevent the remaining groups from running. Separately select
the suite: `all` (default), `multisource`, `spec`, or `fleetbench`.
The `software` group excludes LAM; the paper group explicitly includes both
encodings. Fleetbench runs only in the `software` and `id32` groups, so selecting
all groups with just Fleetbench runs those two groups.

| Group | Configurations |
|---|---|
| software | plain-glibc, plain-musl, glibc, musl-nonstrict, musl-strict |
| lam | plain-glibc, plain-musl, glibc-lam, musl-nonstrict-lam, musl-strict-lam |
| id32 | plain-glibc, plain-musl, glibc-id32, musl-nonstrict-id32, musl-strict-id32 |
| glibc-stack-off | plain-glibc, glibc, glibc-lam; paper-style stack/arithmetic policy |

All groups build fresh benchmark binaries. The **plain glibc baseline uses our
Clang with the default system headers and libraries**, including the system C++
standard library. It has no custom libc/C++ dependency build, sysroot, or rpath.
The instrumented glibc configurations still use their documented PTSan-compatible
LLVM C++ libraries. Thus their comparison against plain glibc also includes the
C++ library difference.

All musl executables use **static musl** and matching static libc++, libc++abi
and libunwind. `PTSAN_BUILD_BASELINE_DEPENDENCIES=ON` enables the shared CMake
`ptsan-musl-baseline-cxx` target and generated `ptsan-musl-baseline[-c].cfg`
configs. It builds uninstrumented libraries with the same layout as the
instrumented musl stacks. Plain musl explicitly uses `-O2`, matching instrumented
musl, rather than relying on configure's automatic optimization flags. CI invokes
that target and checks its output; it does not maintain a separate build recipe.
See the [PTSan build guide](../clang/docs/PointerTagSanitizer.rst)
for compiler and dependency setup.

## Configuration

- Correctness and performance share the [mode table](scripts/ptsan_ci_modes.py)
  and [configuration helper](scripts/ptsan_ci_config.py). MultiSource's common
  options live in its [CMake cache](multisource/ptsan.cmake); SPEC's optimization
  and compatibility options live in [ptsan.cfg](spec/ptsan.cfg). The runners
  supply paths, input sizes, and MultiSource's runtime-validation selection.
- The compiler and instrumented dependencies use `PTSan.cmake`, Release with
  assertions, as in correctness CI. Every group runs `ninja` without named targets,
  building all configured default targets. LAM groups additionally enable the LAM
  dependency stacks and require the existing hardware/kernel probe to pass.
- musl is pinned by tag `v1.2.6`; the clean LLVM test suite is `llvmorg-18.1.8`.
  Resolved revisions are recorded. No local miniFE modification is applied.
- Representation is selected explicitly: ID16 software, 15-bit LAM, or ID32.
  **Strong stack is enabled** in `software`, `lam`, and `id32`, in applications
  and dependencies in both correctness and performance CI. `glibc-stack-off`
  selects `-ptsan-stack-temporal-safety=0` for applications.
- Applications use abort-on-error, with **no `ptsan-runtime-validate`** at compile
  or link time. The normal runtime archive is selected. SPEC reoptimization and
  compatibility policies are shared with correctness CI, including GCC's
  non-strict-only global-instrumentation exemption. Baselines omit all PTSan
  options. MultiSource keeps its correctness CI optimization policy.
- Generic x86-64 ISA for MultiSource train and SPEC rate ref inputs, one copy.
  The two specrand workloads run as correctness checks but are not timed in the
  aggregate. SPEC includes mixed C/Fortran `507.cactuBSSN_r` in all glibc
  configurations only: 17 timed workloads plus two specrand checks for glibc,
  16 plus two for musl. Its Fortran support sources use uninstrumented GNU
  Fortran; C/C++ use the selected Clang configuration. Other Fortran workloads
  are excluded. No SPEC reportable-score claims.

## ID32

Both workflows have an `id32` dispatch group. Correctness runs the three ID32
libc configurations; performance adds the same uninstrumented baselines used by
ID16. Each application uses the matching generated config and C++ dependencies,
including the separate `ptsan-glibc-id32-cxx` strip-only shared-library build.
Its runtime metadata references resolve from the instrumented executable;
applications dynamically link both libc++ and glibc without extra link flags.

Stack temporal safety is enabled for all ID32 suites and runtime smoke tests.
Both workflows explicitly configure `PTSAN_ID32_STACK_TEMPORAL_SAFETY=1`, matching
the CMake default, so generated ID32 configs and instrumented musl/C++ dependencies
match the applications. ID32 uses cached per-frame origins and clears spans on
return while retaining its inline stack-ID fast path.

SPEC excludes Blender's unsupported pointer serialization, keeps Cactus glibc-only,
and uses the documented ID32 GCC pointer-bit and Omnet++ stack-accounting flags.
GCC globals remain instrumented in ID32. ID16 exhaustion expectations are not
inherited. ID32 expectations are provisional pending a full run with stack
temporal safety enabled; unexpected outcomes fail CI for review.

The `id32` group includes markerless strict musl through
`ptsan-musl-id32-strict.cfg`; it does not require a separate dispatch option.
MultiSource, SPEC, and all Fleetbench families except Cord and Protobuf use that
configuration. Cord and Protobuf use `ptsan-musl-id32-strict-indicator.cfg`, with
mixed decoding enabled only for Abseil's native frame walker. Both workflows
build the matching indicator dependencies. Correctness smoke tests check both
strict runtime configurations.

## Fleetbench

Both correctness and performance CI run the supported subset: proto, SwissMap,
libc memory routines, Snappy compression, hashing, Cord, RPC, and SIMD. These
eight executables contain 17 benchmark cases. TCMalloc and other compression
algorithms are not selected. Every expected case must run without a runtime
diagnostic or Google Benchmark error; a successful empty filter is a failure.

In strict ID16 (`musl-strict`), Proto, SwissMap, libc, Cord, and RPC are expected
to exhaust the ID pool. Both workflows require exit code 1 and only the exact
`PTSan: ID space exhausted in strict mode` diagnostic for these families. All
nine cases in those families are recorded as `out-of-ids` and excluded from
ratios, including any timings produced before exhaustion; the remaining eight
cases must pass. Other failures and unexpected passes fail CI for review.
These expectations do not apply to ID32, non-strict modes, or baselines.

Fleetbench is pinned to `86b091782b7600c3c806cb8bc70c51acc5fad64b` and Bazel to
8.0.0 (checksum checked). The runner needs autoconf, automake, and libtool installed.
All configurations use Fleetbench's `haswell` ISA profile, including baselines.
The Snappy tuple patch and three documented per-file exceptions match the
[Fleetbench harness](scripts/ptsan_fleetbench.py). Bazel host tools remain
native; target actions use the generated Clang configs, including for plain musl.
Stack temporal safety is enabled for both ID16 and ID32, matching the other CI
suites. Correctness enables runtime validation; performance does not.

Fleetbench performance is pinned to CPU 3 through `PERF_CPU`; correctness is
intentionally unpinned. On the current heterogeneous CI host, CPU-pinned CPUID
leaf 4 verified CPU 3's cache sizes as 49152, 3145728, and 25165824 bytes
respectively. The workflows use those values by default because the runner
hides cache topology in sysfs. Repository variables `FLEETBENCH_L1_DATA_SIZE`,
`FLEETBENCH_L2_SIZE`, and `FLEETBENCH_L3_SIZE` can override them if the runner
or selected performance CPU changes. Both workflows pass the values to every
configuration, including baselines, and record them in the run commands. These
values define fixed benchmark working sets; in correctness they do not imply
CPU affinity or performance claims. Do not substitute guessed sizes or summed
multi-socket cache totals. The defaults describe CPU 3's cache topology, not
every core on the machine.

Strict ID32 Cord and Protobuf are built in a separate
`musl-strict-id32-indicator` output root.
The build plan, compiler config, and result rows record this exception; other
strict ID32 cases remain markerless. Protobuf uses Cord internally, and Cordz's
occasional stack sampling reaches Abseil's native frame walker in both workloads.
The indicator setting is a pointer ABI choice for the whole application and its
instrumented dependencies, not just the unwinder. Only `stacktrace.cc` receives
`-ptsan-may-decode-raw` alongside `-ptsan-nochecks`. Short runs may not trigger
sampling, so retain the dedicated frame-walker regressions as well. This is not
specific to Fleetbench: other applications exercising that path need the same
compatible configuration.

Bazel builds run after the MultiSource/SPEC build pool finishes. The shared
builder overlaps at most four configurations and divides twice the available CPU
count among them, capped at the available CPU count per build. This gives four
builds with eight Bazel action slots each on the 16-CPU runner; each build also
uses an 18 GB Bazel scheduling budget. `--build-parallelism` and `--build-jobs`
override those defaults. Measurements start only after every build finishes and
run serially on the same pinned CPU as the other suites. Each case uses a
one-second Google Benchmark measurement loop, and reports CPU seconds **per iteration**.
The whole-program 10 ms cutoff does not apply to those per-iteration times.
Each of the 17 cases has equal weight in the Fleetbench geometric mean. Revision,
patched-source details, resolved Bazel lockfile, compiler configs and logs are
recorded under `ci-results/fleetbench/`.

## Paper-style glibc comparison

Select **group: glibc-stack-off** to measure plain glibc versus PTSan glibc
**with and without LAM**. Both instrumented variants disable stack temporal
safety and preserve IDs in pointer arithmetic/comparisons. SPEC already preserves
those IDs in all profiles; ordinary MultiSource inherits the compiler defaults.
This group builds both glibc-compatible C++ stacks and requires LAM_U48, but
builds no musl dependencies. Runtime validation stays off, abort-on-error stays
on, and current glibc failure expectations still apply.

This reproduces the paper's main configuration with current sources, not its
exact binaries or aggregate score. The July 11, 2026 campaign used PTSan
`dcc5e46b6e73da92800cb8a88a57168f749e6d15` and baseline compiler
`a4bf6cd7cfb1a1421ba92bca9d017b49936c55e4`; CI builds both from the selected
revision and rebuilds C++ dependencies. Other differences to account for:

- All Perl builds omit the erroneous historical `-DDEBUGGING`.
- Clean test-suite sources replace local modifications, including miniFE's annotation.
- CI rotates configuration order and interleaves SPEC modes per benchmark;
  the old campaign processed whole configurations and ran test inputs first.
- There is no separate untimed warm-up. Compare aggregates over the same
  eligible cases, using the exclusions below.

## Execution

Each run clears `/home/ptsan-ci/builds/PointerTagSanitizer-private/performance-<group>`; the resulting
builds remain until the next run of that group. Dependency builds use all allowed
cores. All MultiSource/SPEC configurations share one GNU Make jobserver so long
translation units overlap without independent per-tree CPU budgets. Expected
CLAMR build failures do not stop other targets. A build scheduler timeout or
cancellation aborts the pool and prevents measurement.

After builds, the workflow calls `/usr/local/sbin/ptsan-benchmark` and pins all
measurements to logical CPU **3**, set by `PERF_CPU` in the job. It checks allowed
affinity and records topology and CPU policy. CPU 3 is a performance core without
an SMT sibling on this runner. An `always()` step calls `/usr/local/sbin/ptsan-fast`
after success, failure, or runner-handled cancellation; a machine/runner crash
can require manual restoration. Governors and other users' affinity are unchanged.

Measurements are serial: MultiSource train, SPEC ref, then Fleetbench, with no builds
running. Lit uses `-j1`; SPEC uses one copy and `--nobuild`. Mode order rotates
between repetitions. MultiSource runs a whole suite per mode; SPEC interleaves
modes per benchmark. Each SPEC repetition uses a fresh output root and copies
existing executables/configs; setup is outside reported runtime. Each workload
has a four-hour limit, and failures do not prevent subsequent workloads or suites.

Full candidate validation uses **1** repetition; focused manual measurements
default to **3**. The job allows four days and shares `ptsan-private-runner`
concurrency with correctness CI within this repository. Both workflows use
`queue: max` with `cancel-in-progress: false`, allowing up to 100 pending workflow
runs to wait without replacing one another. GitHub cancels additional runs if
that queue fills. The shared runner must use one worker to prevent overlap with
jobs from other repositories.
Other users' workloads are not stopped; recorded load, affinity and resource
limits should be reviewed when interpreting results.

## Validation and reports

Every workload expected to pass must still satisfy output validation. For a
workload already expected to fail, performance accepts any runtime failure,
output mismatch, or workload timeout and excludes it from timing comparisons.
Correctness CI retains its diagnostic-aware expectations. Unexpected passes,
new failing workloads, missing evidence, build/configuration errors, and harness
failures still require review. Existing target-specific CLAMR build failures
remain accepted; a missing executable alone is not sufficient.

Baseline MultiSource expectations start from the October local campaigns: all
167 pass with glibc, and plain musl has five excluded runtime failures plus CLAMR's build failure. The first clean
CI run found that consumer-typeset crashes in plain musl: it unconditionally
closes the invalid descriptor returned by a failed `catopen`. Baseline SPEC ref
expects all 19 glibc checks and all 18 musl checks to pass. Cactus is marked
“Not selected for this configuration” in comparisons involving musl; it is
not counted as a failure or a missing measurement.

For the full matrices, the GitHub summary leads with three comparisons: PTSan glibc / plain glibc,
PTSan musl strict / plain musl, and PTSan musl non-strict / plain musl. The LAM
job uses the corresponding LAM configurations. Other comparisons remain
available below that table. `performance/summary.md`
contains expanded per-benchmark tables; `comparison.json` contains every ratio
and exclusion; `measurements.json` combines the separate
`measurements-multisource.json`, `measurements-spec.json`, and (for software/ID32)
`measurements-fleetbench.json` records. Each suite
has its own start marker and before/after environment records. A missing or
interrupted suite makes the combined report incomplete.

- MultiSource uses lit's `exec_time`, not lit/harness wall time.
- SPEC uses RSF `reported_time`, after checking `valid: S`, child statuses and
  diagnostics. Ignore-errors never substitutes for successful validation.
- Each runtime is the median across **all requested successful repetitions**.
- Ratios are `instrumented / baseline`; overhead is `100 * (ratio - 1)`.
- Each comparison is reported against **both plain glibc and plain musl**.
  Geometric means use the intersection passing in every repetition of that
  pair. Counts and exclusions are explicit; cohorts may differ between pairs.
- Exclude a MultiSource/SPEC benchmark from a comparison if either configuration has **any
  repetition below 10 ms**, before calculating medians or geometric means.
  Exactly 10 ms is eligible. Short runs remain correctness passes. A zero
  MultiSource user-CPU time is retained as a valid short measurement; do not
  replace it with an epsilon or selectively substitute wall/system time.
- Exclude reported runtime errors even if the program continues and exits zero.
  MultiSource scans preserved raw output for PTSan diagnostics, including on
  lit PASS results; SPEC checks diagnostics and child statuses as well as RSF
  validity. An unexpected error still fails the job. All exclusion reasons
  appear in the detailed report.
- Missing, negative, nonfinite, or malformed timings remain errors. Failed and
  timed-out workloads never count as speedups. Unexpected passes need review.
  Partial reports are labeled, and unexpected outcomes or an incomplete matrix
  fail the job.

Build/run logs, effective configs and derived evidence are uploaded. SPEC
sources, inputs, binaries and full application outputs remain in the local
build tree. The [candidate workflow](workflows/ptsan-candidate.yml) manually
runs the full correctness sequence before performance for the selected private
branch. Pushes do not trigger it.
See the [CI setup and execution policy](README-ptsan-ci.md).
Manual dispatch remains available once these workflows reach the default branch;
manual performance runs default to three repetitions.

## Downloadable reports and spreadsheets

The Actions summary prints headline numbers and links the compact
`ptsan-performance-reports-<group>-<run-id>-<attempt>` artifact. The full
`ptsan-performance-<group>-<run-id>-<attempt>` artifact also includes diagnostics.
Both ZIPs have a top-level folder matching their artifact name, and include the
[results README](performance/README.md), which documents all files, CSV units,
stable spreadsheet cells, and extraction. GitHub retains artifacts for 30 days.
Download them for longer storage; partial reports can exist after failed runs.
