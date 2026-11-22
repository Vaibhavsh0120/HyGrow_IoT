"""Run all hardware-independent firmware regressions; stop on any failure."""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    for tool in ("g++", "node"):
        if not shutil.which(tool):
            raise SystemExit(f"Missing {tool}; install a C++17 compiler and Node.js.")
    for test in sorted((ROOT / "tools").glob("test-*.py")):
        if test.name == "test-all.py":
            continue
        print(f"\nRunning {test.name}", flush=True)
        subprocess.run([sys.executable, str(test)], cwd=ROOT, check=True)
    subprocess.run(["node", "tools/test-telemetry-view.js"], cwd=ROOT, check=True)
    print("\nAll hardware-independent firmware regressions passed.")


if __name__ == "__main__":
    main()
