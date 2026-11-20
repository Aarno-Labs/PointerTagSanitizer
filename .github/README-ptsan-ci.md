# Private PTSan candidate validation

Pushes to any branch containing the
[candidate workflow](workflows/ptsan-candidate.yml) start validation in
`Aarno-Labs/PointerTagSanitizer-private`. Tag-only pushes do not trigger it.
The workflow is read-only with respect
to repository contents. Every PTSan job checks the exact repository identity,
so a public release or a fork cannot execute these jobs.

The automatic sequence is:

1. Correctness: ID32, software, then LAM, with every supported suite.
2. If all correctness groups pass, performance: ID32, software, LAM, then
   glibc-stack-off, with every supported suite and one repetition.

Later correctness groups run after an ordinary earlier failure. Performance
requires success from all three correctness groups. A performance failure does
not suppress later performance groups. Failures remain failures in the overall
workflow. LAM and glibc-stack-off omit Fleetbench, which currently supports only
software and ID32.

Explicit job dependencies enforce this order. Each invocation checks out the
same triggering commit. A new push supersedes the previous automatic run on the
same branch. The shared runner concurrency group uses `queue: max`, so up to
100 invocations from different branches or manual runs can wait without replacing
one another; GitHub cancels additional invocations if that queue fills.
Cancellation stops further work, retains available reports, and
runs the existing performance CPU-policy cleanup. Runner or machine failure can
still require manual CPU-policy restoration.

One repetition measures coverage and initial performance; it does not establish
run-to-run variability. The reusable [correctness](workflows/ptsan-ci.yml) and
[performance](workflows/ptsan-performance.yml) workflows also retain manual
entrypoints for focused reruns. Manual performance defaults to three repetitions
for release measurements. GitHub exposes those entrypoints after the workflow
files reach the default branch; automatic push runs need no default-branch setup.

## Runner setup

Use the existing organization runner group `ptsan-ci`, labels
`self-hosted`, `linux`, `x64`, and runner account `ptsan-ci`. Grant the group access
only to drift and the private repository; exclude the public repository and
leave public-repository access disabled. The group must use a single worker on
the benchmark host, so builds and measurements from the two repositories do not
overlap. GitHub concurrency groups are repository-scoped, not host-wide locks.

Correctness builds use
`/home/ptsan-ci/builds/PointerTagSanitizer-private/<group>`; performance uses
`/home/ptsan-ci/builds/PointerTagSanitizer-private/performance-<group>`. Each job
validates its group before clearing only that directory. The runner account
needs write access to this parent. Actions checkout workspaces are separate for
each repository.

The existing runner prerequisites remain:

- A clean musl `v1.2.6` checkout at `/home/ptsan-ci/sources/musl`.
- The licensed SPEC installation at `/home/ptsan-ci/spec2017`, including its
  writable config/tmp directories, plus GNU Make and GNU Fortran.
- Clang, LLD, CMake, Ninja, Python with venv support, and dependency-fetch access.
- Working LAM_U48 support and the existing `ptsan-fast` / `ptsan-benchmark` sudo
  helpers. Measurements use logical CPU 3.
- Verified Fleetbench cache sizes. The existing defaults can be overridden with
  repository variables `FLEETBENCH_L1_DATA_SIZE`, `FLEETBENCH_L2_SIZE`, and
  `FLEETBENCH_L3_SIZE`.

The private-only [runner smoke workflow](workflows/ptsan-runner-smoke.yml) can
check the basic account/tools once available for manual dispatch. Full runs
also check their own prerequisites. No runner registration change or additional
worker is needed when the existing group already serves both repositories.

## Results

Correctness artifacts are `ptsan-ci-<group>-<run-id>-<attempt>`. Performance
artifacts include compact reports and full diagnostics per group, retained for
30 days. See [performance reporting](performance/README.md) and the
[execution details](README-performance.md). Partial results retain their failure
and completion status. SPEC sources, inputs, binaries and complete workload
outputs remain on the runner.
