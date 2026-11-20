# PTSan SPEC correctness CI

The manual **PTSan CI** workflow selects **software** (default), **lam**, or **id32**.
Each group runs glibc, musl non-strict and musl strict with **test and ref**
inputs, one copy and one iteration. There are 18 common rate checks including
both specrand checks; glibc also includes `507.cactuBSSN_r`, for 19 checks.
Parallel validation timings are not performance measurements.

## Configuration and installation

Correctness and performance share [ptsan.cfg](ptsan.cfg), the
[argument builder](../scripts/ptsan_ci_config.py), and the
[mode table](../scripts/ptsan_ci_modes.py). CMake supplies the documented
compiler/dependency stacks; the config owns optimization and compatibility flags.

- Dynamic glibc uses PTSan-compatible strip-only libc++; musl uses matching
  static instrumented libc, libc++, libc++abi and libunwind.
- Software decoding has 16-bit IDs; LAM has 15 usable ID bits and requires
  the kernel probe and dependency smoke checks.
- Abort-on-error, reoptimization, and stack temporal safety are enabled for
  software, LAM, and ID32, including instrumented dependencies.
  **Internal runtime validation is disabled** for SPEC; test/ref share one build.
- Generic x86-64 ISA, without `-march=native`. Pointer arithmetic/comparisons
  explicitly preserve IDs, matching the established SPEC recipes.
- Musl portability settings are defined in [ptsan.cfg](ptsan.cfg), with the
  optional Blender backtrace shim in [musl_compat/execinfo.h](musl_compat/execinfo.h).
  GCC disables global instrumentation only in non-strict configurations,
  for both libcs and encodings. Strict musl retains globals; the
  [ref expectations](../expectations/ptsan-spec-ref.json) record expected outcomes.
- `507.cactuBSSN_r` is glibc-only. Its support sources use uninstrumented
  `/usr/bin/gfortran -m64 -fallow-argument-mismatch`; C/C++ use the selected
  Clang configuration. The measured paths use C/C++. Other Fortran workloads
  are excluded; the manifests declare the Cactus configuration restriction.

The runner owns a dedicated licensed installation at `/home/ptsan-ci/spec2017`,
selected by `SPEC_ROOT`. It needs a working `shrc`, writable `config` directory,
GNU Fortran and `/usr/bin/make`. CI checks prerequisites before building.
Each workload uses an independent `--output_root` and config copy under
`$BUILD_DIR/spec/<mode>/<benchmark>` because SPEC writes executable hashes to
configs. Existing output roots are rejected. The next workflow run clears its
build directory, not the dedicated SPEC installation or other users' results.
Licensed sources, inputs, executables and complete outputs remain on the runner.

## Build and validation phases

```sh
python .github/scripts/ptsan-spec.py build --modes software
python .github/scripts/ptsan-spec.py validate glibc --modes software
# Repeat test-input validation for musl-nonstrict and musl-strict.
python .github/scripts/ptsan-spec.py validate --modes software --size ref
# For LAM, use --modes lam and the corresponding MODE-lam names.
# For ID32, use --modes id32 and the corresponding MODE-id32 names.
```

All 55 configuration/workload builds share one GNU Make jobserver using every
allowed CPU. Blender builds start first; other builds interleave across modes.
Python workers preserve the jobserver descriptors through `runcpu`, and SPEC
uses `/usr/bin/make` without its own `-j`. The bundled `specmake` does not support
this shared pool correctly. The slot budget limits build commands, not internal
compiler/linker threads.

All builds finish before validation. Test workloads run sequentially within
each configuration, in separate workflow steps. Ref workloads share one worker
pool across all configurations in the selected group, each with its own
`runcpu`, config, output root and log. Ref uses `--nobuild` and copies the built config with
its executable hashes. Saved plans/completion records reject incomplete builds,
changed expectations, mismatched mode groups and repeated validation. Test and
ref are each permitted once per build.

Ref scheduling uses the fixed longest-first preference in `REF_PRIORITY` in
`ptsan-spec.py`. All selected configurations of a benchmark are queued together,
with configuration names breaking ties. The list is not an allowlist: manifests
select workloads, and unlisted benchmarks run afterward in name order. This
avoids waiting for each configuration's slowest workload before starting the
next configuration. An optional positional mode still allows a single-mode ref
run. Performance measurements use their separate serial runner and are unchanged.

| Limit | Default |
|---|---|
| Each build worker, including waiting for slots | 1 hour |
| Shared build workflow step | 3 hours |
| Each test workload / configuration step | 15 minutes / 90 minutes |
| Each ref workload / pooled step | 4 hours / 18 hours |
| Overall workflow | 30 hours |

Ref concurrency defaults to all CPUs allowed by affinity; `--run-jobs` can lower
it. `--run-timeout` overrides the ref workload limit. There is no estimated
per-workload memory budget: OOM/crashes remain failures. The supervisor records
progress every 30 seconds and cleans up worker process groups on timeout or
cancellation before reusing slots.
The pooled workflow budget accommodates four waves of four-hour workloads at
the runner's 16-worker default, plus cleanup; normal runs should finish sooner.

Ordinary build failures allow other builds and successfully built workloads to
continue. A build timeout, interrupted worker or cancellation aborts the pool:
killing nested Make can lose jobserver tokens and otherwise hang the batch.
Validation refuses aborted builds. Individual validation failures/timeouts do
not stop other workloads or configurations.

## Result policy

[Test](../expectations/ptsan-spec.json) and
[ref](../expectations/ptsan-spec-ref.json) manifests specify input- and
configuration-specific outcomes and provenance. Differences require review,
not automatic acceptance or updates based solely on historical totals.

The checker requires successful builds, matching RSF benchmark `valid` records,
explicit specrand Success lines, and complete `specinvoke` child statuses and
diagnostics from declared stdout/stderr files. It selects RSF/run directories
by input size so test evidence cannot satisfy ref validation. Unknown diagnostic
categories, signals, missing/duplicate evidence, timeouts and unexpected passes
fail the step. Access complaints do not pin check numbers, function IDs or
addresses. `specinvoke` uses POSIX wait statuses: 256 means exit 1, not a signal.
Perl's driver can return zero despite failed subprocesses, so its expectations
also require the appropriate validation failure and diagnostics.

`ignore_errors = 1` lets other workloads continue; it does not establish success.
The `monitor_specrun_wrapper` appends `-k` to override SPEC's built-in `-q`, so a
failed input does not skip the remaining inputs of that benchmark. Every planned
invocation must complete; historical short-circuited `-q` runs are not accepted.
[Performance](../README-performance.md#validation-and-reports) has separate
policies for excluding already-known failures from timing comparisons.

## Results and artifacts

Correctness builds use
`/home/ptsan-ci/builds/PointerTagSanitizer-private/<group>`, where the group is
`id32`, `software`, or `lam`. They share the performance workflow's
repository concurrency group. Artifacts are `ptsan-ci-<group>-<run-id>-<attempt>` and are
uploaded even after failed gates.

- `ci-results/spec/`: shared `build-progress.json`, `build-phase.log`,
  `build-phase.json` and `build-summary.md` for all builds.
- `ci-results/spec/<mode>/<benchmark>/`: effective config, build/test harness
  logs, atomic build result, extracted child statuses and PTSan diagnostics.
  Mode summaries are `checked-results.json` and `summary.md`.
- `ci-results/spec-ref/<mode>/`: separate ref logs, expectations, extracted
  execution evidence and summaries, preserving test-input results.
- `ci-results/spec-ref/`: pooled `run-progress.json` and `run-phase.json`.
  Active, pending and completed jobs identify both configuration and benchmark.
- `$BUILD_DIR/spec/build-plan.json`: connects builds with validation and
  records the selected manifests/modes. Full benchmark files stay local.

Reports use manifest order even when workers finish out of order. Subprocesses
receive a minimal environment so SPEC's environment dump excludes GitHub credentials.
