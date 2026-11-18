#!/usr/bin/env python3
"""Fresh builds, followed by serial MultiSource, SPEC and Fleetbench measurements."""
import argparse
import csv
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time

from ptsan_ci_builds import jobserver, minimal_environment, run_build_batch, stop_group, write_json
from ptsan_ci_modes import BASELINES, selected_modes
from ptsan_ci_config import multisource_command, multisource_lit, spec_arguments, spec_benchmarks
from check_ptsan_spec import check_result, inspect_case
import ptsan_fleetbench as fleet

SCRIPTS = Path(__file__).resolve().parent
MIN_RATIO_SECONDS = 0.010
SUITES = ('multisource', 'spec', 'fleetbench')


def suites(group):
    # Fleetbench's validated recipe currently covers software decoding only.
    return SUITES if group in ('software', 'id32') else SUITES[:2]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


spec_runner = load('spec_runner', 'ptsan-spec.py')
ms_runner = load('ms_runner', 'ptsan-multisource.py')
ms = ms_runner.checker


def modes(group):
    if group == 'glibc-stack-off':
        return ('plain-glibc', 'glibc', 'glibc-lam')
    return BASELINES + selected_modes(group)


def benchmark_policy(group):
    # The paper used weak stack safety and preserved IDs in arithmetic/comparisons.
    # Restrict this profile to glibc; musl requires matching dependency stacks.
    paper = group == 'glibc-stack-off'
    return {'stack_temporal': 0 if paper else 1, 'preserve_ids': paper}


def nonnegative(value):
    return isinstance(value, (float, int)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0


def positive(value):
    return nonnegative(value) and value > 0


def run(command, log, timeout, shared=False):
    env = minimal_environment()
    fds = ()
    if shared:
        env['MAKEFLAGS'], fds = jobserver()
    write_json(log.with_suffix('.command.json'), list(map(str, command)))
    with log.open('w') as stream:
        p = subprocess.Popen(list(map(str, command)), env=env, pass_fds=fds,
                             stdout=stream, stderr=subprocess.STDOUT, start_new_session=not shared)
        start = time.monotonic()
        try:
            while True:
                try:
                    return p.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    if time.monotonic() - start >= timeout:
                        return 124
        finally:
            if not shared:
                stop_group(p)


def build_worker(path):
    job = json.loads(path.read_text())
    if job['suite'] == 'spec':
        return spec_runner.build_worker(path)
    results, suite = Path(job['results']), Path(job['output'])
    write_json(results / 'build-started.json', {'pid': os.getpid()})
    row = {'build_exit': None, 'problem': 'Worker interrupted', 'fatal': True}
    try:
        status = ms_runner.build_tree(Path(job['build']), suite, results,
            lambda command, log, timeout: run(command, log, timeout, shared=True), 14400)
        rules = ms.expectations(job['manifest'], job['mode'])
        errors = ms.check_build((results / 'build.log').read_text(), status, rules, 'make')
        row = {'build_exit': status, 'problem': '; '.join(errors) or None,
               'fatal': status == 124 or status < 0}
    except (OSError, ValueError) as exc:
        row.update(problem=str(exc), fatal=True)
    finally:
        write_json(results / 'build-result.json', row)
    return bool(row['problem'])


def manifests(workspace):
    root = workspace / '.github/expectations'
    multi = json.loads((root / 'ptsan-multisource.json').read_text())
    baseline = json.loads((root / 'ptsan-performance-baselines.json').read_text())
    multi['rules'] += baseline['multisource_rules']
    spec = json.loads((root / 'ptsan-spec-ref.json').read_text())
    for mode in BASELINES:
        spec['configurations'][mode] = {}
    inventory = (root / 'ptsan-multisource-tests.txt').read_text().splitlines()
    return multi, spec, inventory, baseline


def prepare(build, workspace, spec_root, group, selected):
    selected = tuple(selected)
    if (not selected or len(set(selected)) != len(selected)
            or any(suite not in suites(group) for suite in selected)):
        raise ValueError(f'Unsupported performance suites for {group}: {selected}')
    root = build / 'performance'
    root.mkdir(exist_ok=False)
    results = workspace / 'ci-results/performance'
    results.mkdir(parents=True, exist_ok=True)
    multi, spec, inventory, baseline = manifests(workspace)
    write_json(results / 'baseline-provenance.json', baseline)
    write_json(results / 'multisource-expectations.json', multi)
    write_json(results / 'spec-expectations.json', spec)
    jobs = []
    policy = benchmark_policy(group)
    for mode in modes(group):
        if 'multisource' in selected:
            out = root / 'multisource' / mode
            case = results / 'builds/multisource' / mode
            case.mkdir(parents=True)
            command = multisource_command(build, workspace, out, mode, **policy)
            if run(command, case / 'configure.log', 900):
                raise ValueError(f'MultiSource configure failed: {mode}')
            for name in ('CMakeCache.txt', 'compile_commands.json'):
                shutil.copyfile(out / name, case / name)
            generated = sorted(str(p.relative_to(out / 'MultiSource'))
                               for p in (out / 'MultiSource').rglob('*.test'))
            (case / 'generated-tests.txt').write_text('\n'.join(generated) + '\n')
            if set(generated) != set(inventory) or len(generated) != len(inventory):
                raise ValueError(f'MultiSource inventory differs: {mode}')
            jobs.append({'suite': 'multisource', 'name': 'MultiSource', 'mode': mode,
                         'build': str(build), 'results': str(case), 'output': str(out),
                         'manifest': multi})
        if 'spec' in selected:
            for name in spec_benchmarks(spec, mode):
                out = root / 'spec' / mode / name
                out.mkdir(parents=True)
                case = results / 'builds/spec' / mode / name
                case.mkdir(parents=True)
                config = out / 'ptsan.cfg'
                shutil.copyfile(workspace / '.github/spec/ptsan.cfg', config)
                arguments = [*spec_arguments(
                    build, workspace, mode, config, out, size='ref',
                    stack_temporal=policy['stack_temporal']),
                    '--action=build', '--rebuild', name]
                jobs.append({'suite': 'spec', 'name': name, 'mode': mode,
                             'spec': str(spec_root), 'output': str(out),
                             'results': str(case), 'config': str(config),
                             'timeout': 14400, 'arguments': arguments})
    # Share one jobserver across every selected build. Start long Blender
    # compilations first, then overlap MultiSource with the other SPEC builds.
    jobs.sort(key=lambda j: (0 if j['name'] == '526.blender_r' else
                            1 if j['suite'] == 'multisource' else 2, j['name']))
    plan = {'schema': 1, 'group': group, 'policy': policy,
            'suites': list(selected), 'modes': list(modes(group)), 'jobs': jobs,
            'multisource_manifest': multi, 'spec_manifest': spec, 'inventory': inventory,
            'revision': subprocess.check_output(['git', '-C', str(workspace), 'rev-parse', 'HEAD'], text=True).strip()}
    write_json(root / 'plan.json', plan)
    write_json(results / 'plan.json', plan)
    aborted = run_build_batch(root, results, jobs, len(os.sched_getaffinity(0)), Path(__file__))
    problems = []
    for job in jobs:
        row = Path(job['results']) / 'build-result.json'
        if not row.exists() or json.loads(row.read_text())['problem']:
            problems.append(f"{job['suite']}/{job['mode']}/{job['name']}")
    write_json(results / 'build-summary.json', {'aborted': aborted, 'failed': problems})
    return bool(aborted or problems)


def run_multisource(job, plan, destination, repetition, timeout):
    destination.mkdir(parents=True, exist_ok=False)
    suite = Path(job['output'])
    # Lit reuses these filenames. Remove only generated outputs, so an interrupted
    # invocation cannot be mistaken for a result from an earlier repetition.
    for directory in (suite / 'MultiSource').rglob('Output'):
        if directory.is_dir():
            shutil.rmtree(directory)
    for name in ('build.log', 'generated-tests.txt'):
        shutil.copyfile(Path(job['results']) / name, destination / name)
    status = run([multisource_lit(Path(job['build'])), '-sv', '-j1',
                  f'--timeout={timeout}', f'--output={destination}/results.json', suite / 'MultiSource'],
                 destination / 'run.log', timeout * len(plan['inventory']) + 300)
    built = json.loads((Path(job['results']) / 'build-result.json').read_text())
    checked, errors = ms.check_suite(suite, destination, plan['multisource_manifest'],
                                     set(plan['inventory']), job['mode'], built['build_exit'], status, 'make',
                                     match_diagnostics=False)
    tests = json.loads((destination / 'results.json').read_text())['tests']
    rows = multisource_rows(checked, tests, job['mode'], repetition)
    write_json(destination / 'checked-results.json', {'tests': rows, 'errors': errors})
    return rows, errors


def multisource_rows(checked, tests, mode, repetition):
    metrics = {t['name'].removeprefix('test-suite :: MultiSource/'): t.get('metrics', {}) for t in tests}
    rows = []
    for row in checked:
        row = dict(row)
        seconds = metrics.get(row['name'], {}).get('exec_time')
        if row['matched'] and row['category'] == 'pass':
            if not nonnegative(seconds):
                row.update(matched=False, detail='Missing, nonfinite, or negative exec_time')
            elif seconds == 0:
                row['detail'] = 'Zero user CPU time; excluded from ratios'
        row.update(suite='multisource', mode=mode, repetition=repetition,
                   seconds=seconds if row['matched'] and row['category'] == 'pass' else None)
        rows.append(row)
    return rows


def spec_seconds(output, benchmark):
    if benchmark.startswith(('997.', '999.')):
        return None  # correctness-only checks, never included in performance means
    values = []
    for path in (output / 'result').glob('*.rsf'):
        values += re.findall(r'^spec\.cpu2017\.results\.' + re.escape(benchmark.replace('.', '_')) +
                             r'\.base\.000\.reported_time:\s*([^\n]+)$', path.read_text(), re.M)
    if len(values) != 1 or not positive(float(values[0])):
        raise ValueError(f'Expected one positive reported_time: {values}')
    return float(values[0])


def run_spec(job, plan, destination, repetition, timeout):
    destination.mkdir(parents=True, exist_ok=False)
    rule = plan['spec_manifest']['configurations'][job['mode']].get(job['name'])
    row = {'suite': 'spec', 'mode': job['mode'], 'name': job['name'], 'repetition': repetition,
           'category': rule['category'] if rule else 'pass', 'matched': False, 'seconds': None}
    try:
        built = json.loads((Path(job['results']) / 'build-result.json').read_text())
        if built['problem']:
            raise ValueError(built['problem'])
        # Fresh output roots keep repetitions unambiguous. Copy executables and
        # the hash-bearing config, never rebuild them during measurement.
        output = Path(job['output']) / 'repetitions' / f'rep-{repetition}'
        output.mkdir(parents=True, exist_ok=False)
        relative = Path('benchspec/CPU') / job['name'] / 'exe'
        shutil.copytree(Path(job['output']) / relative, output / relative)
        config = output / 'ptsan.cfg'
        shutil.copyfile(job['config'], config)
        shutil.copyfile(config, destination / 'effective.cfg')
        commands = [f'--config={config}' if a.startswith('--config=') else
                    f'--output_root={output}' if a.startswith('--output_root=') else a
                    for a in job['arguments'][:-3]]
        status = spec_runner.invoke(Path(job['spec']), [*commands, '--action=run', '--nobuild', job['name']],
                                    destination / 'run.log', timeout)
        row['run_exit'] = status
        if status == 124 and rule is not None:
            row.update(matched=True, detail='Expected failing workload timed out; excluded from ratios')
            write_json(destination / 'checked-results.json', row)
            return row
        if status:
            raise ValueError(f'Run harness failed or timed out: exit {status}')
        evidence = inspect_case(output, job['name'], job['mode'], 'ref')
        write_json(destination / 'execution.json', evidence)
        row['diagnostics'] = evidence['diagnostics']
        problem = check_result(job['name'], rule, evidence, status, (destination / 'run.log').read_text(),
                               match_diagnostics=False)
        row.update(matched=problem is None, detail=problem)
        if row['matched'] and rule is None:
            row['seconds'] = spec_seconds(output, job['name'])
    except (OSError, ValueError) as exc:
        row.update(matched=False, seconds=None, detail=str(exc))
    write_json(destination / 'checked-results.json', row)
    return row


def environment(cpu):
    policy = Path('/sys/devices/system/cpu/intel_pstate')
    return {'cpu': cpu, 'affinity': sorted(os.sched_getaffinity(0)), 'load': os.getloadavg(),
            'time_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
            'cpu_policy': {name: (policy / name).read_text().strip()
                           for name in ('no_turbo', 'min_perf_pct', 'max_perf_pct')},
            'governors': {str(p): p.read_text().strip() for p in
                          Path('/sys/devices/system/cpu/cpufreq').glob('policy*/scaling_governor')}}


def measure(build, workspace, group, repetitions, cpu, timeout, suite):
    if suite not in suites(group):
        raise ValueError(f'Unknown measurement suite: {suite}')
    root = build / 'performance'
    results = workspace / 'ci-results/performance'
    plan = json.loads((root / 'plan.json').read_text())
    if plan['group'] != group or plan['modes'] != list(modes(group)):
        raise ValueError('Build configuration group does not match')
    if suite not in plan.get('suites', suites(group)):
        raise ValueError(f'{suite} was not selected when building')
    phase = json.loads((results / 'build-phase.json').read_text())
    if phase['aborted'] or phase['exit'] is None:
        raise ValueError(f'Build pool incomplete or aborted: {phase}')
    if cpu not in os.sched_getaffinity(0):
        raise ValueError(f'CPU {cpu} is outside the permitted CPU set')
    with (root / f'{suite}-measurement-started.json').open('x') as stream:
        json.dump({'pid': os.getpid(), 'repetitions': repetitions, 'cpu': cpu}, stream)
    before = environment(cpu)
    if before['cpu_policy'] != {'no_turbo': '1', 'min_perf_pct': '40', 'max_perf_pct': '40'}:
        raise ValueError('Expected ptsan-benchmark CPU policy before measurement')
    write_json(results / f'{suite}-environment-before.json', before)
    os.sched_setaffinity(0, {cpu})
    rows, errors = [], []
    selections = {mode: spec_benchmarks(plan['spec_manifest'], mode) for mode in plan['modes']}
    if suite == 'fleetbench':
        count = len(plan['modes']) * fleet.CASE_COUNT
    else:
        count = (len(plan['modes']) * len(plan['inventory']) if suite == 'multisource'
                 else sum(len(names) for names in selections.values()))
    expected = repetitions * count
    snapshot = {'schema': 1, 'group': group, 'modes': plan['modes'],
                'suites': plan.get('suites', list(suites(group))),
                'repetitions': repetitions, 'suite': suite, 'cpu': cpu,
                'spec_benchmark_configurations': plan['spec_manifest'].get('benchmark_configurations', {}),
                'expected_records': expected, 'complete': False, 'rows': rows, 'errors': errors}
    try:
        for rep in range(1, repetitions + 1):
            offset = (rep - 1) % len(plan['modes'])
            order = plan['modes'][offset:] + plan['modes'][:offset]
            # Exactly one lit child / one runcpu workload at a time. Rotate mode
            # order per repetition; SPEC also interleaves modes per benchmark.
            if suite == 'multisource':
                for mode in order:
                    job = next(j for j in plan['jobs'] if j['suite'] == 'multisource' and j['mode'] == mode)
                    dest = results / 'runs/multisource' / mode / f'rep-{rep}'
                    print(f'Measuring MultiSource {mode}, repetition {rep}/{repetitions}', flush=True)
                    try:
                        new, problems = run_multisource(job, plan, dest, rep, timeout)
                        rows.extend(new)
                        errors.extend(f'{mode}/rep-{rep}: {p}' for p in problems)
                    except (OSError, ValueError, KeyError) as exc:
                        errors.append(f'MultiSource {mode}/rep-{rep}: {exc}')
                    write_json(results / f'measurements-{suite}.json', snapshot)
            elif suite == 'fleetbench':
                for mode in order:
                    dest = results / 'runs/fleetbench' / mode / f'rep-{rep}'
                    try:
                        new, problems = fleet.run_mode(build, mode, dest, rep, performance=True)
                        rows.extend(new)
                        errors.extend(problems)
                    except (OSError, ValueError, KeyError) as exc:
                        errors.append(f'Fleetbench {mode}/rep-{rep}: {exc}')
                    write_json(results / f'measurements-{suite}.json', snapshot)
            else:
                for name in plan['spec_manifest']['benchmarks']:
                    for mode in order:
                        if name not in selections[mode]:
                            continue
                        job = next(j for j in plan['jobs'] if j['suite'] == 'spec' and j['mode'] == mode and j['name'] == name)
                        print(f'Measuring SPEC {name} {mode}, repetition {rep}/{repetitions}', flush=True)
                        rows.append(run_spec(job, plan, results / 'runs/spec' / mode / name / f'rep-{rep}', rep, timeout))
                        write_json(results / f'measurements-{suite}.json', snapshot)
        snapshot['complete'] = True
    finally:
        after = environment(cpu)
        if after['cpu_policy'] != before['cpu_policy'] or after['governors'] != before['governors']:
            errors.append('CPU policy changed during measurement')
        write_json(results / f'{suite}-environment-after.json', after)
        write_json(results / f'measurements-{suite}.json', snapshot)
    return not aggregate(snapshot)['success']


def aggregate(data):
    errors = list(data['errors'])
    if not data['complete'] or len(data['rows']) != data['expected_records']:
        errors.append('Missing/incomplete measurement matrix')
    samples, excluded, seen = {}, {}, set()
    restrictions = data.get('spec_benchmark_configurations', {})
    for name, allowed in restrictions.items():
        for mode in data['modes']:
            if mode not in allowed:
                excluded[('spec', name, mode)] = ['Not selected for this configuration']
    for row in data['rows']:
        key = (row['suite'], row['name'], row['mode'])
        sample_key = (*key, row['repetition'])
        if sample_key in seen or row['repetition'] not in range(1, data['repetitions'] + 1):
            errors.append(f'Duplicate or invalid repetition: {sample_key}')
            excluded.setdefault(key, []).append('Duplicate or invalid repetition')
        seen.add(sample_key)
        valid_time = nonnegative(row['seconds']) if row['suite'] == 'multisource' else positive(row['seconds'])
        if (row['matched'] and row['category'] == 'pass' and not valid_time
                and not (row['suite'] == 'spec' and row['name'].startswith(('997.', '999.')))):
            errors.append(f'Invalid successful timing: {sample_key}')
        if not row['matched']:
            errors.append(f'{sample_key}: {row.get("detail")}')
        diagnostics = row.get('diagnostics') or row.get('diagnostic_lines')
        if (row['matched'] and row['category'] == 'pass' and positive(row['seconds'])
                # Fleetbench reports per-iteration CPU time, not a whole run.
                # Its measurement loop runs for at least one second per case.
                and (row['suite'] == 'fleetbench' or row['seconds'] >= MIN_RATIO_SECONDS)
                and not diagnostics):
            samples.setdefault(key, []).append(row['seconds'])
        else:
            reason = ('Reported runtime error; excluded from ratios' if diagnostics else
                      row.get('detail') or ('Below 10 ms in at least one repetition'
                      if row['category'] == 'pass' and valid_time and row['seconds'] < MIN_RATIO_SECONDS
                      else row['category']))
            excluded.setdefault(key, []).append(reason)
    medians = {key: statistics.median(values) for key, values in samples.items()
               if len(values) == data['repetitions'] and key not in excluded}
    comparisons = []
    for suite in data_suites(data):
        names = sorted({r['name'] for r in data['rows'] if r['suite'] == suite})
        for baseline in BASELINES:
            if baseline not in data['modes']:
                continue
            for mode in data['modes']:
                if mode == baseline:
                    continue
                individual = []
                omissions = []
                for name in names:
                    left, right = (suite, name, mode), (suite, name, baseline)
                    if left not in medians or right not in medians:
                        omissions.append(name)
                        continue
                    ratio = medians[left] / medians[right]
                    individual.append({'name': name, 'seconds': medians[left], 'baseline_seconds': medians[right],
                                       'ratio': ratio, 'overhead_percent': 100 * (ratio - 1)})
                geo = math.exp(statistics.mean(math.log(r['ratio']) for r in individual)) if individual else None
                comparisons.append({'suite': suite, 'mode': mode, 'baseline': baseline,
                                    'count': len(individual), 'excluded': omissions, 'benchmarks': individual,
                                    'exclusion_reasons': {name: {m: excluded.get((suite, name, m),
                                        ['Missing or incomplete repetitions'])
                                        for m in (mode, baseline) if (suite, name, m) not in medians}
                                        for name in omissions},
                                    'geomean_ratio': geo, 'overhead_percent': 100 * (geo - 1) if geo is not None else None})
    return {'success': not errors, 'errors': errors, 'comparisons': comparisons}


def data_suites(data):
    return tuple(data.get('suites', suites(data['group'])))


def collect_measurements(results):
    parts, errors = [], []
    plan = json.loads((results / 'plan.json').read_text())
    group = plan['group']
    selected = tuple(plan.get('suites', suites(group)))
    for suite in selected:
        path = results / f'measurements-{suite}.json'
        if not path.exists():
            errors.append(f'Missing {suite} measurements')
            continue
        data = json.loads(path.read_text())
        if data['suite'] != suite or any(row['suite'] != suite for row in data['rows']):
            raise ValueError(f'Misclassified measurements in {path}')
        parts.append(data)
    if not parts:
        raise ValueError('No measurement results available')
    first = parts[0]
    for part in parts[1:]:
        if (any(part[key] != first[key] for key in ('schema', 'group', 'modes', 'repetitions', 'cpu'))
                or part.get('spec_benchmark_configurations', {}) != first.get('spec_benchmark_configurations', {})):
            raise ValueError('Measurement suites used different configurations')
    return {**{key: first[key] for key in ('schema', 'group', 'modes', 'repetitions', 'cpu')},
            'suites': list(selected),
            'spec_benchmark_configurations': first.get('spec_benchmark_configurations', {}),
            'expected_records': sum(part['expected_records'] for part in parts),
            'complete': len(parts) == len(selected) and all(part['complete'] for part in parts),
            'rows': [row for part in parts for row in part['rows']],
            'errors': errors + [error for part in parts for error in part['errors']]}


def primary_pairs(group):
    if group == 'glibc-stack-off':
        return [('glibc', 'plain-glibc'), ('glibc-lam', 'plain-glibc')]
    suffix = {'software': '', 'lam': '-lam', 'id32': '-id32'}[group]
    return [(mode + suffix, baseline) for mode, baseline in (
        ('glibc', 'plain-glibc'), ('musl-strict', 'plain-musl'), ('musl-nonstrict', 'plain-musl'))]


def write_csv(path, header, rows):
    # UTF-8 BOM helps Excel recognize diagnostic text. Keep numbers numeric;
    # quoted text from diagnostics must not become spreadsheet formulas.
    def cell(value):
        if isinstance(value, str) and value.startswith(('=', '+', '-', '@', '\t', '\r', '\n')):
            return "'" + value
        return value
    with path.open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(header)
        writer.writerows([cell(value) for value in row] for row in rows)


def export_csv(results, data, report):
    pairs = primary_pairs(data['group'])
    lookup = {(c['suite'], c['mode'], c['baseline']): c for c in report['comparisons']}
    headline = []
    for mode, baseline in pairs:
        row = [mode, baseline]
        for suite in data_suites(data):
            c = lookup.get((suite, mode, baseline), {})
            row += [c.get('overhead_percent'), c.get('count', 0)]
        headline.append([*row, report['success']])
    write_csv(results / 'primary-summary.csv',
              ['configuration', 'baseline',
               *[field for suite in data_suites(data)
                 for field in (suite + '_overhead_percent', suite + '_cases')], 'report_success'], headline)
    write_csv(results / 'summary.csv',
              ['suite', 'configuration', 'baseline', 'primary', 'cases', 'geomean_ratio',
               'overhead_percent', 'report_success'],
              [[c['suite'], c['mode'], c['baseline'], (c['mode'], c['baseline']) in pairs,
                c['count'], c['geomean_ratio'], c['overhead_percent'], report['success']]
               for c in report['comparisons']])
    # Keep excluded benchmarks in the same rows, with blank numeric cells and
    # explicit reasons; changing outcomes must not shift subsequent benchmarks.
    inventory = {}
    if (results / 'plan.json').exists():
        plan = json.loads((results / 'plan.json').read_text())
        inventory = {'multisource': plan['inventory'], 'spec': plan['spec_manifest']['benchmarks']}
    detailed = []
    for c in report['comparisons']:
        included = {r['name']: r for r in c['benchmarks']}
        for name in sorted(set(inventory.get(c['suite'], [])) | set(included) | set(c['excluded'])):
            r = included.get(name, {})
            reason = '; '.join(mode + ': ' + ', '.join(dict.fromkeys(details))
                               for mode, details in c['exclusion_reasons'].get(name, {}).items())
            if name not in included and not reason:
                reason = 'Missing measurements'
            detailed.append([c['suite'], name, c['mode'], c['baseline'], name in included,
                             r.get('seconds'), r.get('baseline_seconds'), r.get('ratio'),
                             r.get('overhead_percent'), reason, report['success']])
    write_csv(results / 'comparisons.csv',
              ['suite', 'benchmark', 'configuration', 'baseline', 'included', 'seconds',
               'baseline_seconds', 'ratio', 'overhead_percent', 'exclusion_reason', 'report_success'], detailed)
    write_csv(results / 'measurements.csv',
              ['suite', 'benchmark', 'configuration', 'repetition', 'expected_category',
               'matched', 'seconds', 'diagnostics', 'detail'],
              [[r['suite'], r['name'], r['mode'], r['repetition'], r['category'], r['matched'],
                r['seconds'], '; '.join(r.get('diagnostics') or r.get('diagnostic_lines') or []), r.get('detail')]
               for r in sorted(data['rows'], key=lambda r: (r['suite'], r['name'], r['mode'], r['repetition']))])


def summarize(results):
    data = collect_measurements(results)
    write_json(results / 'measurements.json', data)
    report = aggregate(data)
    report.update(group=data['group'], policy=benchmark_policy(data['group']))
    write_json(results / 'comparison.json', report)
    export_csv(results, data, report)
    pairs = primary_pairs(data['group'])
    lookup = {(c['suite'], c['mode'], c['baseline']): c for c in report['comparisons']}
    temporal = benchmark_policy(data['group'])['stack_temporal']
    labels = {'multisource': 'MultiSource train', 'spec': 'SPEC ref',
              'fleetbench': 'Fleetbench'}
    selected = ' / '.join(labels[suite] for suite in data_suites(data))
    lines = ['### PTSan performance', '',
             f"Group: `{data['group']}`; stack temporal safety: **{'on' if temporal else 'off'}**.",
             f'{selected}; one pinned CPU, serial execution, no runtime validation.',
             f"{data['repetitions']} repetitions; ratios of median runtimes. Geometric means use each pair's common passing set.",
             'Ratios exclude specrand, errors, missing timings, and MultiSource/SPEC repetitions below 10 ms. Fleetbench uses per-iteration CPU time from one-second measurement loops.', '',
             '**' + ('Complete' if report['success'] else 'INCOMPLETE OR FAILED — partial results only') + '**', '',
             '| Configuration | Baseline | ' + ' | '.join(
                 s + ' overhead (cases)' for s in data_suites(data)) + ' |',
             '|---|---|' + '---:|' * len(data_suites(data))]
    headline = [['Configuration', 'Baseline',
                 *[s + ' overhead (cases)' for s in data_suites(data)]]]
    for mode, baseline in pairs:
        values = []
        for suite in data_suites(data):
            c = lookup.get((suite, mode, baseline), {})
            overhead = c.get('overhead_percent')
            values.append(f"{overhead:.2f}% ({c['count']})" if overhead is not None else '— (0)')
        lines.append(f"| {mode} | {baseline} | {' | '.join(values)} |")
        headline.append([mode, baseline, *[value.replace('—', 'N/A') for value in values]])
    widths = [max(len(row[i]) for row in headline) for i in range(len(headline[0]))]
    border = '+' + '+'.join('-' * (width + 2) for width in widths) + '+'
    print(f"\nPTSan performance: {data['group']}; stack temporal safety {'on' if temporal else 'off'}")
    print('Complete' if report['success'] else 'INCOMPLETE OR FAILED - partial results only')
    print(f"{data['repetitions']} repetitions; geometric mean overheads from median runtimes.")
    print(border)
    for index, row in enumerate(headline):
        print('| ' + ' | '.join(value.ljust(width) for value, width in zip(row, widths)) + ' |')
        if index == 0:
            print(border)
    print(border, flush=True)
    lines += ['', '<details>', '<summary>Other baseline comparisons</summary>', '',
              '| Suite | Configuration | Baseline | Cases | Overhead |', '|---|---|---|---:|---:|']
    for c in report['comparisons']:
        if (c['mode'], c['baseline']) in pairs:
            continue
        value = f"{c['overhead_percent']:.2f}%" if c['overhead_percent'] is not None else '—'
        lines.append(f"| {c['suite']} | {c['mode']} | {c['baseline']} | {c['count']} | {value} |")
    lines += ['', '</details>']
    compact = '\n'.join(lines)
    for c in report['comparisons']:
        lines += ['', f"#### {c['suite']}: {c['mode']} / {c['baseline']}", '',
                  '| Benchmark | Seconds | Baseline seconds | Ratio | Overhead |', '|---|---:|---:|---:|---:|']
        lines += [f"| {r['name']} | {r['seconds']:.6g} | {r['baseline_seconds']:.6g} | {r['ratio']:.4f} | {r['overhead_percent']:.2f}% |"
                  for r in c['benchmarks']]
        lines += ['', 'Excluded: ' + ', '.join(c['excluded'])]
        if c['exclusion_reasons']:
            lines.append('')
        for name, reasons in c['exclusion_reasons'].items():
            lines.append('- ' + name + ': ' + '; '.join(
                mode + ': ' + ', '.join(dict.fromkeys(details)) for mode, details in reasons.items()))
    if report['errors']:
        lines += ['', 'Errors:', '', *['- ' + str(e) for e in report['errors']]]
    summary = '\n'.join(lines) + '\n'
    (results / 'summary.md').write_text(summary)
    # Keep the GitHub summary compact; the downloadable artifact has full tables.
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as stream:
            stream.write(compact + '\n\nDownload the reports artifact for `summary.md`, fixed-layout `primary-summary.csv`, '
                         '`summary.csv`, `comparisons.csv`, and `measurements.csv`.\n')
    print(f"Performance report: {results / 'summary.md'}; success={report['success']}", flush=True)
    return not report['success']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', nargs='?', choices=['build', 'run', 'report'])
    parser.add_argument('--suite', choices=('all', *SUITES), default='all',
                        help='suite to build or run')
    parser.add_argument('--group', choices=['software', 'lam', 'id32', 'glibc-stack-off'], default='software')
    parser.add_argument('--build-worker', type=Path)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--cpu', type=int, default=0)
    parser.add_argument('--timeout', type=int, default=14400)
    args = parser.parse_args()
    if args.build_worker:
        return build_worker(args.build_worker)
    if not args.action or not 1 <= args.repetitions <= 10 or args.timeout < 1:
        parser.error('Specify an action, 1–10 repetitions, and a positive workload timeout')
    build = Path(os.environ['BUILD_DIR']).resolve()
    workspace = Path(os.environ['GITHUB_WORKSPACE']).resolve()
    spec_root = Path(os.environ['SPEC_ROOT']).resolve()
    for p in (build, workspace, spec_root):
        if any(c.isspace() for c in str(p)):
            raise ValueError('Paths must not contain whitespace')
    if args.action == 'build':
        selected = suites(args.group) if args.suite == 'all' else (args.suite,)
        return prepare(build, workspace, spec_root, args.group, selected)
    if args.action == 'run':
        if args.suite == 'all':
            parser.error('run requires a specific --suite')
        return measure(build, workspace, args.group, args.repetitions, args.cpu, args.timeout, args.suite)
    return summarize(workspace / 'ci-results/performance')


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    sys.exit(main())
