"""Verify actual native artifacts before publishing source-linked downloads."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent.parent
APPLICATION_ID = "dev.vaibhavsh0120.hygrow"


def native_version(root=ROOT):
    version = (root / "VERSION").read_text().strip()
    match = re.search(r"^version: (\d+\.\d+\.\d+)\+(\d+)\s*$", (root / "app/pubspec.yaml").read_text(), re.MULTILINE)
    if not match or match[1] != version:
        raise ValueError("Ecosystem and native versions must match")
    return version, match[2]


def verify_ipa(path, version, build):
    prefix = "Payload/Runner.app/"
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if any(name.startswith(prefix + "_CodeSignature/") or name == prefix + "embedded.mobileprovision" for name in names):
            raise ValueError("Unsigned IPA contains an app signature or provisioning profile")
        info = plistlib.loads(archive.read(prefix + "Info.plist"))
        if (info.get("CFBundleIdentifier"), info.get("CFBundleShortVersionString"), info.get("CFBundleVersion")) != (APPLICATION_ID, version, build):
            raise ValueError("IPA identity/version mismatch")
        if "iPhoneOS" not in info.get("CFBundleSupportedPlatforms", []):
            raise ValueError("IPA is not a device build")
        executable = info.get("CFBundleExecutable")
        if not isinstance(executable, str) or "/" in executable:
            raise ValueError("Invalid IPA executable")
        with archive.open(prefix + executable) as stream:
            header = stream.read(8)
        if len(header) != 8 or struct.unpack("<II", header) != (0xFEEDFACF, 0x0100000C):
            raise ValueError("IPA executable must be a 64-bit ARM device Mach-O")
    return {"applicationId": APPLICATION_ID, "version": version, "build": build, "signing": "unsigned-device-release", "architecture": "arm64"}


def verify_apk(path, version, build, apksigner, aapt, certificate):
    signature = subprocess.run([str(apksigner), "verify", "--print-certs", str(path)], check=True, capture_output=True, text=True).stdout
    match = re.search(r"certificate SHA-256 digest:\s*([0-9a-fA-F:]+)", signature)
    if not match or match[1].replace(":", "").lower() != certificate.lower():
        raise ValueError("APK signer does not match the project's persistent certificate")
    badging = subprocess.run([str(aapt), "dump", "badging", str(path)], check=True, capture_output=True, text=True).stdout
    package = re.search(r"package: name='([^']+)' versionCode='([^']+)' versionName='([^']+)'", badging)
    abi_match = re.search(r"^native-code:\s*'([^']+)'\s*$", badging, re.MULTILINE)
    abi_codes = {"armeabi-v7a": 1, "arm64-v8a": 2, "x86_64": 4}
    abi = abi_match[1] if abi_match else None
    # Flutter gives split APKs distinct codes: ABI_VERSION * 1000 + base build.
    expected_code = str(abi_codes[abi] * 1000 + int(build)) if abi in abi_codes else None
    if not package or package.groups() != (APPLICATION_ID, expected_code, version):
        raise ValueError("APK identity/version mismatch")
    return {"applicationId": APPLICATION_ID, "version": version, "build": build, "versionCode": expected_code, "architecture": abi, "signing": "project-release-key", "certificateSha256": certificate}


def verify_source_stamp(path, asset_path, version, build, revision):
    with zipfile.ZipFile(path) as archive:
        record = json.loads(archive.read(asset_path))
    if record != {"sourceRevision": revision, "version": version, "build": build}:
        raise ValueError("Embedded native source stamp does not match this release")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["android", "ios", "stamp"])
    parser.add_argument("--input", type=Path, nargs="+")
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--apksigner", type=Path)
    parser.add_argument("--aapt", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.revision):
        parser.error("A complete source commit SHA is required")
    version, build = native_version()
    if args.platform == "stamp":
        (ROOT / "app/assets/metadata/build-info.json").write_text(json.dumps({"sourceRevision": args.revision, "version": version, "build": build}) + "\n")
        print("Stamped native build identity for", args.revision)
        return
    if not args.input or not args.output:
        parser.error("Verification requires --input and --output")
    if args.platform == "android" and (not args.apksigner or not args.aapt):
        parser.error("APK verification requires apksigner and aapt")
    certificate = json.loads((ROOT / "config/android-signing.json").read_text())["certificateSha256"]
    artifacts = []
    for path in args.input:
        metadata = verify_ipa(path, version, build) if args.platform == "ios" else verify_apk(path, version, build, args.apksigner, args.aapt, certificate)
        asset_path = "assets/flutter_assets/assets/metadata/build-info.json" if args.platform == "android" else "Payload/Runner.app/Frameworks/App.framework/flutter_assets/assets/metadata/build-info.json"
        verify_source_stamp(path, asset_path, version, build, args.revision)
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        artifacts.append({**metadata, "file": path.name, "sha256": digest, "sizeBytes": path.stat().st_size})
    args.output.write_text(json.dumps({"sourceRevision": args.revision, "artifacts": artifacts}, indent=2) + "\n")
    print(f"Verified {len(artifacts)} {args.platform} artifact(s), version {version}+{build}, source {args.revision}")


if __name__ == "__main__":
    main()
