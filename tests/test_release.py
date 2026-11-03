import hashlib
import subprocess
import sys
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ReleaseTests(unittest.TestCase):
    def test_handler_contract(self):
        subprocess.run(
            [sys.executable, str(ROOT / "tools/verify_build.py"), str(ROOT / "build/altscreen_handler.so")],
            check=True,
        )

    def test_release_is_self_consistent_and_firmware_free(self):
        subprocess.run([sys.executable, str(ROOT / "tools/make_release.py")], check=True)
        archive = ROOT / "dist/moi3-nar-carplay-maps-r8410l-beta.zip"
        with zipfile.ZipFile(archive) as package:
            names = set(package.namelist())
            expected = {
                "ENABLE_ALTSCREEN.txt",
                "fix_hw_partnumber.sh",
                "INSTALL.txt",
                "moi3-nar/payload/altscreen_handler.so",
                "moi3-nar/payload/altscreen_handler.so.sha256",
                "moi3-nar/scripts/install.sh",
                "moi3-nar/scripts/uninstall.sh",
                "moi3-nar/scripts/collect.sh",
            }
            self.assertEqual(names, expected)
            payload = package.read("moi3-nar/payload/altscreen_handler.so")
            checksum = package.read("moi3-nar/payload/altscreen_handler.so.sha256").decode().split()[0]
            self.assertEqual(hashlib.sha256(payload).hexdigest(), checksum)
            self.assertFalse(any(name.endswith((".img", ".mbn", ".dat")) for name in names))


if __name__ == "__main__":
    unittest.main()
