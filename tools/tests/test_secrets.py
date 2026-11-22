"""Exercise real age and Git in disposable directories, never project secrets."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("project_secrets", Path(__file__).parents[1] / "project_secrets.py")
secrets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(secrets)


class SecretTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run(["age-keygen"], capture_output=True, text=True, check=True)
        cls.identity = next(line for line in result.stdout.splitlines() if line.startswith("AGE-SECRET-KEY-"))
        cls.recipient = subprocess.run(["age-keygen", "-y"], input=cls.identity, capture_output=True, text=True, check=True).stdout.strip()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hygrow-secrets-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        (self.root / ".gitignore").write_text(".env\n.secrets-backup/\n")
        self.plain = self.root / ".env"
        self.cipher = self.root / ".env.age"
        self.plain.write_bytes(b"TEST_VALUE=not-a-real-secret\n")
        self.manifest = self.root / "manifest.json"
        self.entries = [{"plaintext": ".env", "ciphertext": ".env.age", "required": True}]
        self.write_manifest()

    def write_manifest(self):
        self.manifest.write_text(json.dumps({"version": 1, "recipient": self.recipient, "files": self.entries}))

    def test_roundtrip_and_private_material_not_written(self):
        secrets.transform(self.root, self.manifest, "encrypt")
        self.plain.unlink()
        secrets.transform(self.root, self.manifest, "decrypt", identity=self.identity)
        self.assertEqual(self.plain.read_bytes(), b"TEST_VALUE=not-a-real-secret\n")
        for path in self.root.rglob("*"):
            if path.is_file():
                self.assertNotIn(self.identity.encode(), path.read_bytes())

    def test_existing_plaintext_requires_explicit_replace_and_keeps_backup(self):
        secrets.transform(self.root, self.manifest, "encrypt")
        self.plain.write_text("keep-this-working-config")
        with self.assertRaisesRegex(ValueError, "overwrite"):
            secrets.transform(self.root, self.manifest, "decrypt", identity=self.identity)
        self.assertEqual(self.plain.read_text(), "keep-this-working-config")
        secrets.transform(self.root, self.manifest, "decrypt", identity=self.identity, replace=True)
        backups = list((self.root / ".secrets-backup").rglob("*.bak"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_text(), "keep-this-working-config")

    def test_wrong_key_and_corruption_leave_working_files_untouched(self):
        secrets.transform(self.root, self.manifest, "encrypt")
        another = subprocess.run(["age-keygen"], capture_output=True, text=True, check=True).stdout
        before = self.plain.read_bytes()
        with self.assertRaisesRegex(ValueError, "age failed"):
            secrets.transform(self.root, self.manifest, "decrypt", identity=another, replace=True)
        self.assertEqual(self.plain.read_bytes(), before)
        self.cipher.write_bytes(b"corrupt ciphertext")
        with self.assertRaisesRegex(ValueError, "age failed"):
            secrets.transform(self.root, self.manifest, "decrypt", identity=self.identity, replace=True)
        self.assertEqual(self.plain.read_bytes(), before)

    def test_path_traversal_absolute_and_git_paths_are_rejected(self):
        for path in ("../outside", str(self.root / ".env"), ".git/config", "config/../../outside"):
            with self.subTest(path=path):
                self.entries[0]["plaintext"] = path
                self.write_manifest()
                with self.assertRaises(ValueError):
                    secrets.load_manifest(self.root, self.manifest)

    def test_unignored_or_tracked_plaintext_is_rejected(self):
        (self.root / ".gitignore").write_text("")
        with self.assertRaisesRegex(ValueError, "ignored"):
            secrets.load_manifest(self.root, self.manifest)
        subprocess.run(["git", "-C", str(self.root), "add", ".env"], check=True)
        (self.root / ".gitignore").write_text(".env\n")
        with self.assertRaisesRegex(ValueError, "tracked"):
            secrets.load_manifest(self.root, self.manifest)

    def test_duplicate_and_overlapping_destinations_rejected(self):
        self.entries.append(dict(self.entries[0]))
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            secrets.load_manifest(self.root, self.manifest)

    def test_no_partial_update_when_second_file_is_corrupt(self):
        self.entries.append({"plaintext": "nested/.env", "ciphertext": "nested/.env.age", "required": True})
        (self.root / "nested").mkdir()
        (self.root / "nested/.env").write_text("second")
        self.write_manifest()
        secrets.transform(self.root, self.manifest, "encrypt")
        self.plain.write_text("first-working")
        (self.root / "nested/.env.age").write_bytes(b"corrupt")
        with self.assertRaises(ValueError):
            secrets.transform(self.root, self.manifest, "decrypt", identity=self.identity, replace=True)
        self.assertEqual(self.plain.read_text(), "first-working")

    def test_missing_required_file_and_invalid_identity_fail(self):
        self.plain.unlink()
        with self.assertRaisesRegex(ValueError, "Missing"):
            secrets.transform(self.root, self.manifest, "encrypt")
        with self.assertRaisesRegex(ValueError, "identity"):
            secrets.transform(self.root, self.manifest, "decrypt", identity="invalid")


if __name__ == "__main__":
    unittest.main()
