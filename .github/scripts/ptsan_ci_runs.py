"""Bounded parallel correctness runs; each worker owns a separate process group."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from ptsan_ci_builds import minimal_environment, stop_group, write_json


def run_slots(requested=None):
    available = len(os.sched_getaffinity(0))
    return min(requested if requested is not None else available, available)


def run_validation_batch(jobs, slots, results, on_result, worker_script=None):
    """Continue ordinary failures/timeouts; stop every descendant on cancellation."""
    script = worker_script or Path(__file__).with_name('ptsan-spec.py')
    targets = [(job['mode'], job['name']) for job in jobs]
    if len(set(targets)) != len(targets):
        raise ValueError('Duplicate ref run targets')
    pending = list(jobs)
    active = []
    completed = []
    aborted = None
    last_update = 0
    try:
        while pending or active:
            while pending and len(active) < slots:
                job = pending.pop(0)
                directory = Path(job['results'])
                path = directory / 'run-job.json'
                write_json(path, job)
                log = (directory / 'worker.log').open('w')
                try:
                    process = subprocess.Popen([sys.executable, str(script), '--run-worker', str(path)],
                        stdout=log, stderr=subprocess.STDOUT, env=minimal_environment(), start_new_session=True)
                finally:
                    log.close()
                active.append((job, process, time.monotonic()))
                print(f"Starting ref run: {job['mode']}/{job['name']}", flush=True)
            for job, process, started in list(active):
                status = process.poll()
                timed_out = time.monotonic() - started > job['timeout'] + 15
                if status is None and not timed_out:
                    continue
                stop_group(process)
                path = Path(job['results']) / 'run-result.json'
                if path.exists() and status == 0 and not timed_out:
                    row = json.loads(path.read_text())
                    if (row['mode'], row['name']) != (job['mode'], job['name']):
                        raise ValueError(f"Mismatched ref result for {job['mode']}/{job['name']}")
                else:
                    row = {'mode': job['mode'], 'name': job['name'],
                           'category': job['rule']['category'] if job['rule'] else 'pass',
                           'matched': False, 'detail': 'Run worker timed out' if timed_out else
                           f'Run worker failed or left no result (exit {status})'}
                write_json(path, row)
                completed.append(row)
                active.remove((job, process, started))
                on_result(row)
            summary = {'completed': completed,
                       'active': [f"{j['mode']}/{j['name']}" for j, _, _ in active],
                       'pending': [f"{j['mode']}/{j['name']}" for j in pending], 'slots': slots}
            write_json(results / 'run-progress.json', summary)
            if time.monotonic() - last_update >= 30:
                print(f"Ref runs: {len(completed)}/{len(jobs)} complete; {len(active)} active; "
                      f"{len(pending)} queued (limit {slots})", flush=True)
                last_update = time.monotonic()
            if pending or active:
                time.sleep(0.2)
    except BaseException as exc:
        aborted = f'{type(exc).__name__}: {exc}'
        raise
    finally:
        cleanup_errors = []
        for _, process, _ in active:
            try:
                stop_group(process)
            except Exception as exc:
                cleanup_errors.append(str(exc))
        write_json(results / 'run-phase.json', {
            'complete': aborted is None and not cleanup_errors and len(completed) == len(jobs),
            'aborted': aborted, 'cleanup_errors': cleanup_errors})
        if cleanup_errors:
            raise RuntimeError('; '.join(cleanup_errors))
