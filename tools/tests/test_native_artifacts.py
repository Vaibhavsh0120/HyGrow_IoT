import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location("native_artifacts", Path(__file__).parents[1] / "native_artifacts.py")
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)


class NativeArtifactTests(unittest.TestCase):
    def test_embedded_stamp_must_match_exact_source_version_and_build(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'app.apk'
            asset = 'assets/flutter_assets/assets/metadata/build-info.json'
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr(asset, json.dumps({'sourceRevision': 'a' * 40, 'version': '0.3.1', 'build': '4'}))
            native.verify_source_stamp(path, asset, '0.3.1', '4', 'a' * 40)
            with self.assertRaisesRegex(ValueError, 'source stamp'):
                native.verify_source_stamp(path, asset, '0.3.1', '4', 'b' * 40)

    def make_ipa(self, root, platform="iPhoneOS", identifier=native.APPLICATION_ID, signed=False):
        path = root / "test.ipa"
        prefix = "Payload/Runner.app/"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(prefix + "Info.plist", plistlib.dumps({
                "CFBundleIdentifier": identifier, "CFBundleShortVersionString": "0.3.1", "CFBundleVersion": "4",
                "CFBundleSupportedPlatforms": [platform], "CFBundleExecutable": "Runner",
            }))
            archive.writestr(prefix + "Runner", struct.pack("<II", 0xFEEDFACF, 0x0100000C))
            if signed:
                archive.writestr(prefix + "_CodeSignature/CodeResources", b"signature")
        return path

    def test_unsigned_device_package_and_identity_are_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(native.verify_ipa(self.make_ipa(root), "0.3.1", "4")["signing"], "unsigned-device-release")
            for overrides in [{"platform": "iPhoneSimulator"}, {"identifier": "another.app"}, {"signed": True}]:
                with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                    native.verify_ipa(self.make_ipa(root, **overrides), "0.3.1", "4")

    def test_package_version_must_match_requested_release(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "version mismatch"):
                native.verify_ipa(self.make_ipa(Path(directory)), "0.3.2", "5")

    def test_split_apk_has_abi_adjusted_code_and_persistent_signer(self):
        signature = 'Signer #1 certificate SHA-256 digest: ' + 'a' * 64
        badging = "package: name='dev.vaibhavsh0120.hygrow' versionCode='2004' versionName='0.3.1'\nnative-code: 'arm64-v8a'\n"
        with patch.object(native.subprocess, 'run', side_effect=[SimpleNamespace(stdout=signature), SimpleNamespace(stdout=badging)]):
            record = native.verify_apk(Path('arm64.apk'), '0.3.1', '4', 'apksigner', 'aapt', 'a' * 64)
            self.assertEqual(record['versionCode'], '2004')
            self.assertEqual(record['architecture'], 'arm64-v8a')
        # Captured from the pinned Flutter SDK's actual x86_64 release APK.
        x86_badging = badging.replace('2004', '4004').replace('arm64-v8a', 'x86_64')
        with patch.object(native.subprocess, 'run', side_effect=[SimpleNamespace(stdout=signature), SimpleNamespace(stdout=x86_badging)]):
            self.assertEqual(native.verify_apk(Path('x86.apk'), '0.3.1', '4', 'apksigner', 'aapt', 'a' * 64)['versionCode'], '4004')
        for bad in [badging.replace('2004', '4'), badging.replace('dev.vaibhavsh0120.hygrow', 'other.app')]:
            with patch.object(native.subprocess, 'run', side_effect=[SimpleNamespace(stdout=signature), SimpleNamespace(stdout=bad)]), self.assertRaises(ValueError):
                native.verify_apk(Path('arm64.apk'), '0.3.1', '4', 'apksigner', 'aapt', 'a' * 64)
        with patch.object(native.subprocess, 'run', return_value=SimpleNamespace(stdout=signature)), self.assertRaisesRegex(ValueError, 'signer'):
            native.verify_apk(Path('arm64.apk'), '0.3.1', '4', 'apksigner', 'aapt', 'b' * 64)
