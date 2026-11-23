import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).parents[2]


@unittest.skipUnless(shutil.which('docker'), 'Docker Compose required')
class ProductionComposeTests(unittest.TestCase):
    def config(self, values):
        with tempfile.TemporaryDirectory() as directory:
            envfile = Path(directory) / 'config.env'
            envfile.write_text(''.join(f'{key}={value}\n' for key, value in values.items()))
            # Do not let local shell credentials make a missing-value test pass.
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith('HYGROW_') and not key.startswith('SERVICE_URL_')}
            return subprocess.run(['docker', 'compose', '--env-file', str(envfile), '-f',
                                   str(ROOT / 'infra/compose.production.yaml'), 'config', '--format', 'json'],
                                  env=env, capture_output=True, text=True, timeout=30)

    def test_missing_runtime_credentials_fail_before_deployment(self):
        result = self.config({})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('required variable', result.stderr)

    def test_rendered_stack_is_isolated_and_uses_real_production_mail(self):
        values = {key: secrets.token_hex(32) for key in [
            'HYGROW_DATABASE_PASSWORD', 'HYGROW_REDIS_PASSWORD', 'HYGROW_AUTH_ENCRYPTION_KEY',
            'HYGROW_AUTH_OTP_PEPPER', 'HYGROW_BREVO_API_KEY']}
        values.update(HYGROW_API_IMAGE='ghcr.io/vaibhavsh0120/hygrow_iot-api@sha256:' + 'a' * 64,
                      HYGROW_SOURCE_REVISION='b' * 40, HYGROW_MAIL_FROM='noreply@hygrow.test',
                      HYGROW_TRUSTED_PROXIES='10.0.2.7')
        result = self.config(values)
        self.assertEqual(result.returncode, 0, result.stderr)
        stack = json.loads(result.stdout)
        self.assertTrue(stack['networks']['data']['internal'])
        for name, service in stack['services'].items():
            self.assertFalse(service.get('ports'), name + ' must not bind host ports')
            self.assertFalse(service.get('privileged'), name)
        for name in ['api', 'worker']:
            service = stack['services'][name]
            self.assertTrue(service['read_only'])
            self.assertEqual(service['environment']['NODE_ENV'], 'production')
            self.assertEqual(service['environment']['MAIL_MODE'], 'brevo')
            self.assertNotIn('MAILPIT_URL', service['environment'])
            self.assertIn('@sha256:', service['image'])
        for name in ['db', 'redis']:
            self.assertEqual(set(stack['services'][name]['networks']), {'data'})
        self.assertEqual(set(stack['services']['worker']['networks']), {'data', 'outbound'})
        self.assertEqual(set(stack['services']['api']['networks']), {'data', 'proxy'})
