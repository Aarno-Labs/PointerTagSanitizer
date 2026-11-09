"""Supervise CI build workers sharing a GNU Make jobserver."""
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import threading
import time


def write_json(path, data):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n")
    temporary.replace(path)


def minimal_environment():
    # SPEC records its environment; exclude credentials and unrelated settings.
    env = {key: os.environ[key] for key in
           ("HOME", "USER", "LOGNAME", "RUNNER_TRACKING_ID") if key in os.environ}
    env.update(PATH="/usr/local/bin:/usr/bin:/bin", LC_ALL="C")
    return env


def jobserver():
    flags = os.environ.get("MAKEFLAGS", "")
    matches = re.findall(r"(?:^|\s)--jobserver-auth=(\d+),(\d+)(?=\s|$)", flags)
    if not matches:
        raise ValueError("Build worker requires GNU Make's pipe jobserver")
    fds = tuple(map(int, matches[-1]))
    for fd in fds:
        os.fstat(fd)  # Fail closed instead of silently running serially/unlimited.
    return flags, fds


def stop_group(process):
    # Every build descendant stays in this group, including runcpu and compilers.
    # Ignore repeated termination while cleaning up, then restore the handler.
    main_thread = threading.current_thread() is threading.main_thread()
    previous = signal.signal(signal.SIGTERM, signal.SIG_IGN) if main_thread else None
    try:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        # SIGKILL delivery is asynchronous. Wait until descendants have actually
        # stopped before a validation run or another configuration may start.
        deadline = time.monotonic() + 5
        while live_group_processes(process.pid):
            if time.monotonic() >= deadline:
                raise RuntimeError(f"Process group {process.pid} did not stop after SIGKILL")
            time.sleep(0.05)
    finally:
        if main_thread:
            signal.signal(signal.SIGTERM, previous)


def live_group_processes(group):
    live = []
    for path in Path("/proc").glob("[0-9]*/stat"):
        try:
            fields = path.read_text().rsplit(") ", 1)[1].split()
            if int(fields[2]) == group and fields[0] not in ("Z", "X"):
                live.append(int(path.parent.name))
        except (OSError, ValueError, IndexError):
            continue
    return live


def run_build_batch(root, results, jobs, slots, worker_script=None):
    """Run all builds, returning an abort reason for infrastructure failures."""
    script = (worker_script or Path(__file__).with_name("ptsan-spec.py")).resolve()
    targets = [f"{job['mode']}/{job['name']}" for job in jobs]
    if len(set(targets)) != len(targets):
        raise ValueError("Duplicate build targets")
    lines = [".PHONY: all " + " ".join(targets), "all: " + " ".join(targets)]
    for target, job in zip(targets, jobs):
        path = Path(job["results"]) / "build-job.json"
        write_json(path, job)
        command = shlex.join([sys.executable, str(script), "--build-worker", str(path)])
        # '+' makes GNU Make retain its jobserver descriptors for the worker.
        lines += [target + ":", "\t+" + command.replace("$", "$$")]
    makefile = root / "builds.mk"
    makefile.write_text("\n".join(lines) + "\n")
    command = ["/usr/bin/make", "--keep-going", f"--jobs={slots}", "-f", str(makefile)]
    write_json(results / "build-phase.command.json", command)
    aborted = None
    process = None
    status = None
    last_summary = None
    last_update = 0
    try:
        with (results / "build-phase.log").open("w") as log:
            process = subprocess.Popen(command, cwd=root, env=minimal_environment(),
                                       stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            while True:
                completed = []
                active = []
                for job in jobs:
                    directory = Path(job["results"])
                    result_path = directory / "build-result.json"
                    if result_path.exists():
                        result = json.loads(result_path.read_text())
                        completed.append({"mode": job["mode"], "name": job["name"], **result})
                        if result.get("fatal"):
                            aborted = f"{job['mode']}/{job['name']}: {result['problem']}"
                    elif (directory / "build-started.json").exists():
                        started = json.loads((directory / "build-started.json").read_text())
                        active.append(f"{job['mode']}/{job['name']}")
                        try:
                            os.kill(started["pid"], 0)
                        except ProcessLookupError:
                            if not result_path.exists():
                                aborted = f"{job['mode']}/{job['name']}: build worker disappeared"
                summary = {"completed": completed, "active": active, "aborted": aborted}
                if summary != last_summary:
                    write_json(results / "build-progress.json", summary)
                    last_summary = summary
                now = time.monotonic()
                if now - last_update >= 30:
                    print(f"Builds: {len(completed)}/{len(jobs)} complete; "
                          f"{len(active)} active", flush=True)
                    last_update = now
                status = process.poll()
                if aborted or status is not None:
                    if status is not None and len(completed) != len(jobs) and not aborted:
                        aborted = "Build scheduler exited without all worker results"
                    if status is not None and status != 0 and not aborted and all(
                            result["problem"] is None for result in completed):
                        aborted = f"Build scheduler failed unexpectedly (exit {status})"
                    break
                time.sleep(0.2)
    except (KeyboardInterrupt, SystemExit):
        aborted = "Build batch cancelled"
    except (OSError, ValueError) as exc:
        aborted = f"Build scheduler failed: {exc}"
    finally:
        if process is not None:
            try:
                stop_group(process)
            except RuntimeError as exc:
                aborted = str(exc)
            status = process.returncode
        write_json(results / "build-phase.json", {"exit": status, "aborted": aborted})
        completed = []
        for job in jobs:
            path = Path(job["results"]) / "build-result.json"
            if path.exists():
                completed.append({"mode": job["mode"], "name": job["name"],
                                  **json.loads(path.read_text())})
        write_json(results / "build-progress.json", {
            "completed": completed, "active": [], "aborted": aborted, "complete": True})
    return aborted
