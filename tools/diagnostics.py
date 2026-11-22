"""Collect safe, reproducible project diagnostics without reading secret values."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import subprocess
import urllib.request
import urllib.error

ROOT = Path(__file__).resolve().parent.parent


def command(args, cwd):
    try:
        result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=15)
        return result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def health(path):
    try:
        with urllib.request.urlopen('http://127.0.0.1:8088' + path, timeout=5) as response:
            body = json.load(response)
            # Only public readiness fields are copied, never raw headers or bodies.
            return {key: body.get(key) for key in ('service', 'status', 'version') if key in body}
    except (OSError, ValueError, urllib.error.URLError):
        return {'status': 'unavailable'}


def collect(root, offline=False):
    root = Path(root).resolve()
    changed = command(['git', 'status', '--porcelain', '--untracked-files=no'], root)
    report = {
        'created_at': datetime.now(timezone.utc).isoformat(),
        'version': (root / 'VERSION').read_text(encoding='utf-8-sig').strip(),
        'git': {
            'commit': command(['git', 'rev-parse', 'HEAD'], root),
            'branch': command(['git', 'branch', '--show-current'], root),
            'tracked_change_count': None if changed is None else len(changed.splitlines()) if changed else 0,
        },
        'tools': {name: bool(shutil.which(name)) for name in ('git', 'python', 'node', 'npm', 'flutter', 'docker', 'age')},
        'local_inputs': {
            'environment_exists': (root / '.env').is_file(),
            'firmware_secrets_exist': (root / 'firmware/secrets.h').is_file(),
        },
        'ci': 'https://github.com/Vaibhavsh0120/HyGrow_IoT/actions',
        'network_probes': 'skipped' if offline else 'local-only',
    }
    if not offline:
        report['api'] = health('/health/ready')
        report['worker'] = health('/health/worker')
        # Deliberately exclude container commands: those can contain credentials.
        report['containers'] = command(['docker', 'compose', '--project-name', 'hygrow-local', '--env-file', str(root / '.env'), '-f', str(root / 'infra/compose.yaml'), 'ps', '--format', '{{.Service}}|{{.State}}|{{.Health}}'], root)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    directory = ROOT / '.dev'
    if directory.is_symlink() or (hasattr(directory, 'is_junction') and directory.is_junction()):
        raise SystemExit('Refusing linked diagnostics output directory')
    directory.mkdir(exist_ok=True)
    report = collect(ROOT, args.offline)
    output = directory / f"diagnostics-{datetime.now(timezone.utc):%Y%m%dT%H%M%S%f}.json"
    with output.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
    print('Safe local diagnostic report: ' + str(output))
    print('Report contains versions, health and service status. It does not read secret-file values.')


if __name__ == '__main__':
    main()
