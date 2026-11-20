"""Fleetbench CI must distinguish real measurements from successful empty runs."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
import ptsan_fleetbench as fleet
from ptsan_ci_modes import BASE_MODES, ID32_MODES, CONFIGURATIONS


spec = importlib.util.spec_from_file_location('compiler', SCRIPTS / 'ptsan-fleetbench-compiler.py')
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)


class FleetbenchTests(unittest.TestCase):
    def result(self, **kwargs):
        return {'name': 'BM_example', 'iterations': 100, 'cpu_time': 12,
                'time_unit': 'ns', **kwargs}

    def test_workflows_default_to_cpu_3_cache_sizes(self):
        workflows = SCRIPTS.parent / 'workflows'
        sizes = {
            'FLEETBENCH_L1_DATA_SIZE': 49152,
            'FLEETBENCH_L2_SIZE': 3145728,
            'FLEETBENCH_L3_SIZE': 25165824,
        }
        for name in ('ptsan-ci.yml', 'ptsan-performance.yml'):
            text = (workflows / name).read_text()
            for variable, size in sizes.items():
                self.assertIn(
                    f"${{{{ vars.{variable} || '{size}' }}}}", text)

    def test_cache_overrides_in_validation_and_performance(self):
        # Sizes are runner settings, not sanitizer-specific workload changes.
        for performance in (False, True):
            for configured in (False, True):
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    (root / 'fleetbench').mkdir()
                    (root / 'fleetbench/plan.json').write_text(json.dumps({
                        'performance': performance,
                        'builds': {'glibc': {'problem': None, 'bin_dir': str(root)}}}))
                    sizes = {'L1_data_size': 32768, 'L2_size': 262144, 'L3_size': 8388608}
                    env = {'FLEETBENCH_' + flag.upper(): str(size) if configured else ''
                           for flag, size in sizes.items()}
                    def fake_run(command, log, cwd, timeout):
                        for flag, size in sizes.items():
                            self.assertEqual(f'--{flag}={size}' in command, configured)
                        log.write_text('')
                        (log.parent / 'results.json').write_text(json.dumps({
                            'benchmarks': [self.result()]}))
                        return 0
                    with patch.dict(os.environ, env), \
                            patch.object(fleet, 'FAMILIES', {'proto': fleet.FAMILIES['proto']}), \
                            patch.object(fleet, 'run', side_effect=fake_run):
                        _, errors = fleet.run_mode(root, 'glibc', root / 'results',
                                                   performance=performance)
                    self.assertEqual(errors, [])

    def run_family(self, mode, family, status, diagnostic, performance=False, build_problem=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'fleetbench').mkdir()
            (root / 'fleetbench/plan.json').write_text(json.dumps({
                'performance': performance, 'builds': {
                    fleet.build_variant(mode, family): {
                        'problem': build_problem, 'bin_dir': str(root)}}}))
            names = fleet.ID16_EXHAUSTION_CASES.get(family) or [
                f'BM_example_{i}' for i in range(fleet.FAMILIES[family][1])]
            def fake_run(command, log, cwd, timeout):
                log.write_text(diagnostic + '\n')
                # Even if timings exist before exhaustion, exclude the family.
                (log.parent / 'results.json').write_text(json.dumps({
                    'benchmarks': [self.result(name=name) for name in names]}))
                return status
            with patch.object(fleet, 'FAMILIES', {family: fleet.FAMILIES[family]}), \
                    patch.object(fleet, 'run', side_effect=fake_run) as run:
                rows, errors = fleet.run_mode(root, mode, root / 'results', performance=performance)
            if build_problem:
                run.assert_not_called()
            saved = json.loads((root / 'results/checked-results.json').read_text())
            self.assertEqual(saved, {'rows': rows, 'errors': errors})
            return rows, errors

    def test_id_exhaustion_expectations(self):
        for mode in CONFIGURATIONS:
            for family in fleet.FAMILIES:
                for performance in (False, True):
                    with self.subTest(mode=mode, family=family, performance=performance):
                        rows, errors = self.run_family(mode, family, 1, fleet.EXHAUSTION, performance)
                        if mode == 'musl-strict' and family in fleet.ID16_EXHAUSTION_CASES:
                            self.assertEqual(errors, [])
                            self.assertEqual([row['name'] for row in rows], [
                                family + '/' + name for name in fleet.ID16_EXHAUSTION_CASES[family]])
                            self.assertEqual(len(rows), fleet.FAMILIES[family][1])
                            for row in rows:
                                self.assertEqual(row['category'], 'out-of-ids')
                                self.assertTrue(row['matched'])
                                self.assertIsNone(row['seconds'])
                                self.assertEqual(row['diagnostics'], [fleet.EXHAUSTION])
                        else:
                            self.assertEqual(rows, [])
                            self.assertEqual(len(errors), 1)
                            self.assertIn(fleet.EXHAUSTION, errors[0])

    def test_expected_exhaustion_does_not_hide_other_outcomes(self):
        for performance in (False, True):
            for status, diagnostic in (
                    (0, ''), (0, fleet.EXHAUSTION), (1, ''), (2, fleet.EXHAUSTION),
                    (-11, fleet.EXHAUSTION), (124, fleet.EXHAUSTION), (143, fleet.EXHAUSTION),
                    (1, fleet.EXHAUSTION + '\nFound bad LOAD access'),
                    (1, fleet.EXHAUSTION + '\nPTSan ERROR: bad access'),
                    (1, 'PTSan: ID space exhausted in strict mode (unexpected detail)')):
                with self.subTest(performance=performance, status=status, diagnostic=diagnostic):
                    rows, errors = self.run_family('musl-strict', 'proto', status, diagnostic, performance)
                    self.assertEqual(rows, [])
                    self.assertEqual(len(errors), 1)
                    if status == 0 and not diagnostic:
                        self.assertIn('Unexpected pass', errors[0])
        rows, errors = self.run_family('musl-strict', 'proto', 1, fleet.EXHAUSTION,
                                      build_problem='Build failed (1)')
        self.assertEqual(rows, [])
        self.assertIn('Build failed', errors[0])

    def test_inventory(self):
        self.assertEqual(len(fleet.FAMILIES), 8)
        self.assertEqual(fleet.CASE_COUNT, 17)

    def test_build_schedule(self):
        self.assertEqual(fleet.build_schedule(4, cpus=16), (4, 8))
        self.assertEqual(fleet.build_schedule(3, cpus=16), (3, 10))
        self.assertEqual(fleet.build_schedule(
            4, build_parallelism=1, build_jobs=16, cpus=16), (1, 16))
        for arguments in ((0,), (1, 0), (1, None, 0)):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                fleet.build_schedule(*arguments, cpus=16)

    def test_results(self):
        self.assertEqual(fleet.check_cases({'benchmarks': [self.result()]}, 1), [self.result()])
        for cases in ([], [self.result(), self.result()], [self.result(error_occurred=True)],
                      [self.result(iterations=0)], [self.result(cpu_time=float('nan'))],
                      [self.result(cpu_time=-1)]):
            with self.subTest(cases=cases), self.assertRaises(ValueError):
                fleet.check_cases({'benchmarks': cases}, 1)

    def test_host_tools_stay_native(self):
        config = {'build': '/llvm', 'mode': 'musl-strict', 'flags': ['--config=ptsan-musl-strict.cfg']}
        argv = compiler.command(config, ['tool.cc', '-lstdc++', '-o', 'protoc'])
        self.assertEqual(argv, ['/llvm/bin/clang', '--no-default-config',
                                'tool.cc', '-lstdc++', '-o', 'protoc'])

    def test_target_and_native_exceptions_keep_selected_libc(self):
        for mode in ('plain-musl', 'musl-strict', 'glibc'):
            config = {'build': '/llvm', 'mode': mode, 'flags': ['--config=selected.cfg']}
            argv = compiler.command(config, [compiler.TARGET, '-c', 'memcpy.cpp', '-fno-sanitize=ptsan'])
            self.assertIn('--config=selected.cfg', argv)
            self.assertIn('-fno-sanitize=ptsan', argv)
            self.assertNotIn(compiler.TARGET, argv)
            argv = compiler.command(config, [compiler.TARGET, 'file.o', '-lstdc++'])
            self.assertEqual(argv[0], '/llvm/bin/clang++')
            self.assertNotIn('-lstdc++', argv)

    def test_plain_glibc_keeps_system_cxx(self):
        config = {'build': '/llvm', 'mode': 'plain-glibc', 'flags': ['-g']}
        self.assertIn('-lstdc++', compiler.command(config, [compiler.TARGET, 'file.o', '-lstdc++']))

    def test_response_file(self):
        with tempfile.TemporaryDirectory() as directory:
            response = Path(directory) / 'params'
            response.write_text(compiler.TARGET + ' "file with space.cc" -c')
            config = {'build': '/llvm', 'mode': 'glibc', 'flags': ['--config=ptsan-glibc.cfg']}
            self.assertIn('file with space.cc', compiler.command(config, ['@' + str(response)]))

    def test_toolchain_preserves_native_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            template = root / 'template'
            template.mkdir()
            (template / 'BUILD').write_text('''load(":cc.bzl", "cc_toolchain_config")
cc_toolchain_config(name="local", tool_paths={"gcc":"/native/clang", "ar":"/usr/bin/ar"},
    cxx_builtin_include_directories=["/usr/include"], link_flags=["-fuse-ld=lld"])
''')
            with patch.object(fleet.subprocess, 'check_output', return_value='module "crosstool" {}') as generate:
                fleet.configure_toolchain(template, root / 'out', root / 'compiler',
                                          ['/musl/include'], '/generate.sh')
            self.assertEqual(generate.call_args.args[0],
                             ['bash', '/generate.sh', '/usr/include', '/musl/include'])
            build = (root / 'out/BUILD').read_text()
            for text in ('/usr/include', '/musl/include', '/usr/bin/ar', '-fuse-ld=lld', str(root / 'compiler')):
                self.assertIn(text, build)
            self.assertNotIn('/native/clang', build)

    def test_strict_id32_is_markerless_except_cord_and_proto(self):
        strict = CONFIGURATIONS['musl-strict-id32']
        self.assertTrue(strict.strict)
        self.assertEqual(strict.cxx, '--config=ptsan-musl-id32-strict.cfg')
        for mode in BASE_MODES + ID32_MODES + fleet.BASELINES:
            for family in fleet.FAMILIES:
                expected = mode + '-indicator' if mode == 'musl-strict-id32' \
                    and family in ('cord', 'proto') else mode
                self.assertEqual(fleet.build_variant(mode, family), expected)
        for performance in (False, True):
            flags = fleet.compiler_flags('musl-strict-id32', performance)
            self.assertIn(strict.cxx, flags)
            mixed = fleet.compiler_flags('musl-strict-id32', performance, indicator=True)
            self.assertIn('--config=ptsan-musl-id32-strict-indicator.cfg', mixed)
            self.assertNotIn(strict.cxx, mixed)
            self.assertNotIn('-ptsan-may-decode-raw', mixed)
            self.assertEqual('-fsanitize-ptsan-runtime-validate' in mixed, not performance)
            self.assertIn('-ptsan-stack-temporal-safety=1', mixed)
            self.assertNotIn('-ptsan-stack-temporal-safety=0', mixed)

    def test_stack_temporal_policy(self):
        for mode in BASE_MODES + ID32_MODES:
            for performance in (False, True):
                with self.subTest(mode=mode, performance=performance):
                    flags = fleet.compiler_flags(mode, performance)
                    temporal = 1
                    self.assertIn(f'-ptsan-stack-temporal-safety={temporal}', flags)
                    self.assertNotIn(f'-ptsan-stack-temporal-safety={1 - temporal}', flags)

    def test_strict_cord_and_proto_run_the_indicator_binaries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'fleetbench').mkdir()
            (root / 'fleetbench/plan.json').write_text(json.dumps({
                'performance': False, 'builds': {
                    'musl-strict-id32': {'problem': None, 'bin_dir': str(root / 'markerless')},
                    'musl-strict-id32-indicator': {'problem': None, 'bin_dir': str(root / 'indicator')},
                }}))
            def fake_run(command, log, cwd, timeout):
                log.write_text('')
                (log.parent / 'results.json').write_text(json.dumps({'benchmarks': [self.result()]}))
                return 0
            with patch.object(fleet, 'FAMILIES', {name: fleet.FAMILIES[name]
                                               for name in ('cord', 'proto')}), \
                    patch.object(fleet, 'run', side_effect=fake_run) as run:
                rows, errors = fleet.run_mode(root, 'musl-strict-id32', root / 'results')
            self.assertEqual(errors, [])
            self.assertEqual([call.args[0][0] for call in run.call_args_list], [
                root / 'indicator/fleetbench/stl/cord_benchmark',
                root / 'indicator/fleetbench/proto/proto_benchmark'])
            self.assertEqual(len(rows), 2)
            for row in rows:
                self.assertEqual(row['mode'], 'musl-strict-id32')
                self.assertEqual(row['build_variant'], 'musl-strict-id32-indicator')

    def test_diagnostics_fail_even_when_process_returns_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'fleetbench').mkdir()
            (root / 'fleetbench/plan.json').write_text(json.dumps({
                'performance': False, 'builds': {'glibc': {'problem': None, 'bin_dir': str(root)}}}))
            def fake_run(command, log, cwd, timeout):
                log.write_text('PTSan ERROR: bad access\n')
                (log.parent / 'results.json').write_text(json.dumps({'benchmarks': [self.result()]}))
                return 0
            with patch.object(fleet, 'FAMILIES', {'proto': fleet.FAMILIES['proto']}), \
                    patch.object(fleet, 'run', fake_run):
                rows, errors = fleet.run_mode(root, 'glibc', root / 'results')
            self.assertEqual(rows, [])
            self.assertEqual(len(errors), 1)


if __name__ == '__main__':
    unittest.main()
