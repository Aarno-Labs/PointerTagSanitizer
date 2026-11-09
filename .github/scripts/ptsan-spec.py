#!/usr/bin/env python3
"""Build the CPU2017 matrix, then validate test inputs per mode and pool ref runs."""
import argparse
from ptsan_ci_modes import MODES, CONFIGURATIONS, selected_modes
from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

from ptsan_ci_runs import run_slots, run_validation_batch
from ptsan_ci_config import spec_arguments, spec_benchmarks, stack_temporal_safety

from check_ptsan_spec import build_problem, check_result, inspect_case
from ptsan_ci_builds import (jobserver, minimal_environment, run_build_batch,
                               stop_group, write_json)


# Scheduling preference only: manifests still select the workloads. Start long
# benchmarks across all modes first so each configuration does not leave a tail.
REF_PRIORITY = (
    "520.omnetpp_r", "500.perlbench_r", "511.povray_r", "523.xalancbmk_r",
    "541.leela_r", "510.parest_r", "538.imagick_r", "502.gcc_r", "557.xz_r",
    "505.mcf_r", "531.deepsjeng_r", "544.nab_r", "525.x264_r", "508.namd_r",
    "507.cactuBSSN_r", "519.lbm_r", "997.specrand_fr", "999.specrand_ir",
)


def invoke(spec, arguments, destination, timeout, shared_build=False, shared_run=False):
    # runcpu startup uses this shared directory before selecting output_root.
    # Keep it nonempty so another runcpu cannot remove it between the directory
    # check and tempfile(). Leave the marker for subsequent CI invocations.
    temporary = spec / "tmp"
    temporary.mkdir(exist_ok=True)
    (temporary / ".ptsan-ci-keep").touch(exist_ok=True)
    env = minimal_environment()
    fds = ()
    if shared_build:
        env["MAKEFLAGS"], fds = jobserver()
    command = ["/bin/bash", "-c", 'cd "$1" && source ./shrc && shift && exec runcpu "$@"',
               "ptsan-spec", str(spec), *arguments]
    write_json(destination.with_suffix(".command.json"), command)
    print(f"{destination.stem}: {' '.join(arguments)}", flush=True)
    with destination.open("w") as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=not (shared_build or shared_run), pass_fds=fds)
        try:
            started = time.monotonic()
            while True:
                try:
                    return process.wait(timeout=min(30, timeout))
                except subprocess.TimeoutExpired:
                    elapsed = time.monotonic() - started
                    if elapsed >= timeout:
                        log.write(f"\nCI timeout after {timeout} seconds\n")
                        return 124
        finally:
            if not (shared_build or shared_run):
                stop_group(process)
            # Shared builds stay in the scheduler's process group. On timeout
            # the worker reports a fatal result and the coordinator kills the
            # entire batch: a forcibly killed Make can lose jobserver tokens.


def build_worker(path):
    job = json.loads(path.read_text())
    results = Path(job["results"])
    write_json(results / "build-started.json", {"pid": os.getpid()})
    row = {"build_exit": None, "problem": "Build worker interrupted", "fatal": True}
    try:
        status = invoke(Path(job["spec"]), job["arguments"], results / "build.log",
                        job["timeout"], shared_build=True)
        row = {"build_exit": status,
               "problem": build_problem(job["name"], status,
                                        (results / "build.log").read_text()),
               "fatal": status == 124 or status < 0}
    except (OSError, ValueError) as exc:
        row["problem"] = f"Build worker failed: {exc}"
    finally:
        try:
            build_logs = Path(job["output"]) / "benchspec" / "CPU" / job["name"] / "build"
            for directory in build_logs.glob(f"build_base_ci-{job['mode']}.*"):
                target = results / directory.name
                target.mkdir(exist_ok=True)
                for name in ("make.out", "make.err", "make.clean.out", "make.clean.err"):
                    if (directory / name).is_file():
                        shutil.copyfile(directory / name, target / name)
            shutil.copyfile(job["config"], results / "effective.cfg")
        except OSError as exc:
            row.update(problem=f"Could not preserve build evidence: {exc}", fatal=True)
        write_json(results / "build-result.json", row)
    return bool(row["problem"])


def report(results, mode, rows, inventory, final=False, size="test"):
    errors = [f"{row['name']}: {row['detail']}" for row in rows if not row["matched"]]
    if final and [row["name"] for row in rows] != inventory:
        errors.append("Missing, extra, or reordered benchmark results")
    counts = Counter(row["category"] for row in rows if row["matched"])
    write_json(results / "checked-results.json", {
        "mode": mode, "size": size, "complete": final, "success": final and not errors,
        "matched": dict(counts), "tests": rows, "errors": errors})
    encoding = ('32-bit IDs' if CONFIGURATIONS[mode].id32 else
                '15-bit LAM' if mode.endswith('-lam') else '16-bit software decoding')
    temporal = 'on' if stack_temporal_safety(mode) else 'off'
    lines = [f"### SPEC {size} inputs: {mode}", "",
             f"Correctness only; {encoding}, runtime validation disabled, stack temporal safety {temporal}. "
             "Historical expectations are provisional. No performance claims.", "",
             f"Expectation check: {'FAIL' if errors else 'PASS' if final else 'RUNNING'}", "",
             "| Benchmark | Expected | Matched |", "|---|---|---|"]
    lines += [f"| {r['name']} | {r['category']} | {'yes' if r['matched'] else 'NO'} |" for r in rows]
    if errors:
        lines += ["", *[f"- {error}" for error in errors]]
    summary = "\n".join(lines) + "\n"
    (results / "summary.md").write_text(summary)
    if final:
        print(summary, flush=True)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as stream:
                stream.write(summary)
    return bool(errors)


def prepare_jobs(build, workspace, spec, manifest, mode):
    results = workspace / "ci-results" / "spec" / mode
    results.mkdir(parents=True, exist_ok=True)
    inventory = spec_benchmarks(manifest, mode)
    manifest_path = workspace / ".github/expectations/ptsan-spec.json"
    shutil.copyfile(manifest_path, results / "expectations.json")
    shutil.copyfile(spec / "version.txt", results / "spec-version.txt")
    root = build / "spec" / mode
    # Refuse old evidence. The workflow clears BUILD_DIR at the start of a job.
    root.mkdir(parents=True, exist_ok=False)
    template = workspace / ".github/spec/ptsan.cfg"
    shutil.copyfile(template, results / "config-template.cfg")
    jobs = []
    for benchmark in inventory:
        case_results = results / benchmark
        case_results.mkdir()
        output = root / benchmark
        output.mkdir()
        config = output / "ptsan.cfg"
        shutil.copyfile(template, config)
        commands = spec_arguments(build, workspace, mode, config, output, size="test")
        jobs.append({"name": benchmark, "mode": mode, "spec": str(spec),
                     "results": str(case_results), "output": str(output),
                     "config": str(config), "timeout": 3600,
                     "arguments": [*commands, "--action=build", "--rebuild", benchmark]})
    return jobs


def validate_mode(spec, results, mode, jobs, manifest, aborted):
    inventory = spec_benchmarks(manifest, mode)
    rules = manifest["configurations"][mode]
    rows = []
    # All builds have finished (or the batch has been terminated) before any run.
    for job in jobs:
        benchmark = job["name"]
        case_results = Path(job["results"])
        output = Path(job["output"])
        commands = job["arguments"][:-3]
        rule = rules.get(benchmark)
        row = {"name": benchmark, "category": rule["category"] if rule else "pass",
               "matched": False}
        try:
            if aborted:
                raise ValueError(f"Build phase aborted: {aborted}")
            build_result = json.loads((case_results / "build-result.json").read_text())
            row["build_exit"] = build_result["build_exit"]
            problem = build_result["problem"]
            if problem is None:
                status = invoke(spec, [*commands, "--action=run", "--nobuild", benchmark],
                                case_results / "run.log", 900)
                row["run_exit"] = status
                if status != 0:
                    raise ValueError(f"Run harness {'timed out' if status == 124 else 'failed'} (exit {status})")
                log = (case_results / "run.log").read_text(errors="replace")
                evidence = inspect_case(output, benchmark, mode)
                write_json(case_results / "execution.json", evidence)
                problem = check_result(benchmark, rule, evidence, status, log)
            row.update(matched=problem is None, detail=problem)
        except (OSError, ValueError) as exc:
            row["detail"] = f"Could not validate: {exc}"
        rows.append(row)
        report(results, mode, rows, inventory)
        print(f"{benchmark}: {'matched' if row['matched'] else row['detail']}", flush=True)
    return report(results, mode, rows, inventory, final=True)


def run_worker(path):
    job = json.loads(path.read_text())
    results = Path(job["results"])
    rule = job["rule"]
    row = {"mode": job["mode"], "name": job["name"],
           "category": rule["category"] if rule else "pass", "matched": False}
    try:
        build_result = json.loads(Path(job["build_result"]).read_text())
        row["build_exit"] = build_result["build_exit"]
        problem = build_result["problem"]
        if problem is None:
            status = invoke(Path(job["spec"]), job["arguments"], results / "run.log",
                            job["timeout"], shared_run=True)
            row["run_exit"] = status
            if status != 0:
                raise ValueError(f"Run harness {'timed out' if status == 124 else 'failed'} (exit {status})")
            evidence = inspect_case(Path(job["output"]), job["name"], job["mode"], job["size"])
            write_json(results / "execution.json", evidence)
            problem = check_result(job["name"], rule, evidence, status,
                                   (results / "run.log").read_text(errors="replace"))
        row.update(matched=problem is None, detail=problem)
    except (OSError, ValueError) as exc:
        row["detail"] = f"Could not validate: {exc}"
    write_json(results / "run-result.json", row)
    # An ordinary mismatch is evidence, not a broken worker.
    return 0


def validate_ref(results, modes, jobs, manifest, aborted, slots, timeout):
    inventories = {mode: spec_benchmarks(manifest, mode) for mode in modes}
    results.mkdir(parents=True, exist_ok=True)
    for mode in modes:
        directory = results / mode
        directory.mkdir(exist_ok=False)
        write_json(directory / "expectations.json", manifest)
        report(directory, mode, [], inventories[mode], size="ref")
    rows = {}

    def completed(row):
        mode, name = row["mode"], row["name"]
        if mode not in inventories or name not in inventories[mode] or (mode, name) in rows:
            raise ValueError(f"Unexpected or duplicate ref result: {mode}/{name}")
        rows[mode, name] = row
        ordered = [rows[mode, name] for name in inventories[mode] if (mode, name) in rows]
        report(results / mode, mode, ordered, inventories[mode], size="ref")
        print(f"{mode}/{row['name']}: {'matched' if row['matched'] else row['detail']}", flush=True)

    queue = []
    finished = False
    try:
        for job in jobs:
            mode, name = job["mode"], job["name"]
            rule = manifest["configurations"][mode].get(name)
            if aborted:
                completed({"mode": mode, "name": name,
                           "category": rule["category"] if rule else "pass",
                           "matched": False, "detail": f"Build phase aborted: {aborted}"})
                continue
            case = results / mode / name
            case.mkdir()
            # SPEC writes hashes to configs. Give ref its own copy, while reusing
            # the output root/executable; size distinguishes run directories.
            config = Path(job["output"]) / "ptsan-ref.cfg"
            shutil.copyfile(job["config"], config)
            shutil.copyfile(config, case / "effective.cfg")
            commands = ["--size=ref" if a == "--size=test" else
                        f"--config={config}" if a.startswith("--config=") else a
                        for a in job["arguments"][:-3]]
            queue.append({**job, "results": str(case), "size": "ref", "rule": rule,
                          "build_result": str(Path(job["results"]) / "build-result.json"),
                          "timeout": timeout,
                          "arguments": [*commands, "--action=run", "--nobuild", name]})
        priority = {name: index for index, name in enumerate(REF_PRIORITY)}
        queue.sort(key=lambda job: (priority.get(job["name"], len(priority)),
                                    job["name"], job["mode"]))
        run_validation_batch(queue, slots, results, completed)
        finished = True
    finally:
        failed = False
        for mode, inventory in inventories.items():
            ordered = [rows[mode, name] for name in inventory if (mode, name) in rows]
            failed |= report(results / mode, mode, ordered, inventory, final=finished, size="ref")
    return failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", choices=["build", "validate"])
    parser.add_argument("mode", nargs="?", choices=MODES)
    parser.add_argument("--build-worker", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--run-worker", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--modes", choices=["software", "lam", "id32", "all"], default="all")
    parser.add_argument("--size", choices=["test", "ref"], default="test")
    parser.add_argument("--run-jobs", type=int, help="ref concurrency ceiling; defaults to all allowed CPUs")
    parser.add_argument("--run-timeout", type=int, default=14400, help="seconds per ref workload")
    args = parser.parse_args()
    if args.run_worker:
        return run_worker(args.run_worker)
    modes = selected_modes(args.modes)
    if args.mode and args.mode not in modes:
        parser.error("validation mode is outside the selected build group")
    if args.size == "ref" and args.action != "validate":
        parser.error("use --size ref with validate; ref reuses the existing build")
    if args.run_timeout < 1 or (args.run_jobs is not None and args.run_jobs < 1):
        parser.error("run limits must be positive")
    if args.build_worker:
        return build_worker(args.build_worker)
    if (args.action is None or (args.action == "build" and args.mode is not None)
            or (args.action == "validate" and args.size == "test" and args.mode is None)):
        parser.error("use 'build', 'validate MODE', or 'validate --size ref'")
    build = Path(os.environ["BUILD_DIR"]).resolve()
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    spec = Path(os.environ["SPEC_ROOT"]).resolve()
    for path in (build, workspace, spec):
        if any(c.isspace() for c in str(path)):
            raise ValueError("SPEC CI requires absolute paths without whitespace")
    if not (spec / "shrc").is_file() or not os.access(spec / "bin/runcpu", os.X_OK):
        raise ValueError(f"SPEC installation is not readable/executable: {spec}")
    manifest = json.loads((workspace / ".github/expectations/ptsan-spec.json").read_text())
    ref_manifest = json.loads((workspace / ".github/expectations/ptsan-spec-ref.json").read_text())
    if (ref_manifest["schema"] != 1 or ref_manifest["benchmarks"] != manifest["benchmarks"]
            or ref_manifest.get("benchmark_configurations", {}) != manifest.get("benchmark_configurations", {})
            or set(ref_manifest["configurations"]) != set(MODES)
            or any(not set(rules) <= set(ref_manifest["benchmarks"])
                   for rules in ref_manifest["configurations"].values())):
        raise ValueError("Invalid SPEC ref expectations/inventory")
    inventory = manifest["benchmarks"]
    if (manifest["schema"] != 1 or len(inventory) != 19 or len(set(inventory)) != 19
            or any(not re.fullmatch(r"\d{3}\.[a-zA-Z0-9_]+", name) for name in inventory)
            or set(manifest["configurations"]) != set(MODES)
            or any(not set(rules) <= set(inventory)
                   for rules in manifest["configurations"].values())):
        raise ValueError("Invalid SPEC expectations/inventory")
    root = build / "spec"
    results = workspace / "ci-results/spec"
    plan_path = root / "build-plan.json"
    if args.action == "build":
        # Refuse previous builds; validation must consume this exact fresh batch.
        root.mkdir(parents=True, exist_ok=False)
        by_mode = {mode: prepare_jobs(build, workspace, spec, manifest, mode) for mode in modes}
        # Start the known long Blender compilations early. Interleave modes so
        # their long translation units can overlap instead of forming separate tails.
        jobs = [job for mode in modes for job in by_mode[mode]]
        jobs.sort(key=lambda job: (job["name"] != "526.blender_r",
                                  inventory.index(job["name"]), modes.index(job["mode"])))
        write_json(plan_path, {"schema": 1, "manifest": manifest, "ref_manifest": ref_manifest, "modes": list(modes), "jobs": jobs})
        aborted = run_build_batch(root, results, jobs, len(os.sched_getaffinity(0)))
        failed = []
        for job in jobs:
            path = Path(job["results"]) / "build-result.json"
            if not path.is_file() or json.loads(path.read_text())["problem"] is not None:
                failed.append(f"{job['mode']}/{job['name']}")
        summary = (f"### SPEC builds\n\n{len(jobs) - len(failed)}/{len(jobs)} built successfully.\n"
                   + (f"\nBuild batch aborted: {aborted}\n" if aborted else "")
                   + ("\nFailed or incomplete:\n" + "".join(f"- {name}\n" for name in failed)
                      if failed else ""))
        (results / "build-summary.md").write_text(summary)
        print(summary, flush=True)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as stream:
                stream.write(summary)
        return bool(aborted or failed)

    plan = json.loads(plan_path.read_text())
    if (plan["schema"] != 1 or plan["manifest"] != manifest
            or plan.get("ref_manifest") != ref_manifest or plan.get("modes") != list(modes)):
        raise ValueError("SPEC build plan does not match current expectations")
    expected = {(mode, name) for mode in modes for name in spec_benchmarks(manifest, mode)}
    actual = [(job["mode"], job["name"]) for job in plan["jobs"]]
    if len(actual) != len(expected) or set(actual) != expected:
        raise ValueError("Missing or duplicate SPEC builds in build plan")
    # No validation starts without a completed scheduler record. Missing records
    # (e.g. a hard-killed coordinator) fail closed, even if some workers finished.
    phase = json.loads((results / "build-phase.json").read_text())
    aborted = phase["aborted"]
    if phase["exit"] is None and not aborted:
        raise ValueError("SPEC build scheduler has not finished")
    if args.size == "ref":
        ref_modes = (args.mode,) if args.mode else modes
        slots = run_slots(args.run_jobs) if not aborted else 1
        for mode in ref_modes:
            with (root / mode / "ref-validation-started.json").open("x") as stream:
                json.dump({"pid": os.getpid(), "slots": slots}, stream)
        return validate_ref(workspace / "ci-results/spec-ref", ref_modes,
                            [job for job in plan["jobs"] if job["mode"] in ref_modes], ref_manifest,
                            aborted, slots, args.run_timeout)
    inventory = spec_benchmarks(manifest, args.mode)
    jobs_by_name = {job["name"]: job for job in plan["jobs"] if job["mode"] == args.mode}
    with (root / args.mode / "validation-started.json").open("x") as stream:
        json.dump({"pid": os.getpid()}, stream)
    return validate_mode(spec, results / args.mode, args.mode,
                         [jobs_by_name[name] for name in inventory], manifest, aborted)


if __name__ == "__main__":
    # Run cleanup in invoke's finally block on workflow cancellation too.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    sys.exit(main())
