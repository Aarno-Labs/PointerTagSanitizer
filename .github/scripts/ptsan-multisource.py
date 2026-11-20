#!/usr/bin/env python3
"""Build all MultiSource configurations together, then validate each separately."""
import argparse
from ptsan_ci_modes import MODES, selected_modes
import importlib.util
import json
import os
from pathlib import Path
import signal
import shutil
import tarfile
import time
import subprocess
import sys

from ptsan_ci_config import multisource_command
from ptsan_ci_builds import jobserver, minimal_environment, run_build_batch, write_json

SCRIPTS = Path(__file__).resolve().parent

loader = importlib.util.spec_from_file_location("multisource_checker", SCRIPTS / "check-ptsan-multisource.py")
checker = importlib.util.module_from_spec(loader)
loader.loader.exec_module(checker)


def build_tree(build, suite, results, run, timeout):
    """Build native tools and all targets using the caller's shared jobserver."""
    status = run(['/usr/bin/make', '-C', suite, 'build-fpcmp', 'build-timeit'],
                 results / 'build-tools.log', 3600)
    if status:
        raise ValueError(f'Native test tools failed: exit {status}')
    status = run(['/usr/bin/make', '-C', suite, '--keep-going'], results / 'build.log', timeout)
    if status == 124 or status < 0:
        return status  # The coordinator must stop the pool before any validation.
    # Preserve raw diagnostics while retaining upstream hashing/comparison.
    tools = suite / 'tools'
    if not (tools / 'HashProgramOutput.sh').is_file():
        raise ValueError('Missing generated HashProgramOutput.sh')
    shutil.copyfile(build / 'test-suite-src/HashProgramOutput.sh', tools / 'HashProgramOutput.original.sh')
    shutil.copyfile(SCRIPTS / 'ptsan-preserve-hash-output.sh', tools / 'HashProgramOutput.sh')
    (tools / 'HashProgramOutput.sh').chmod(0o755)
    return status


def build_worker(path):
    job = json.loads(path.read_text())
    results = Path(job["results"])
    write_json(results / "build-started.json", {"pid": os.getpid()})
    row = {"build_exit": None, "problem": "Build worker interrupted", "fatal": True}
    try:
        flags, fds = jobserver()
        env = minimal_environment()
        env['MAKEFLAGS'] = flags
        deadline = time.monotonic() + job['timeout']

        def run(command, log, timeout):
            with log.open('w') as stream:
                process = subprocess.Popen(command, env=env, pass_fds=fds,
                                           stdout=stream, stderr=subprocess.STDOUT)
                try:
                    return process.wait(timeout=max(0, min(timeout, deadline - time.monotonic())))
                except subprocess.TimeoutExpired:
                    # Descendants stay in the shared pool; the coordinator kills it.
                    return 124

        build = Path(job['build'])
        status = build_tree(build, build / 'multisource' / job['mode'], results, run, job['timeout'])
        rules = checker.expectations(json.loads((results / "expectations.json").read_text()), job["mode"])
        errors = checker.check_build((results / "build.log").read_text(), status, rules, "make")
        row = {"build_exit": status, "problem": "; ".join(errors) or None,
               "fatal": status == 124 or status < 0}
    except (OSError, ValueError) as exc:
        row["problem"] = f"Build worker failed: {exc}"
    finally:
        write_json(results / "build-result.json", row)
    return bool(row["problem"])


def logged(command, path):
    """Stream configure/lit output to both the Actions log and its artifact."""
    env = {key: value for key, value in os.environ.items()
           if key not in ('LIT_FILTER', 'LIT_FILTER_OUT', 'LIT_OPTS')}
    with path.open('w') as log, subprocess.Popen(command, env=env, text=True, errors="replace",
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
        for line in process.stdout:
            log.write(line)
            print(line, end='', flush=True)
        return process.wait()


def configure(build, workspace, suite, results, mode):
    for source, destination in [('ptsan-multisource.json', 'expectations.json'),
                                ('ptsan-multisource-tests.txt', 'expected-tests.txt')]:
        shutil.copyfile(workspace / '.github/expectations' / source, results / destination)
    command = multisource_command(build, workspace, suite, mode, runtime_validate=True)
    if logged(command, results / 'configure.log'):
        raise ValueError(f'MultiSource configure failed: {mode}')
    for name in ('CMakeCache.txt', 'compile_commands.json'):
        shutil.copyfile(suite / name, results / name)
    generated = sorted(str(p.relative_to(suite / 'MultiSource'))
                       for p in (suite / 'MultiSource').rglob('*.test'))
    (results / 'generated-tests.txt').write_text('\n'.join(generated) + '\n')


def validate(build, suite, results, mode, build_exit):
    status = logged([str(build / 'bin/llvm-lit'), '-sv', f'-j{len(os.sched_getaffinity(0))}',
                     '--timeout=300', f'--output={results}/results.json',
                     f'--xunit-xml-output={results}/results.xml', str(suite / 'MultiSource')],
                    results / 'test.log')
    if (suite / 'test.log').is_file():
        shutil.copyfile(suite / 'test.log', results / 'lit-debug.log')
    with tarfile.open(results / 'execution-records.tar.gz', 'w:gz') as archive:
        for path in sorted((suite / 'MultiSource').rglob('*')):
            if 'Output' in path.parts and path.suffix in ('.time', '.script'):
                archive.add(path, arcname=str(path.relative_to(suite)))
    return subprocess.call([sys.executable, str(SCRIPTS / 'check-ptsan-multisource.py'),
                            '--suite', str(suite), '--results', str(results), '--mode', mode,
                            '--build-exit', str(build_exit), '--build-system', 'make',
                            '--lit-exit', str(status)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", choices=["build", "validate"])
    parser.add_argument("mode", nargs="?", choices=MODES)
    parser.add_argument("--build-worker", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--modes", choices=["software", "lam", "id32", "all"], default="all")
    args = parser.parse_args()
    modes = selected_modes(args.modes)
    if args.mode and args.mode not in modes:
        parser.error("validation mode is outside the selected build group")
    if args.build_worker:
        return build_worker(args.build_worker)
    if args.action is None or (args.action == "validate") != (args.mode is not None):
        parser.error("use 'build' for all configurations or 'validate MODE'")
    build = Path(os.environ["BUILD_DIR"]).resolve()
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    root = build / "multisource"
    results = workspace / "ci-results/multisource"
    expectations = workspace / ".github/expectations"
    plan = {"schema": 1, "modes": list(modes),
            "expectations": json.loads((expectations / "ptsan-multisource.json").read_text()),
            "inventory": (expectations / "ptsan-multisource-tests.txt").read_text()}
    if args.action == "build":
        root.mkdir(parents=True, exist_ok=False)
        results.mkdir(parents=True, exist_ok=True)
        jobs = []
        for mode in modes:
            # Configuring is short and also performs compiler probes; do it before
            # sharing compilation slots. Refuse old artifacts in either location.
            (results / mode).mkdir()
            configure(build, workspace, root / mode, results / mode, mode)
            jobs.append({"name": "MultiSource", "mode": mode, "build": str(build),
                         "workspace": str(workspace), "results": str(results / mode),
                         "timeout": 3600})
        write_json(root / "build-plan.json", plan)
        aborted = run_build_batch(root, results, jobs, len(os.sched_getaffinity(0)),
                                  worker_script=Path(__file__).resolve())
        errors = []
        if aborted:
            errors.append(f"Build batch aborted: {aborted}")
        for mode in modes:
            path = results / mode / "build-result.json"
            problem = json.loads(path.read_text())["problem"] if path.exists() else "Missing build result"
            if problem:
                errors.append(f"{mode}: {problem}")
        summary = "### MultiSource builds\n\n" + ("\n".join(errors) if errors else
            f"All {len(modes)} builds match expectations, including musl CLAMR build failures.") + "\n"
        (results / "build-summary.md").write_text(summary)
        print(summary, flush=True)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as stream:
                stream.write(summary)
        return bool(errors)

    if json.loads((root / "build-plan.json").read_text()) != plan:
        raise ValueError("MultiSource build plan does not match current expectations")
    phase = json.loads((results / "build-phase.json").read_text())
    if phase["aborted"] or phase["exit"] is None:
        raise ValueError(f"MultiSource build batch incomplete or aborted: {phase}")
    row = json.loads((results / args.mode / "build-result.json").read_text())
    if row["fatal"] or row["build_exit"] is None:
        raise ValueError(f"MultiSource build did not finish: {row}")
    with (root / args.mode / "validation-started.json").open("x") as stream:
        json.dump({"pid": os.getpid()}, stream)
    return validate(build, root / args.mode, results / args.mode, args.mode, row["build_exit"])


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    sys.exit(main())
