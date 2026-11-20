"""Shared benchmark modes and compiler flags."""
from pathlib import Path
import sys
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
from ptsan_ci_config import benchmark_flags, multisource_command
from ptsan_ci_modes import BASELINES, BASE_MODES, ID32_MODES, MODES, CONFIGURATIONS, selected_modes


class ConfigurationTests(unittest.TestCase):
    def test_workflows_build_all_default_targets(self):
        for workflow, step in (
                ('ptsan-ci.yml', 'Build all default targets'),
                ('ptsan-performance.yml', 'Configure and build all default targets')):
            source = (SCRIPTS.parent / 'workflows' / workflow).read_text()
            section = source.split(f'- name: {step}', 1)[1].split('\n      - name:', 1)[0]
            invocation, = [line.strip() for line in section.splitlines()
                           if line.strip().startswith('ninja ')]
            self.assertEqual(invocation.removesuffix(' \\'),
                             'ninja -C "$BUILD_DIR" -j"$(nproc)"')

    def test_workflows_copy_verified_local_musl_source(self):
        workflows = SCRIPTS.parent / 'workflows'
        revision = '9fa28ece75d8a2191de7c5bb53bed224c5947417'
        for name in ('ptsan-ci.yml', 'ptsan-performance.yml'):
            workflow = (workflows / name).read_text()
            self.assertIn('MUSL_SOURCE: /home/ptsan-ci/sources/musl', workflow)
            self.assertIn(revision, workflow)
            self.assertIn('describe --tags --exact-match', workflow)
            self.assertIn('status --porcelain --untracked-files=all', workflow)
            self.assertIn('cp -a "$MUSL_SOURCE" "$BUILD_DIR/musl-src"', workflow)
            self.assertNotIn('https://git.musl-libc.org', workflow)

    def test_correctness_all_groups_use_the_selected_matrix_group(self):
        workflow = (SCRIPTS.parent / 'workflows/ptsan-ci.yml').read_text()
        self.assertIn('options: [software, lam, id32, all]', workflow)
        self.assertIn('CI_GROUP: ${{ matrix.group }}', workflow)
        self.assertIn('max-parallel: 1', workflow)
        self.assertIn('fail-fast: false', workflow)
        self.assertIn('["software", "lam", "id32"]', workflow)
        self.assertIn("inputs.suite == 'fleetbench' && '[\"software\", \"id32\"]'", workflow)
        self.assertIn('name: ptsan-ci-${{ matrix.group }}-', workflow)
        for name in ('Require working LAM_U48',
                     'Fetch Fleetbench and pinned Bazel',
                     'Run host-libc regressions',
                     'Check selected runtime and C++ dependencies'):
            section = workflow.split(f'- name: {name}', 1)[1]
            section = section.split('\n      - name:', 1)[0]
            self.assertIn('matrix.group', section)
            self.assertNotIn('inputs.group', section)

    def test_correctness_workflow_can_select_benchmark_suite(self):
        workflow = (SCRIPTS.parent / 'workflows/ptsan-ci.yml').read_text()
        self.assertIn('options: [all, multisource, spec, fleetbench]', workflow)
        self.assertIn('default: all', workflow)

        steps = {
            'multisource': (
                'Fetch LLVM test-suite source',
                'Build MultiSource benchmarks',
                'Validate MultiSource with glibc',
                'Validate MultiSource with non-strict musl',
                'Validate MultiSource with strict musl',
            ),
            'spec': (
                'Check SPEC installation access',
                'Build SPEC benchmarks',
                'Validate SPEC test inputs with glibc',
                'Validate SPEC test inputs with non-strict musl',
                'Validate SPEC test inputs with strict musl',
                'Validate SPEC ref inputs',
            ),
            'fleetbench': (
                'Fetch Fleetbench and pinned Bazel',
                'Build Fleetbench',
                'Validate Fleetbench',
            ),
        }
        for suite, names in steps.items():
            for name in names:
                section = workflow.split(f'- name: {name}', 1)[1]
                section = section.split('\n      - name:', 1)[0]
                self.assertIn(f"inputs.suite == '{suite}'", section)

    def test_runtime_validation_is_only_in_correctness_flags(self):
        for mode in BASE_MODES + ID32_MODES:
            flags = benchmark_flags(mode, runtime_validate=True)[1]
            self.assertIn('-fsanitize-ptsan-runtime-validate', flags)
        for mode in BASELINES + BASE_MODES + ID32_MODES:
            flags = benchmark_flags(mode)[1]
            self.assertNotIn('-fsanitize-ptsan-runtime-validate', flags)

    def test_stack_temporal_policy_in_multisource(self):
        for mode in MODES:
            for validate in (False, True):
                with self.subTest(mode=mode, validate=validate):
                    command = multisource_command(Path('/build'), Path('/source'),
                                                  Path('/output'), mode,
                                                  runtime_validate=validate)
                    temporal = 1
                    for language in ('C', 'CXX'):
                        flags, = [arg for arg in command
                                  if arg.startswith(f'-DCMAKE_{language}_FLAGS=')]
                        self.assertIn(f'-ptsan-stack-temporal-safety={temporal}', flags)
                        self.assertNotIn(f'-ptsan-stack-temporal-safety={1 - temporal}', flags)

    def test_workflows_enable_id32_stack_temporal_in_dependencies(self):
        for name in ('ptsan-ci.yml', 'ptsan-performance.yml'):
            workflow = (SCRIPTS.parent / 'workflows' / name).read_text()
            self.assertIn('-DPTSAN_ID32_STACK_TEMPORAL_SAFETY=1', workflow)

    def test_id32_modes_use_matching_configs(self):
        self.assertEqual(selected_modes('id32'), ID32_MODES)
        self.assertEqual(len(selected_modes('lam')), 3)
        for mode in ID32_MODES:
            config = CONFIGURATIONS[mode]
            self.assertTrue(config.id32)
            self.assertIn('id32', config.cxx)
            self.assertNotIn('lam', config.cxx)

    def test_id32_runtime_smokes_expect_full_width_indices(self):
        workflow = (SCRIPTS.parent / 'workflows/ptsan-ci.yml').read_text()
        section = workflow.split(
            '- name: Check selected runtime and C++ dependencies', 1)[1]
        section = section.split('\n      - name:', 1)[0]
        self.assertIn('id_bits=32', section)
        self.assertNotIn('id_bits=31', section)
        self.assertIn('id_bits=32\n              stack_temporal=1', section)
        self.assertIn('id_bits=15\n              stack_temporal=1', section)
        self.assertIn('-ptsan-stack-temporal-safety="$stack_temporal"', section)


if __name__ == '__main__':
    unittest.main()
