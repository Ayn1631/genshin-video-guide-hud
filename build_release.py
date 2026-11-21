"""Build the Windows x64 ZIP from the locally compiled native targets."""

from __future__ import annotations

import argparse
from hashlib import file_digest
from importlib import metadata
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parent
NATIVE = ROOT / "cpptest" / "build-native"
BUNDLE = "GenshinVideoGuideHUD"
BINARIES = ("map_locate.exe", "bgi_locator.dll", "bgi_window_capture.dll", "opencv_world4100.dll")
PACKAGES = ("numpy", "opencv-python-headless", "Pillow", "websockets", "pyinstaller", "pyinstaller-hooks-contrib")


def copy_notices(bundle: Path, opencv_license: Path) -> None:
    dest = bundle / "licenses"
    shutil.copytree(ROOT / "licenses", dest)
    shutil.copy2(opencv_license, dest / "OpenCV-SDK-LICENSE.txt")

    prefix = Path(sys.base_prefix)
    python_license = next((path for path in (prefix / "LICENSE.txt", prefix / "LICENSE_PYTHON.txt")
                           if path.is_file()), None)
    if python_license is None:
        raise FileNotFoundError(f"Python license file missing under {prefix}")
    shutil.copy2(python_license, dest / "Python-LICENSE.txt")

    versions = [f"Python {platform.python_version()}", "OpenCV SDK 4.10.0"]
    for package in PACKAGES:
        dist = metadata.distribution(package)
        versions.append(f"{dist.metadata['Name']} {dist.version}")
        for item in dist.files or ():
            if not item.name.lower().startswith(("license", "copying", "notice")):
                continue
            if item.is_absolute() or ".." in item.parts:
                continue
            original = dist.locate_file(item)
            if original.is_file():
                target = dest / package / item
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(original, target)
    (dest / "VERSIONS.txt").write_text("\n".join(versions) + "\n", encoding="utf-8")


def package(work: Path, version: str, opencv_license: Path) -> Path:
    for name in BINARIES:
        if not (NATIVE / name).is_file():
            raise FileNotFoundError(f"Run cpptest/build-msvc.cmd first. Missing {NATIVE / name}")
    if not opencv_license.is_file():
        raise FileNotFoundError(f"OpenCV SDK license missing at {opencv_license}")

    work = work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=work, prefix="package-") as temp:
        scratch = Path(temp).resolve()
        if not scratch.is_relative_to(work):
            raise RuntimeError(f"Build directory is outside {work}")
        dist = scratch / "dist"
        variables = dict(os.environ, PYINSTALLER_CONFIG_DIR=str(scratch / "pyinstaller-cache"),
                         TMP=str(scratch), TEMP=str(scratch))
        subprocess.run([
            sys.executable, "-B", "-m", "PyInstaller", "--onedir", "--console", "--hide-console", "hide-early",
            "--noupx", "--name", BUNDLE, "--paths", str(ROOT), "--paths", str(ROOT / "cpptest"),
            "--workpath", str(scratch / "build"), "--distpath", str(dist), "--specpath", str(scratch),
            str(ROOT / "cpptest" / "run_hud.py"),
        ], cwd=ROOT, env=variables, check=True)
        bundle = dist / BUNDLE
        native = bundle / "build-native"
        native.mkdir()
        for name in BINARIES:
            shutil.copy2(NATIVE / name, native / name)
        # Local frame capture and image decoding do not use OpenCV's optional FFmpeg video backend.
        for plugin in bundle.rglob("opencv_videoio_ffmpeg*.dll"):
            plugin.unlink()
        for name in ("LICENSE", "README.md", "THIRD_PARTY_NOTICES.md"):
            shutil.copy2(ROOT / name, bundle / name)
        copy_notices(bundle, opencv_license)

        basename = f"{BUNDLE}-{version}-windows-x64"
        staged = Path(shutil.make_archive(str(scratch / basename), "zip", dist, BUNDLE))
        archive = work / staged.name
        staged.replace(archive)

    with archive.open("rb") as file:
        digest = file_digest(file, "sha256").hexdigest()
    (work / "SHA256SUMS.txt").write_text(f"{digest}  {archive.name}\n", encoding="ascii")
    return archive


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default="1.0.0")
    parser.add_argument("--opencv-license", type=Path, default=ROOT / "cpptest/_deps/opencv/build/LICENSE")
    parser.add_argument("--work-dir", type=Path, default=ROOT / ".release-work")
    args = parser.parse_args()
    if sys.platform != "win32" or platform.machine().upper() != "AMD64" or sys.maxsize <= 2**32:
        parser.error("Build with 64-bit Python on Windows x64")
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        parser.error("Use a version such as 1.0.0")
    print(package(args.work_dir, args.version, args.opencv_license.resolve()))


if __name__ == "__main__":
    main()
