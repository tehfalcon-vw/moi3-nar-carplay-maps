#!/usr/bin/env python3
import hashlib
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "altscreen_handler.so"
STAGE = ROOT / "dist" / "moi3-nar-carplay-maps"
ARCHIVE = ROOT / "dist" / "moi3-nar-carplay-maps-r8410l-beta.zip"

if not BUILD.is_file():
    raise SystemExit("build/altscreen_handler.so is missing")
if STAGE.exists():
    shutil.rmtree(STAGE)
(STAGE / "moi3-nar" / "payload").mkdir(parents=True)
(STAGE / "moi3-nar" / "scripts").mkdir(parents=True)

handler = STAGE / "moi3-nar" / "payload" / "altscreen_handler.so"
shutil.copy2(BUILD, handler)
digest = hashlib.sha256(handler.read_bytes()).hexdigest()
(handler.parent / "altscreen_handler.so.sha256").write_text(
    f"{digest}  altscreen_handler.so\n", encoding="ascii", newline="\n"
)
for name in ("install.sh", "uninstall.sh", "collect.sh"):
    shutil.copy2(ROOT / "scripts" / name, STAGE / "moi3-nar" / "scripts" / name)
shutil.copy2(ROOT / "fix_hw_partnumber.sh", STAGE / "fix_hw_partnumber.sh")
(STAGE / "ENABLE_ALTSCREEN.txt").write_text("one-boot trial\n", encoding="ascii")
(STAGE / "INSTALL.txt").write_text(
    "Read the repository README before use. This is an unverified one-boot beta "
    "for MOI3_EU_SE_R8410L / MU0841 / 575919360B only.\n",
    encoding="ascii",
    newline="\n",
)

ARCHIVE.parent.mkdir(exist_ok=True)
with zipfile.ZipFile(ARCHIVE, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for file in sorted(STAGE.rglob("*")):
        if file.is_file():
            archive.write(file, file.relative_to(STAGE))
print(f"created {ARCHIVE} handler_sha256={digest}")
