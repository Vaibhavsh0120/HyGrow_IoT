#!/usr/bin/env python3
"""Regenerate the LittleFS dashboard's precompressed web assets."""

import gzip
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1] / "data"
ASSETS = (
    "index.html",
    "css/style.css",
    "js/app.js",
    "js/charts.js",
    "fonts/symbols.woff2",
)


def main() -> None:
    for relative in ASSETS:
        source = ROOT / relative
        target = source.with_name(source.name + ".gz")
        content = source.read_bytes()
        target.write_bytes(gzip.compress(content, compresslevel=9, mtime=0))
        if gzip.decompress(target.read_bytes()) != content:
            raise RuntimeError(f"Compressed asset does not match {relative}")
        print(f"{relative} -> {target.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
