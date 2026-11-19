"""SPEC configuration, expectations, and runner behavior."""
import errno
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
from ptsan_ci_config import spec_arguments, spec_benchmarks
from ptsan_ci_modes import BASELINES, BASE_MODES, ID32_MODES, MODES, selected_modes

spec = importlib.util.spec_from_file_location('ptsan_spec', SCRIPTS / 'ptsan-spec.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class SpecTests(unittest.TestCase):
    def test_pooled_ref_order_and_reports(self):
        # Keep scheduling separate from report order, including an unlisted
        # workload. Missing/duplicate results must never yield a passing phase.
        for group in ('software', 'lam', 'id32'):
            for outcome in ('pass', 'mismatch', 'missing', 'duplicate', 'cancelled'):
                with self.subTest(group=group, outcome=outcome), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    modes = selected_modes(group)
                    names = ['999.specrand_ir', '526.blender_r', '520.omnetpp_r']
                    manifest = {'benchmarks': names, 'configurations': {mode: {} for mode in modes},
                                'benchmark_configurations': {'526.blender_r': [modes[0]]}}
                    jobs = []
                    for mode in modes:
                        for name in spec_benchmarks(manifest, mode):
                            output = root / 'build' / mode / name
                            output.mkdir(parents=True)
                            config = output / 'ptsan.cfg'
                            config.write_text('built config\n')
                            jobs.append({'mode': mode, 'name': name, 'output': str(output),
                                         'results': str(output), 'config': str(config),
                                         'arguments': [f'--config={config}', '--size=test',
                                                       '--action=build', '--rebuild', name]})

                    def batch(queue, slots, results, completed):
                        self.assertEqual(slots, 2)
                        expected = [(mode, name) for name in (names[2], names[0], names[1])
                                    for mode in sorted(modes)
                                    if name in spec_benchmarks(manifest, mode)]
                        self.assertEqual([(j['mode'], j['name']) for j in queue], expected)
                        for index, job in enumerate(reversed(queue)):
                            self.assertEqual(Path(job['results']), results / job['mode'] / job['name'])
                            self.assertIn('--size=ref', job['arguments'])
                            self.assertIn('--nobuild', job['arguments'])
                            self.assertNotIn('--rebuild', job['arguments'])
                            self.assertEqual(job['timeout'], 30)
                            if outcome == 'missing' and index == 0:
                                continue
                            mismatch = outcome == 'mismatch' and index == 1
                            row = {'mode': job['mode'], 'name': job['name'], 'category': 'pass',
                                   'matched': not mismatch, 'detail': 'synthetic mismatch' if mismatch else None}
                            completed(row)
                            if outcome == 'duplicate':
                                completed(row)
                            if outcome == 'cancelled':
                                raise SystemExit(143)

                    results = root / 'ref'
                    with patch.object(runner, 'run_validation_batch', side_effect=batch) as pool, \
                            patch('builtins.print'), patch.dict(os.environ, {'GITHUB_STEP_SUMMARY': ''}):
                        if outcome in ('duplicate', 'cancelled'):
                            with self.assertRaises(ValueError if outcome == 'duplicate' else SystemExit):
                                runner.validate_ref(results, modes, jobs, manifest, None, 2, 30)
                        else:
                            failed = runner.validate_ref(results, modes, jobs, manifest, None, 2, 30)
                            self.assertEqual(failed, outcome != 'pass')
                        pool.assert_called_once()
                    reports = [json.loads((results / mode / 'checked-results.json').read_text()) for mode in modes]
                    self.assertEqual(all(report['success'] for report in reports), outcome == 'pass')
                    for mode, report in zip(modes, reports):
                        self.assertEqual(report['mode'], mode)
                        self.assertEqual(report['size'], 'ref')
                        summary = (results / mode / 'summary.md').read_text()
                        temporal = 'on'
                        self.assertIn(f'stack temporal safety {temporal}', summary)
                        if outcome in ('pass', 'mismatch'):
                            self.assertEqual([r['name'] for r in report['tests']], spec_benchmarks(manifest, mode))
                        elif outcome in ('duplicate', 'cancelled'):
                            self.assertFalse(report['complete'])

    def test_stack_temporal_arguments(self):
        for mode in MODES:
            for size in ('test', 'ref'):
                with self.subTest(mode=mode, size=size):
                    args = spec_arguments(Path('/build'), Path('/source'), mode,
                                          Path('/config'), Path('/output'), size=size)
                    id32 = int(mode in ID32_MODES)
                    self.assertIn(f'id32={id32}', args)
                    self.assertIn('stack_temporal=1', args)

    def test_id32_expectations_exclude_blender_not_exhaustion(self):
        root = SCRIPTS.parent / 'expectations'
        test = json.loads((root / 'ptsan-spec.json').read_text())
        ref = json.loads((root / 'ptsan-spec-ref.json').read_text())
        self.assertEqual(test['benchmark_configurations'], ref['benchmark_configurations'])
        for manifest in (test, ref):
            self.assertEqual(set(manifest['configurations']), set(MODES))
            for mode in ID32_MODES:
                names = spec_benchmarks(manifest, mode)
                self.assertNotIn('526.blender_r', names)
                self.assertEqual('507.cactuBSSN_r' in names, mode == 'glibc-id32')
                rules = manifest['configurations'][mode]
                self.assertNotIn('out-of-ids', [r['category'] for r in rules.values()])
            for mode in BASE_MODES + BASELINES:
                self.assertIn('526.blender_r', spec_benchmarks(manifest, mode))

    def test_startup_directory_survives_empty_directory_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            process = Mock()
            process.wait.return_value = 0

            def start(*args, **kwargs):
                temporary = root / 'tmp'
                self.assertTrue((temporary / '.ptsan-ci-keep').is_file())
                # Model another runcpu's cleanup between startup's directory
                # check and File::Temp creating its flags file.
                with self.assertRaises(OSError) as failure:
                    temporary.rmdir()
                self.assertEqual(failure.exception.errno, errno.ENOTEMPTY)
                with tempfile.NamedTemporaryFile(prefix='flags_', dir=temporary):
                    pass
                return process

            with patch.object(runner.subprocess, 'Popen', side_effect=start), \
                    patch.object(runner, 'stop_group'):
                for attempt in range(2):
                    self.assertEqual(runner.invoke(root, ['--action=build'],
                                     root / f'build-{attempt}.log', 60), 0)
            self.assertTrue((root / 'tmp/.ptsan-ci-keep').is_file())


if __name__ == '__main__':
    unittest.main()
