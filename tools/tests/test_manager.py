from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).parents[2]


@unittest.skipUnless(shutil.which("powershell") or shutil.which("pwsh"), "PowerShell required")
class ManagerTests(unittest.TestCase):
    def run_manager(self, *args, shell=None):
        shell = shell or ("powershell" if shutil.which("powershell") else "pwsh")
        with tempfile.TemporaryDirectory() as directory:
            return subprocess.run([shell, "-NoProfile", "-File", str(ROOT / "Dev.ps1"), *args], cwd=directory, capture_output=True, text=True, timeout=60)

    def test_help_from_another_directory_on_both_powershell_versions(self):
        for shell in ("powershell", "pwsh"):
            if shutil.which(shell):
                result = self.run_manager("-Help", shell=shell)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("StartLocal", result.stdout)
                self.assertIn("Decrypt", result.stdout)

    def test_invalid_action_fails_without_mutation(self):
        result = self.run_manager("-Action", "DestroyProduction", "-NonInteractive")
        self.assertNotEqual(result.returncode, 0)

    def test_noninteractive_menu_does_not_hang(self):
        result = self.run_manager("-NonInteractive")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Action", result.stderr)

    def test_noninteractive_decrypt_never_prompts(self):
        result = self.run_manager("-Action", "Decrypt", "-NonInteractive")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("failed", result.stderr.lower())

    def test_local_environment_setup_is_idempotent_and_preserves_existing_values(self):
        with tempfile.TemporaryDirectory(prefix="hygrow manager ") as directory:
            env = Path(directory) / ".env"
            env.write_text("LOCAL_DATABASE_PASSWORD=keep-local-value\n")
            module = str(ROOT / "tools/Developer.psm1").replace("'", "''")
            target = directory.replace("'", "''")
            shell = "powershell" if shutil.which("powershell") else "pwsh"
            command = f"Import-Module '{module}' -Force -DisableNameChecking; Initialize-LocalEnvironment '{target}'"
            first = subprocess.run([shell, "-NoProfile", "-Command", command], capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            contents = env.read_bytes()
            self.assertIn(b"LOCAL_DATABASE_PASSWORD=keep-local-value", contents)
            self.assertIn(b"LOCAL_REDIS_PASSWORD=", contents)
            second = subprocess.run([shell, "-NoProfile", "-Command", command], capture_output=True, text=True)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertEqual(env.read_bytes(), contents)


if __name__ == "__main__":
    unittest.main()
