#!/usr/bin/env python3
"""Build and validate the supported Fleetbench subset with the CI toolchains."""
import argparse
import ast
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.request

from ptsan_ci_builds import minimal_environment, stop_group, write_json
from ptsan_ci_config import benchmark_flags
from ptsan_ci_modes import BASELINES, CONFIGURATIONS, selected_modes

REVISION = '86b091782b7600c3c806cb8bc70c51acc5fad64b'
BAZEL_SHA256 = '18417fdee645a8dc08bc3f27e33f71a5681f9f14661b33ac48536a31dc8d00f1'
TOOLCHAIN = 'rules_cc++cc_configure_extension+local_config_cc'
# Snappy is the supported compression workload; TCMalloc replaces our allocator.
FAMILIES = {
    'proto': ('//fleetbench/proto:proto_benchmark', 1),
    'swissmap': ('//fleetbench/swissmap:swissmap_benchmark', 1),
    'libc': ('//fleetbench/libc:mem_benchmark', 5),
    'compression': ('//fleetbench/compression:compression_benchmark', 2),
    'hashing': ('//fleetbench/hashing:hashing_benchmark', 3),
    'cord': ('//fleetbench/stl:cord_benchmark', 1),
    'rpc': ('//fleetbench/rpc:rpc_benchmark', 1),
    'simd': ('//fleetbench/simd:simd_benchmark', 3),
}
CASE_COUNT = sum(count for _, count in FAMILIES.values())
EXHAUSTION = 'PTSan: ID space exhausted in strict mode'
# Strict ID16 exhausts the ID pool in these families. Keep their pinned case
# names so reports can exclude the whole family, including any partial timings.
ID16_EXHAUSTION_CASES = {
    'proto': ('BM_PROTO_Arena',),
    'swissmap': ('BM_SWISSMAP_InsertMiss<::absl::flat_hash_set, 64>/set_size:64/density:0',),
    'libc': ('BM_LIBC_Memcpy_Fleet_Mixed', 'BM_LIBC_Memmove_Fleet_Mixed',
             'BM_LIBC_Memcmp_Fleet_Mixed', 'BM_LIBC_Bcmp_Fleet_Mixed',
             'BM_LIBC_Memset_Fleet_Mixed'),
    'cord': ('BM_CORD_Fleet',),
    'rpc': ('BM_RPC_Fleet/process_time',),
}
DIAGNOSTICS = re.compile(
    r'^.*(?:Found bad |PTSan.*(?:ERROR|rejected|exhausted)|Failed (?:accept|connect|recv|send|bind|listen)'
    r'|Bad address|Assertion.*failed|FATAL|Check failed|ERROR:).*$', re.M | re.I)


def build_variant(mode, family):
    # Cordz samples native stack frames, including through Protobuf's Cord use.
    # Both need the indicator ABI throughout their dependency builds; mixed
    # decoding remains local to stacktrace.cc. Other workloads stay markerless.
    if mode == 'musl-strict-id32' and family in ('cord', 'proto'):
        return mode + '-indicator'
    return mode


def compiler_flags(mode, performance=False, indicator=False):
    flags = shlex.split(benchmark_flags(mode, runtime_validate=not performance)[1])
    if indicator:
        flags[flags.index('--config=ptsan-musl-id32-strict.cfg')] = \
            '--config=ptsan-musl-id32-strict-indicator.cfg'
    if not CONFIGURATIONS[mode].baseline:
        flags += ['-mllvm', '-ptsan-reoptimize']
    return flags


def run(command, log, cwd, timeout=14400, cancel=None):
    write_json(log.with_suffix('.command.json'), list(map(str, command)))
    with log.open('w') as stream:
        process = subprocess.Popen(list(map(str, command)), cwd=cwd, env=minimal_environment(),
                                   stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            if cancel is None:
                return process.wait(timeout=timeout)
            deadline = time.monotonic() + timeout
            while process.poll() is None:
                if cancel.wait(0.2):
                    return 143
                if time.monotonic() >= deadline:
                    return 124
            return process.returncode
        except subprocess.TimeoutExpired:
            return 124
        finally:
            stop_group(process)


def checked(command, log, cwd, timeout=14400):
    status = run(command, log, cwd, timeout)
    if status:
        raise ValueError(f'Command failed ({status}); see {log}')
    return log.read_text().strip()


def fetch(build, workspace):
    root = build / 'fleetbench'
    root.mkdir()
    results = workspace / 'ci-results/fleetbench'
    results.mkdir(parents=True, exist_ok=True)
    source = root / 'src'
    source.mkdir()
    checked(['git', 'init', source], results / 'init.log', root)
    checked(['git', 'fetch', '--depth=1', 'https://github.com/google/fleetbench.git', REVISION],
            results / 'fetch.log', source)
    checked(['git', 'checkout', '--detach', 'FETCH_HEAD'], results / 'checkout.log', source)
    checked(['git', 'apply', workspace / '.github/fleetbench/snappy.patch'],
            results / 'patch.log', source)
    shutil.copyfile(workspace / '.github/fleetbench/snappy.patch', results / 'snappy.patch')
    bazel = root / 'bazel'
    urllib.request.urlretrieve('https://releases.bazel.build/8.0.0/release/bazel-8.0.0-linux-x86_64', bazel)
    if hashlib.sha256(bazel.read_bytes()).hexdigest() != BAZEL_SHA256:
        raise ValueError('Bazel checksum mismatch')
    bazel.chmod(0o755)
    write_json(results / 'source.json', {'revision': REVISION, 'bazel': '8.0.0',
               'bazel_sha256': BAZEL_SHA256, 'patch': 'snappy.patch'})
    # libnuma's repository rule runs autoconf before Bazel compilation starts.
    for tool in ('autoconf', 'automake', 'libtoolize'):
        if not shutil.which(tool):
            raise ValueError(f'Fleetbench requires {tool} on the runner')


def bazel_command(root, output, rc):
    return [root / 'bazel', '--batch', '--host_jvm_args=-Xmx2048m',
            f'--output_user_root={output}', '--nosystem_rc', '--nohome_rc', f'--bazelrc={rc}']


def build_schedule(build_count, build_parallelism=None, build_jobs=None, cpus=None):
    if build_count <= 0:
        raise ValueError('Fleetbench requires at least one build')
    cpus = len(os.sched_getaffinity(0)) if cpus is None else cpus
    if cpus <= 0:
        raise ValueError('Fleetbench requires at least one available CPU')
    parallelism = min(4, build_count) if build_parallelism is None else build_parallelism
    if parallelism <= 0:
        raise ValueError('--build-parallelism must be positive')
    parallelism = min(parallelism, build_count)
    jobs = (min(cpus, max(1, 2 * cpus // parallelism))
            if build_jobs is None else build_jobs)
    if jobs <= 0:
        raise ValueError('--build-jobs must be positive')
    return parallelism, jobs


def run_variant_build(job, cancel):
    started = time.monotonic()
    result = {'problem': 'Build interrupted'}
    try:
        print(f"Building Fleetbench {job['variant']}", flush=True)
        status = run([*job['prefix'], 'build', '--keep_going', *job['targets']],
                     job['case'] / 'build.log', job['source'], cancel=cancel)
        if status:
            raise ValueError(f"Build failed ({status}); see {job['case']}/build.log")
        bin_dir = checked([*job['prefix'], 'info', 'bazel-bin'],
                          job['case'] / 'bazel-bin.log', job['source']).splitlines()[-1]
        result.update(problem=None, bin_dir=bin_dir)
    except (OSError, ValueError, IndexError) as exc:
        result['problem'] = str(exc)
    result['elapsed_seconds'] = round(time.monotonic() - started, 3)
    return job['variant'], result


def configure_toolchain(template, destination, wrapper, includes, module_generator):
    shutil.copytree(template, destination, symlinks=False)
    path = destination / 'BUILD'
    source = path.read_text()
    # The generated BUILD uses Python-compatible literals. Replace attributes of
    # the native toolchain, retaining Bazel's discovered tools and host includes.
    tree = ast.parse(source)
    call = next(node.value for node in tree.body if isinstance(node, ast.Expr)
                and isinstance(node.value, ast.Call) and isinstance(node.value.func, ast.Name)
                and node.value.func.id == 'cc_toolchain_config')
    replacements = []
    lines = source.splitlines(keepends=True)
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line))
    for keyword in call.keywords:
        if keyword.arg not in ('tool_paths', 'cxx_builtin_include_directories'):
            continue
        value = ast.literal_eval(keyword.value)
        if keyword.arg == 'tool_paths':
            value['gcc'] = str(wrapper)
        else:
            value = list(dict.fromkeys(value + includes))
            # Bazel also checks module ownership of system headers. Regenerate
            # its system module map for the combined host and target headers.
            module_map = subprocess.check_output(['bash', module_generator, *value],
                text=True, env=minimal_environment(), timeout=60)
            (destination / 'module.modulemap').write_text(module_map)
        node = keyword.value
        replacements.append((offsets[node.lineno - 1] + node.col_offset,
                             offsets[node.end_lineno - 1] + node.end_col_offset, json.dumps(value)))
    for start, end, text in sorted(replacements, reverse=True):
        source = source[:start] + text + source[end:]
    path.write_text(source)


def build_modes(build, workspace, performance=False, group='software',
                build_parallelism=None, build_jobs=None):
    root = build / 'fleetbench'
    source = root / 'src'
    results = workspace / 'ci-results/fleetbench'
    selected = (BASELINES if performance else ()) + selected_modes(group)
    plan = {'modes': list(selected), 'group': group, 'performance': performance, 'revision': REVISION,
            'families': FAMILIES, 'builds': {}}
    write_json(root / 'plan.json', plan)
    native = [f'--repo_env=CC={build}/bin/clang', f'--repo_env=CXX={build}/bin/clang++']
    prefix = bazel_command(root, root / 'bootstrap', '/dev/null')
    checked([*prefix, 'build', '--config=clang', *native,
             f'--repository_cache={root}/repository-cache', '--nobuild', FAMILIES['proto'][0]],
            results / 'bootstrap.log', source)
    output = checked([*prefix, 'info', *native, 'output_base'], results / 'output-base.log', source)
    template = Path(output.splitlines()[-1]) / 'external' / TOOLCHAIN
    module_generator = next(template.parent.glob('rules_cc*/cc/private/toolchain/generate_system_module_map.sh'))
    variants = dict.fromkeys((mode, build_variant(mode, family))
                             for mode in selected for family in FAMILIES)
    parallelism, jobs = build_schedule(len(variants), build_parallelism, build_jobs)
    plan.update(build_parallelism=parallelism, build_jobs=jobs,
                local_ram_resources_mb=18000)
    pending = []
    for mode, variant in variants:
        families = {family: target for family, (target, _) in FAMILIES.items()
                    if build_variant(mode, family) == variant}
        indicator = variant != mode
        out = root / variant
        out.mkdir()
        case = results / variant
        case.mkdir()
        record = {'problem': 'Build interrupted', 'families': list(families)}
        plan['builds'][variant] = record
        write_json(root / 'plan.json', plan)
        try:
            flags = compiler_flags(mode, performance, indicator)
            config = {'build': str(build), 'mode': mode, 'flags': flags}
            write_json(out / 'compiler.json', config)
            wrapper = out / 'compiler'
            script = workspace / '.github/scripts/ptsan-fleetbench-compiler.py'
            wrapper.write_text('#!/bin/sh\nexec ' + shlex.join(
                [sys.executable, str(script), str(out / 'compiler.json')]) + ' "$@"\n')
            wrapper.chmod(0o755)
            # Query the actual driver configs rather than duplicating sysroot or
            # libc++ paths in CI. These directories must be declared to Bazel.
            probe = checked([build / 'bin/clang', '--no-default-config', '-no-canonical-prefixes', *flags,
                             '-E', '-x', 'c++', '-v', '/dev/null'], case / 'includes.log', source)
            search = probe.split('#include <...> search starts here:\n')[1].split('End of search list.')[0]
            includes = list(dict.fromkeys(path for line in search.splitlines()
                            for path in (line.strip(), str(Path(line.strip()).resolve()))))
            configure_toolchain(template, out / 'toolchain', wrapper, includes, module_generator)
            lines = ['build --config=clang', 'build --config=opt', 'build --config=haswell',
                     f'common --override_repository={TOOLCHAIN}={out}/toolchain',
                     f'common --repository_cache={root}/repository-cache',
                     *['build ' + arg for arg in native],
                     'build --custom_malloc=@bazel_tools//tools/cpp:malloc',
                     'build --dynamic_mode=off', 'build --features=-supports_dynamic_linker',
                     'build --features=-supports_start_end_lib',
                     f'build --jobs={jobs}', 'build --local_ram_resources=18000',
                     'build --copt=-DPTSAN_FLEETBENCH_TARGET',
                     'build --linkopt=-DPTSAN_FLEETBENCH_TARGET',
                     'build --copt=-U_FORTIFY_SOURCE', 'build --copt=-DABSL_FORCE_WAITER_MODE=2',
                     'build --linkopt=-Wl,--build-id=sha1']
            if CONFIGURATIONS[mode].musl:
                lines += ['build --copt=-idirafter --copt=/usr/include',
                          'build --copt=-idirafter --copt=/usr/include/x86_64-linux-gnu']
            if not CONFIGURATIONS[mode].baseline:
                # LLVM libc's memcpy family is the benchmarked implementation.
                # Protobuf shares a linker section; Abseil walks raw stack frames.
                lines += [r'build --per_file_copt=libc/src/string/(memcpy|memmove|memset)\.cpp$@-fno-sanitize=ptsan',
                          r'build --per_file_copt=google/protobuf/generated_message_util\.cc$@-mllvm,-ptsan-global-section=pb_defaults',
                          r'build --per_file_copt=absl/debugging/stacktrace\.cc$@-mllvm,-ptsan-nochecks']
            if indicator:
                lines += [r'build --per_file_copt=absl/debugging/stacktrace\.cc$@-mllvm,-ptsan-may-decode-raw']
            rc = out / 'config.bazelrc'
            rc.write_text('\n'.join(lines) + '\n')
            shutil.copyfile(rc, case / rc.name)
            shutil.copyfile(out / 'compiler.json', case / 'compiler.json')
            prefix = bazel_command(root, out / 'build', rc)
            pending.append({'variant': variant, 'prefix': prefix,
                            'targets': list(families.values()), 'case': case,
                            'source': source})
        except (OSError, ValueError, IndexError) as exc:
            record['problem'] = str(exc)
        write_json(root / 'plan.json', plan)
        write_json(results / 'plan.json', plan)

    cancel = threading.Event()
    executor = ThreadPoolExecutor(max_workers=parallelism)
    futures = [executor.submit(run_variant_build, job, cancel) for job in pending]
    try:
        for future in as_completed(futures):
            variant, result = future.result()
            plan['builds'][variant].update(result)
            write_json(root / 'plan.json', plan)
            write_json(results / 'plan.json', plan)
    except BaseException:
        cancel.set()
        for future in futures:
            future.cancel()
        executor.shutdown(wait=True, cancel_futures=True)
        raise
    else:
        executor.shutdown()
    if (source / 'MODULE.bazel.lock').exists():
        shutil.copyfile(source / 'MODULE.bazel.lock', results / 'MODULE.bazel.lock')
    return any(row['problem'] for row in plan['builds'].values())


def case_seconds(case):
    return case['cpu_time'] * {'ns': 1e-9, 'us': 1e-6, 'ms': 1e-3, 's': 1}[case['time_unit']]


def check_cases(data, expected):
    cases = data['benchmarks']
    if len(cases) != expected or len({case['name'] for case in cases}) != expected:
        raise ValueError(f'Expected {expected} distinct cases, got {len(cases)}')
    for case in cases:
        if case.get('error_occurred') or case.get('iterations', 0) <= 0:
            raise ValueError(f"Failed or empty benchmark: {case['name']}")
        seconds = case_seconds(case)
        if not math.isfinite(seconds) or seconds <= 0:
            raise ValueError(f"Invalid timing: {case['name']}")
    return cases


def run_mode(build, mode, destination, repetition=1, performance=False):
    plan = json.loads((build / 'fleetbench/plan.json').read_text())
    if plan['performance'] != performance:
        raise ValueError('Fleetbench build and run policies differ')
    # Some runners hide CPU cache topology in sysfs. Use verified host sizes
    # when supplied, consistently for correctness, measurements and baselines.
    cache_flags = []
    for flag in ('L1_data_size', 'L2_size', 'L3_size'):
        variable = 'FLEETBENCH_' + flag.upper()
        value = os.environ.get(variable)
        if value:
            size = int(value)
            if size <= 0:
                raise ValueError(f'{variable} must be a positive size in bytes')
            cache_flags.append(f'--{flag}={size}')
    rows, errors = [], []
    for family, (target, expected) in FAMILIES.items():
        case = destination / family
        case.mkdir(parents=True, exist_ok=False)
        try:
            variant = build_variant(mode, family)
            built = plan['builds'][variant]
            if built['problem']:
                raise ValueError(built['problem'])
            exe = Path(built['bin_dir']) / target[2:].replace(':', '/')
            args = [exe, '--prevent_numa_migrations=false', '--benchmark_repetitions=1',
                    '--benchmark_min_time=' + ('1s' if performance else '0.01s'),
                    f'--benchmark_out={case}/results.json', '--benchmark_out_format=json',
                    *cache_flags]
            if family == 'compression':
                args += ['--benchmark_filter=BM_COMPRESSION_Snappy_(COMPRESS|DECOMPRESS)_Fleet$']
            print(f'Running Fleetbench {mode}/{family}, repetition {repetition}', flush=True)
            status = run(args, case / 'run.log', Path(str(exe) + '.runfiles/_main'), timeout=900)
            diagnostics = DIAGNOSTICS.findall((case / 'run.log').read_text(errors='replace'))
            exhausted_cases = ID16_EXHAUSTION_CASES.get(family, ()) if mode == 'musl-strict' else ()
            if (exhausted_cases and status == 1
                    and {line.strip() for line in diagnostics} == {EXHAUSTION}):
                for name in exhausted_cases:
                    rows.append({'suite': 'fleetbench', 'mode': mode, 'build_variant': variant,
                                 'name': family + '/' + name, 'repetition': repetition,
                                 'category': 'out-of-ids', 'matched': True, 'seconds': None,
                                 'iterations': None, 'diagnostics': diagnostics,
                                 'detail': 'Expected family ID exhaustion (exit 1); all family timings excluded'})
                continue
            if status or diagnostics:
                raise ValueError(f'Exit {status}; diagnostics: {diagnostics}')
            cases = check_cases(json.loads((case / 'results.json').read_text()), expected)
            if exhausted_cases:
                raise ValueError('Unexpected pass; review before removing the ID-exhaustion expectation')
            for result in cases:
                rows.append({'suite': 'fleetbench', 'mode': mode, 'build_variant': variant,
                             'name': family + '/' + result['name'],
                             'repetition': repetition, 'category': 'pass', 'matched': True,
                             'seconds': case_seconds(result), 'iterations': result['iterations'], 'detail': None})
        except (OSError, ValueError, KeyError) as exc:
            errors.append(f'{mode}/{family}: {exc}')
    write_json(destination / 'checked-results.json', {'rows': rows, 'errors': errors})
    return rows, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('fetch', 'build', 'validate'))
    parser.add_argument('--performance', action='store_true')
    parser.add_argument('--group', choices=('software', 'id32'), default='software')
    parser.add_argument('--build-parallelism', type=int)
    parser.add_argument('--build-jobs', type=int)
    args = parser.parse_args()
    build = Path(os.environ['BUILD_DIR']).resolve()
    workspace = Path(os.environ['GITHUB_WORKSPACE']).resolve()
    if args.action == 'fetch':
        return fetch(build, workspace)
    if args.action == 'build':
        return build_modes(build, workspace, args.performance, args.group,
                           args.build_parallelism, args.build_jobs)
    failed = False
    for mode in selected_modes(args.group):
        rows, errors = run_mode(build, mode, workspace / 'ci-results/fleetbench/runs' / mode)
        failed |= bool(errors)
        passed = sum(row['category'] == 'pass' for row in rows)
        exhausted = sum(row['category'] == 'out-of-ids' for row in rows)
        summary = f'{passed} cases passed; {exhausted} cases excluded by expected family ID exhaustion'
        print(f'{mode}: ' + ('; '.join(errors) if errors else summary), flush=True)
    return failed


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    sys.exit(main())
