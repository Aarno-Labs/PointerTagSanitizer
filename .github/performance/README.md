# PTSan performance results

Start with [summary.md](summary.md) for the run configuration, repetition count,
headline overheads, per-benchmark tables, and exclusions. Check its completion
status before using the numbers. A downloadable artifact can exist even when
its job failed or was interrupted; partial results are labeled accordingly.

## Which artifact is this?

- `ptsan-performance-reports-<group>-<run-id>-<attempt>` is the compact results
  package. This README and the reports are inside its named top-level folder.
- `ptsan-performance-<group>-<run-id>-<attempt>` includes reports and diagnostics.
  Inside its named top-level folder, this README and the reports are in
  `performance/`; `python-packages.txt` is alongside that directory and records
  the CI Python environment.

Artifacts are retained on GitHub for 30 days. Download them for longer storage.
Both ZIPs contain one top-level folder named after the GitHub artifact, so plain
`unzip` keeps everything together:

```sh
unzip ptsan-performance-YOUR-DOWNLOAD.zip
```

Renaming the downloaded ZIP does not rename its internal folder. Older artifacts
without this wrapper can be extracted with `unzip file.zip -d file`.

## Tables and spreadsheets

| File | Contents |
|---|---|
| [summary.md](summary.md) | Human-readable headline and per-benchmark tables, completion status, and exclusions. |
| [primary-summary.csv](primary-summary.csv) | Headline overheads and included benchmark counts, with fixed comparison rows. |
| [summary.csv](summary.csv) | Every aggregate comparison: geometric mean ratio, overhead, included count, and primary-comparison marker. |
| [comparisons.csv](comparisons.csv) | Per-benchmark measured and baseline times, ratios, overheads, and exclusion reasons. Start here for detailed analysis. |
| [measurements.csv](measurements.csv) | Individual repetitions: timing, expected outcome category, whether the outcome matched, and diagnostics. |

CSVs use commas and UTF-8 with a BOM for Excel. Import them directly into Excel
or Google Sheets. Times are **seconds**. Overheads are **percentage points**:
`30.80` means **30.80% overhead**, not `0.3080`. Applying spreadsheet percent
formatting directly would multiply that displayed value by 100 again.
Undefined/excluded numeric values are blank, not zero. Fleetbench times are CPU
seconds per iteration, from one-second measurement loops, rather than whole-run times.

`primary-summary.csv` has one header row and these columns:

| Column | Meaning |
|---|---|
| A | Measured configuration |
| B | Baseline configuration |
| C | MultiSource overhead (%) |
| D | MultiSource included benchmark count |
| E | SPEC overhead (%) |
| F | SPEC included benchmark count |
| G | Fleetbench overhead (%) for software/ID32; otherwise overall report success |
| H | Fleetbench included case count (software/ID32 only) |
| I | Overall report success (software/ID32 only) |

For `software`, rows 2–4 are glibc/plain-glibc, musl-strict/plain-musl, and
musl-nonstrict/plain-musl. For `lam`, the same rows use the corresponding LAM
configurations; `id32` uses the corresponding ID32 configurations. For `glibc-stack-off`, row 2 is glibc/plain-glibc and row 3 is
glibc-lam/plain-glibc; both instrumented variants have stack temporal safety off.
The paper group has no musl rows. Consult `summary.md` for this run's actual group.

## How the numbers are calculated

Measurements use MultiSource train inputs, SPEC rate ref inputs, and the supported
17 Fleetbench cases (software and ID32 groups), running
serially on one pinned logical CPU. Internal PTSan runtime validation is off.
The plan and environment files record the selected configuration and CPU policy.

Per-benchmark ratios are measured median runtime divided by baseline median
runtime. Overhead is `100 * (ratio - 1)`. Aggregate ratios are geometric means
over each comparison's common eligible set. Counts can differ between comparisons.
With one repetition, the median is simply that one observation.

Ratios exclude failed runs, reported runtime errors, missing/incomplete timings,
and any MultiSource/SPEC comparison pair with a repetition below **10 ms**.
Fleetbench per-iteration times are not subject to that whole-run cutoff. SPEC specrand checks
are correctness-only and excluded from timing aggregates. Excluded benchmarks
remain in `comparisons.csv` with blank numeric cells and explicit reasons.
`matched` in `measurements.csv` means the outcome matched expectations; an
expected failure can match without qualifying for a performance ratio.

## Structured results and provenance

| File | Contents |
|---|---|
| `comparison.json` | Structured aggregate/per-benchmark comparisons, exclusions, policy, and report errors. |
| `measurements.json` | Combined raw measurements, repetitions, CPU, configuration list, and completion status. |
| `plan.json` | Compiler source revision, benchmark build jobs and commands, configurations, policy, and expectations. |
| `source-revisions.txt` | Resolved benchmark/dependency source revisions. |
| `*-environment-before.json`, `*-environment-after.json` | Selected CPU, allowed affinity, frequency policy, governors, load, and timestamps around each suite. |

## Additional files in the full diagnostics artifact

- `host.txt`: CPU topology, host information, affinity, memory, and resource limits.
- `spec-version.txt`: installed SPEC version.
- `CMakeCache.txt`, `configure.log`, `build-dependencies.log`: compiler/dependency
  build settings and logs.
- `compiler-configs/`: generated compiler configuration files.
- `dependencies/`, `build-baselines.log`: baseline smoke checks, compiler/link
  invocations, ELF information, and system glibc library selection.
- `lam-probe.log`: LAM capability probe, when the group requires LAM.
- `baseline-provenance.json`, `multisource-expectations.json`,
  `spec-expectations.json`: expected outcomes and their provenance.
- `build-phase.*`, `build-progress.json`, `build-summary.json`: pooled build
  command, log, completion state, progress, and outcome summary.
- `builds/<suite>/<configuration>/...`: per-build commands, logs, effective
  configuration, and build results. SPEC has an additional benchmark directory.
- `runs/<suite>/<configuration>/.../rep-N/`: run commands, logs, and checked
  results. SPEC adds a benchmark directory and has `execution.json` for child
  statuses/diagnostics. MultiSource has lit `results.json` and `failure-output/`.
- `measurements-multisource.json`, `measurements-spec.json`,
  `measurements-fleetbench.json` (software/ID32): separate suite
  measurement records before aggregation.
- `../fleetbench/`: pinned source/Bazel provenance, resolved lockfile, per-mode
  build commands, compiler configs, and build logs.

Some files are absent when a run fails before producing them. Licensed SPEC
sources, inputs, benchmark executables, and complete workload outputs stay on
the runner; they are not included in these artifacts.
