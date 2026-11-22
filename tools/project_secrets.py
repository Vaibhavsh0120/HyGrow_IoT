"""Manifest-only age encryption. Private identities travel through stdin only."""
import argparse
from datetime import datetime, timezone
import getpass
import json
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parent.parent


def private_permissions(path):
    if os.name == "nt":
        sid = subprocess.run(["whoami", "/user", "/fo", "csv", "/nh"], capture_output=True, text=True, check=True).stdout.strip().split(",")[-1].strip('"')
        grant = f"*{sid}:(OI)(CI)F" if path.is_dir() else f"*{sid}:F"
        subprocess.run(["icacls", str(path), "/inheritance:r", "/grant:r", grant], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    else:
        path.chmod(0o700 if path.is_dir() else 0o600)


def safe_path(root, value):
    if not isinstance(value, str) or not value or "\\" in value:
        raise ValueError("Manifest paths must be nonempty relative POSIX paths")
    path = Path(value)
    if path.is_absolute() or PureWindowsPath(value).drive or any(p in ("..", ".git", ".secrets-backup") or ":" in p for p in path.parts):
        raise ValueError("Unsafe manifest path")
    target = root / path
    # Reject junctions/symlinks even when they happen to point inside the root.
    for parent in (target, *target.parents):
        if parent == root:
            break
        if parent.is_symlink() or (hasattr(parent, "is_junction") and parent.is_junction()):
            raise ValueError("Linked manifest paths are forbidden")
    if not target.resolve().is_relative_to(root.resolve()) or target.resolve() == root.resolve():
        raise ValueError("Manifest path escapes project")
    return target


def load_manifest(root, manifest):
    root = Path(root).resolve()
    data = json.loads(Path(manifest).read_text(encoding="utf-8-sig"))
    if data.get("version") != 1 or not re.fullmatch(r"age1[0-9a-z]{58}", data.get("recipient", "")):
        raise ValueError("Invalid manifest version or age public recipient")
    if not isinstance(data.get("files"), list) or not data["files"]:
        raise ValueError("Manifest requires files")
    seen = set()
    entries = []
    for item in data["files"]:
        plain = safe_path(root, item["plaintext"])
        cipher = safe_path(root, item["ciphertext"])
        if not str(cipher).endswith(".age"):
            raise ValueError("Ciphertext must end in .age")
        for path in (plain, cipher):
            key = str(path).casefold()
            if key in seen:
                raise ValueError("Duplicate manifest destination")
            seen.add(key)
        relative = plain.relative_to(root).as_posix()
        tracked = subprocess.run(["git", "-C", str(root), "ls-files", "--error-unmatch", "--", relative], capture_output=True).returncode == 0
        if tracked:
            raise ValueError("Plaintext destination is tracked by Git")
        ignored = subprocess.run(["git", "-C", str(root), "check-ignore", "-q", "--", relative], capture_output=True).returncode == 0
        if not ignored:
            raise ValueError("Plaintext destination must be ignored by Git")
        if not isinstance(item.get("required", True), bool):
            raise ValueError("required must be boolean")
        entries.append((plain, cipher, item.get("required", True)))
    return data["recipient"], entries


def transform(root, manifest, operation, identity=None, replace=False):
    if operation not in ("encrypt", "decrypt"):
        raise ValueError("Unknown secret operation")
    if operation == "decrypt" and (not identity or not re.search(r"^AGE-SECRET-KEY-1[0-9A-Z]+$", identity, re.MULTILINE)):
        raise ValueError("Invalid age private identity")
    root = Path(root).resolve()
    recipient, entries = load_manifest(root, manifest)
    if not shutil.which("age"):
        raise ValueError("Missing official age executable; install age and retry")
    jobs = []
    for plain, cipher, required in entries:
        source, destination = (plain, cipher) if operation == "encrypt" else (cipher, plain)
        if not source.is_file():
            if required:
                raise ValueError(f"Missing required input: {source.relative_to(root)}")
            continue
        if destination.exists() and operation == "decrypt" and not replace:
            raise ValueError("Refusing to overwrite existing plaintext; use -ReplaceSecrets to keep a backup and replace")
        jobs.append((source, destination))
    # Authenticate every complete file before replacing even the first destination.
    with tempfile.TemporaryDirectory(prefix="hygrow-secret-stage-") as directory:
        stage = Path(directory)
        private_permissions(stage)
        outputs = []
        for index, (source, destination) in enumerate(jobs):
            output = stage / str(index)
            args = ["age", "-r", recipient] if operation == "encrypt" else ["age", "-d", "-i", "-"]
            result = subprocess.run([*args, "-o", str(output), str(source)], input=(identity + "\n").encode() if identity else None, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            if result.returncode or not output.is_file() or not output.stat().st_size:
                raise ValueError("age failed; wrong identity, corrupt input or empty output. Existing files were preserved")
            private_permissions(output)
            outputs.append((output, destination))
        # Backups are ignored and restricted, including encrypted-file replacements.
        backup_root = root / ".secrets-backup"
        if backup_root.is_symlink() or (hasattr(backup_root, "is_junction") and backup_root.is_junction()):
            raise ValueError("Unsafe backup location")
        if any(target.exists() for _, target in outputs):
            if subprocess.run(["git", "-C", str(root), "check-ignore", "-q", "--", ".secrets-backup/probe"], capture_output=True).returncode:
                raise ValueError("Secret backup directory must be ignored")
            backup_root.mkdir(exist_ok=True)
            private_permissions(backup_root)
        backups = {}
        committed = []
        try:
            for output, destination in outputs:
                destination.parent.mkdir(parents=True, exist_ok=True)
                if destination.exists():
                    backup = backup_root / f"{datetime.now(timezone.utc):%Y%m%dT%H%M%S}-{uuid.uuid4().hex}.bak"
                    shutil.copyfile(destination, backup)
                    private_permissions(backup)
                    backups[destination] = backup
                # Stage on the destination volume, then atomically replace.
                pending = destination.with_name(destination.name + ".pending")
                if pending.exists():
                    raise ValueError("Stale pending output exists; inspect before retrying")
                try:
                    with pending.open("xb") as stream:
                        private_permissions(pending)
                        stream.write(output.read_bytes())
                        stream.flush()
                        os.fsync(stream.fileno())
                    os.replace(pending, destination)
                    committed.append(destination)
                finally:
                    pending.unlink(missing_ok=True)
        except Exception:
            for destination in reversed(committed):
                if destination in backups:
                    shutil.copyfile(backups[destination], destination)
                else:
                    destination.unlink(missing_ok=True)
            raise
    return len(jobs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=["encrypt", "decrypt", "check"])
    parser.add_argument("--replace", action="store_true")
    parser.add_argument("--non-interactive", action="store_true")
    args = parser.parse_args()
    manifest = ROOT / "config/secrets.manifest.json"
    try:
        if args.operation == "check":
            load_manifest(ROOT, manifest)
            print("Secret manifest paths, recipient and Git exclusions verified.")
            return
        identity = None
        if args.operation == "decrypt":
            if args.non_interactive:
                raise ValueError("Decryption requires a hidden local terminal prompt; rerun without -NonInteractive")
            identity = getpass.getpass("Paste age private identity (hidden; never stored): ").strip()
        count = transform(ROOT, manifest, args.operation, identity, args.replace)
        print(f"{args.operation.capitalize()} completed for {count} configured files.")
    except (ValueError, OSError, KeyError, subprocess.SubprocessError):
        # Exceptions from external tooling must never include private input.
        print("Secret operation failed. Check manifest, Git ignores, age installation, input/key and overwrite policy. Existing files are preserved or backed up.", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
