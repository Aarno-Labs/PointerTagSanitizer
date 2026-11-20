"""MultiSource benchmark expectations."""
import importlib.util
import json
from pathlib import Path
import sys
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
from ptsan_ci_modes import ID32_MODES

spec = importlib.util.spec_from_file_location('checker', SCRIPTS / 'check-ptsan-multisource.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class MultiSourceTests(unittest.TestCase):
    def test_id32_expectations_do_not_inherit_exhaustion(self):
        manifest = json.loads((SCRIPTS.parent / 'expectations/ptsan-multisource.json').read_text())
        for mode in ID32_MODES:
            rules = checker.expectations(manifest, mode)
            self.assertTrue(rules)
            self.assertNotIn('out-of-ids', [r['category'] for r in rules.values()])


if __name__ == '__main__':
    unittest.main()
