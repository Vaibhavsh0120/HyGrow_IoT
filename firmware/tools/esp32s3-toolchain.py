"""Keep Windows GCC on the ESP32-S3 binutils when toolchain aliases are missing."""

from pathlib import Path
import shutil
import sys
from tempfile import NamedTemporaryFile

Import("env")

if sys.platform == "win32":
    package = env.PioPlatform().get_package_dir("toolchain-xtensa-esp32s3")
    if not package:
        raise RuntimeError("ESP32-S3 toolchain package is missing. See the PlatformIO troubleshooting section in README.md.")

    # Provide GCC and collect2 with their expected executable names, even
    # when the package is missing those aliases or a host binutils is on PATH.
    tools_dir = Path(env.subst("$BUILD_DIR")) / "xtensa-binutils"
    tools_dir.mkdir(parents=True, exist_ok=True)
    for alias, tool in (("as.exe", "as.exe"), ("ld.exe", "ld.bfd.exe")):
        source = Path(package) / "bin" / f"xtensa-esp32s3-elf-{tool}"
        if not source.is_file():
            raise RuntimeError(f"Missing {source.name}. Reinstall the toolchain as described in README.md.")
        destination = tools_dir / alias
        if destination.is_file():
            current = destination.stat()
            installed = source.stat()
            if (current.st_size, current.st_mtime_ns) == (installed.st_size, installed.st_mtime_ns):
                continue
        with NamedTemporaryFile(dir=tools_dir, delete=False) as temp:
            temporary = Path(temp.name)
        try:
            shutil.copy2(source, temporary)
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)

    env.PrependENVPath("COMPILER_PATH", str(tools_dir))
