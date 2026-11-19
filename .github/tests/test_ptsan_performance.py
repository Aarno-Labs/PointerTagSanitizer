"""Performance configuration and reporting across benchmark suites."""
import csv
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
from ptsan_ci_modes import BASELINES, ID32_MODES

spec = importlib.util.spec_from_file_location('performance', SCRIPTS / 'ptsan-performance.py')
performance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(performance)


class PerformanceTests(unittest.TestCase):
    def test_all_groups_use_the_selected_matrix_group(self):
        workflow = (SCRIPTS.parent / 'workflows/ptsan-performance.yml').read_text()
        self.assertIn('options: [software, lam, id32, glibc-stack-off, all]', workflow)
        self.assertIn('PERF_GROUP: ${{ matrix.group }}', workflow)
        self.assertIn('max-parallel: 1', workflow)
        self.assertIn('fail-fast: false', workflow)
        self.assertIn('["id32", "software", "lam", "glibc-stack-off"]', workflow)
        self.assertIn("inputs.suite == 'fleetbench' && '[\"id32\", \"software\"]'", workflow)
        for name in ('Fetch Fleetbench and pinned Bazel', 'Measure Fleetbench'):
            section = workflow.split(f'- name: {name}', 1)[1]
            section = section.split('\n      - name:', 1)[0]
            self.assertIn("matrix.group == 'software'", section)
            self.assertIn("matrix.group == 'id32'", section)
            self.assertNotIn('inputs.group', section)

    def test_workflow_can_select_benchmark_suite(self):
        workflow = (SCRIPTS.parent / 'workflows/ptsan-performance.yml').read_text()
        self.assertIn('options: [all, multisource, spec, fleetbench]', workflow)
        self.assertIn('--suite "$PERF_SUITE"', workflow)
        steps = {
            'multisource': ('Fetch LLVM test-suite source', 'Measure MultiSource train'),
            'spec': ('Check SPEC installation access', 'Measure SPEC ref'),
            'fleetbench': (
                'Fetch Fleetbench and pinned Bazel',
                'Build Fleetbench configurations',
                'Measure Fleetbench',
            ),
        }
        for suite, names in steps.items():
            for name in names:
                section = workflow.split(f'- name: {name}', 1)[1]
                section = section.split('\n      - name:', 1)[0]
                self.assertIn(f"inputs.suite == '{suite}'", section)

    def test_microbenchmark_times_are_not_dropped_by_whole_program_cutoff(self):
        rows = [{'suite': 'fleetbench', 'name': 'proto/BM_example', 'mode': mode,
                 'repetition': 1, 'category': 'pass', 'matched': True, 'seconds': seconds}
                for mode, seconds in [('plain-glibc', 1e-8), ('glibc', 2e-8)]]
        report = performance.aggregate({'group': 'software', 'modes': ['plain-glibc', 'glibc'],
            'repetitions': 1, 'complete': True, 'expected_records': 2, 'rows': rows, 'errors': []})
        self.assertTrue(report['success'])
        comparison = next(c for c in report['comparisons'] if c['suite'] == 'fleetbench')
        self.assertEqual(comparison['geomean_ratio'], 2)
        self.assertEqual(comparison['count'], 1)

    def test_missing_fleetbench_measurements_fail_software_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'plan.json').write_text(json.dumps({'group': 'software'}))
            for suite in ('multisource', 'spec'):
                (root / f'measurements-{suite}.json').write_text(json.dumps({
                    'schema': 1, 'group': 'software', 'modes': ['glibc'], 'repetitions': 1,
                    'cpu': 3, 'suite': suite, 'expected_records': 0, 'complete': True,
                    'rows': [], 'errors': []}))
            data = performance.collect_measurements(root)
            self.assertFalse(data['complete'])
            self.assertIn('Missing fleetbench measurements', data['errors'])

    def test_focused_report_only_requires_selected_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'plan.json').write_text(json.dumps({
                'group': 'software', 'suites': ['fleetbench']}))
            (root / 'measurements-fleetbench.json').write_text(json.dumps({
                'schema': 1, 'group': 'software', 'modes': ['plain-glibc', 'glibc'],
                'suites': ['fleetbench'], 'repetitions': 1, 'cpu': 3,
                'suite': 'fleetbench', 'expected_records': 0, 'complete': True,
                'rows': [], 'errors': []}))
            data = performance.collect_measurements(root)
            self.assertTrue(data['complete'])
            self.assertEqual(data['suites'], ['fleetbench'])
            report = performance.aggregate(data)
            self.assertTrue(report['success'])
            self.assertTrue(all(c['suite'] == 'fleetbench'
                                for c in report['comparisons']))

    def test_fleetbench_only_plan_has_no_multisource_or_spec_builds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / 'build'
            workspace = root / 'workspace'
            build.mkdir()
            workspace.mkdir()
            manifests = ({'rules': []}, {'configurations': {}}, [], {})
            with patch.object(performance, 'manifests', return_value=manifests), \
                    patch.object(performance, 'run_build_batch',
                                 return_value=None) as run_builds, \
                    patch.object(performance.subprocess, 'check_output',
                                 return_value='revision\n'):
                self.assertFalse(performance.prepare(
                    build, workspace, root / 'spec', 'software',
                    ('fleetbench',)))
            self.assertEqual(run_builds.call_args.args[2], [])
            plan = json.loads(
                (build / 'performance/plan.json').read_text())
            self.assertEqual(plan['suites'], ['fleetbench'])
            self.assertEqual(plan['jobs'], [])

    def test_lam_keeps_two_suites_and_report_columns_match(self):
        self.assertEqual(performance.suites('lam'), ('multisource', 'spec'))
        self.assertEqual(performance.suites('glibc-stack-off'), ('multisource', 'spec'))
        for group, columns in (('software', 9), ('id32', 9), ('lam', 7)):
            data = {'group': group, 'modes': ['plain-glibc'], 'repetitions': 1,
                    'complete': True, 'expected_records': 0, 'rows': [], 'errors': []}
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                performance.export_csv(root, data, performance.aggregate(data))
                with (root / 'primary-summary.csv').open(encoding='utf-8-sig') as stream:
                    rows = list(csv.reader(stream))
                self.assertTrue(all(len(row) == columns for row in rows))

    def test_id32_modes_and_comparisons(self):
        self.assertEqual(performance.modes('id32'), BASELINES + ID32_MODES)
        self.assertEqual(performance.suites('id32'), performance.SUITES)
        self.assertEqual(performance.primary_pairs('id32'), [
            ('glibc-id32', 'plain-glibc'), ('musl-strict-id32', 'plain-musl'),
            ('musl-nonstrict-id32', 'plain-musl')])

    def test_stack_temporal_policy(self):
        for group, temporal, preserve in (
                ('software', 1, False), ('lam', 1, False),
                ('id32', 1, False), ('glibc-stack-off', 0, True)):
            with self.subTest(group=group):
                self.assertEqual(performance.benchmark_policy(group),
                                 {'stack_temporal': temporal, 'preserve_ids': preserve})
                for mode in performance.modes(group):
                    if mode in BASELINES:
                        continue
                    command = performance.multisource_command(
                        Path('/build'), Path('/source'), Path('/output'), mode,
                        **performance.benchmark_policy(group))
                    for language in ('C', 'CXX'):
                        flags, = [arg for arg in command
                                  if arg.startswith(f'-DCMAKE_{language}_FLAGS=')]
                        self.assertIn(f'-ptsan-stack-temporal-safety={temporal}', flags)


if __name__ == '__main__':
    unittest.main()
