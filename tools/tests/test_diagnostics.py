import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
import json

spec = importlib.util.spec_from_file_location('diagnostics', Path(__file__).parents[1] / 'diagnostics.py')
diagnostics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostics)


class DiagnosticTests(unittest.TestCase):
    def test_offline_report_contains_versions_and_status_without_secret_values(self):
        with tempfile.TemporaryDirectory(prefix='hygrow-diagnostics-') as directory:
            root = Path(directory)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / 'VERSION').write_text('0.2.1')
            (root / '.env').write_text('PASSWORD=never-copy-this-diagnostic-sentinel')
            (root / 'firmware').mkdir()
            (root / 'firmware/secrets.h').write_text('never-copy-firmware-private-material')
            report = diagnostics.collect(root, offline=True)
            text = json.dumps(report)
            self.assertEqual(report['version'], '0.2.1')
            self.assertTrue(report['local_inputs']['environment_exists'])
            self.assertTrue(report['local_inputs']['firmware_secrets_exist'])
            self.assertNotIn('never-copy', text)
            self.assertEqual(report['network_probes'], 'skipped')


if __name__ == '__main__':
    unittest.main()
