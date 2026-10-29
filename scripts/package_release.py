#!/usr/bin/env python3
"""Create the two release downloads the installer uses (run by the release workflow).

  harmonyrecomp-package.zip    sdk/     prebuilt ReXGlue SDK (install folder)
                               src/     this project (tracked files only)
                               python/  embeddable Python for build.py
  harmonyrecomp-toolchain.zip  llvm/    clang, lld-link, llvm-rc, ... and clang's own headers
                               cmake/   CMake
                               ninja/   Ninja

Neither contains anything of the game: the installer builds the game on the
player's PC from their own game file.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# LLVM programs the build uses (CMake probes ar/ranlib/rc/mt on Windows too).
LLVM_PROGRAMS = ("clang", "clang++", "clang-cl", "lld-link", "llvm-rc", "llvm-ar", "llvm-lib",
                 "llvm-ranlib", "llvm-mt", "llvm-nm", "llvm-objcopy")
LLVM_LICENSES = ("LICENSE.TXT",)
# Every entry gets this fixed date. Zip entries store local time without a
# time zone, so the build machine's time (UTC) came out hours in the future
# for players west of UTC; CMake then saw CMakeLists.txt newer than its own
# build files and re-ran endlessly ("build.ninja still dirty after 100 tries").
ZIP_DATE = (2020, 1, 1, 0, 0, 0)
# Microsoft's C++ runtime, shipped next to rexglue.exe and the game (app-local
# deployment as allowed by the Visual Studio redistribution terms), so a PC
# without (or with an old) Visual C++ Redistributable can run them.
VC_RUNTIME = ("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
              "msvcp140_codecvt_ids.dll", "vcruntime140.dll", "vcruntime140_1.dll",
              "concrt140.dll")


def exe(name: str, windows: bool) -> str:
    return name + ".exe" if windows else name


def add_file(archive: zipfile.ZipFile, path: Path, name: str) -> None:
    info = zipfile.ZipInfo(name, date_time=ZIP_DATE)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    archive.writestr(info, path.read_bytes(), compresslevel=6)


def add_tree(archive: zipfile.ZipFile, src: Path, prefix: str) -> int:
    count = 0
    for path in sorted(src.rglob("*")):
        if path.is_file():
            add_file(archive, path, f"{prefix}/{path.relative_to(src).as_posix()}")
            count += 1
    return count


def tracked_files() -> list[Path]:
    out = subprocess.run(["git", "-c", "safe.directory=*", "ls-files", "-z"], cwd=ROOT,
                         capture_output=True, check=True).stdout
    return [ROOT / name for name in out.decode("utf-8").split("\0") if name]


def make_package(out: Path, sdk: Path, python_embed: Path, vc_runtime: Path | None) -> None:
    if not (sdk / "lib").is_dir():
        sys.exit(f"error: {sdk} does not look like an SDK install folder (no lib/)")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        n = add_tree(archive, sdk, "sdk")
        print(f"  sdk: {n} files")
        if vc_runtime:
            found = [name for name in VC_RUNTIME if (vc_runtime / name).is_file()]
            for required in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
                if required not in found:
                    sys.exit(f"error: {required} missing in {vc_runtime}")
            for name in found:
                add_file(archive, vc_runtime / name, f"sdk/bin/{name}")
            print(f"  sdk/bin: C++ runtime {', '.join(found)}")
        files = tracked_files()
        for path in files:
            if path.is_file() and path.suffix.lower() not in (".jpg", ".png"):
                add_file(archive, path, f"src/{path.relative_to(ROOT).as_posix()}")
        print(f"  src: {len(files)} tracked files (without images)")
        with zipfile.ZipFile(python_embed) as embed:
            for info in embed.infolist():
                entry = zipfile.ZipInfo(f"python/{info.filename}", date_time=ZIP_DATE)
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.external_attr = 0o644 << 16
                archive.writestr(entry, embed.read(info), compresslevel=6)
        print("  python: embeddable distribution")
    print(f"wrote {out} ({out.stat().st_size >> 20} MB)")


def make_toolchain(out: Path, llvm: Path, cmake: Path, ninja: Path, windows: bool) -> None:
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        found = []
        for program in LLVM_PROGRAMS:
            path = llvm / "bin" / exe(program, windows)
            if path.is_file():
                add_file(archive, path, f"llvm/bin/{path.name}")
                found.append(program)
        for required in ("clang", "clang++", "lld-link", "llvm-rc"):
            if required not in found:
                sys.exit(f"error: {required} missing in {llvm / 'bin'}")
        print(f"  llvm: {', '.join(found)}")
        # Clang's builtin headers (intrinsics) and runtime libraries.
        clang_lib = llvm / "lib" / "clang"
        n = add_tree(archive, clang_lib, "llvm/lib/clang")
        print(f"  llvm/lib/clang: {n} files")
        for name in LLVM_LICENSES:
            if (llvm / name).is_file():
                add_file(archive, llvm / name, f"llvm/{name}")
        n = add_tree(archive, cmake, "cmake")
        print(f"  cmake: {n} files")
        add_file(archive, ninja, f"ninja/{ninja.name}")
    print(f"wrote {out} ({out.stat().st_size >> 20} MB)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True, help="output folder")
    parser.add_argument("--sdk", type=Path, help="SDK install folder")
    parser.add_argument("--python-embed", type=Path, help="python-*-embed-amd64.zip")
    parser.add_argument("--vc-runtime", type=Path,
                        help="folder with the Visual C++ runtime DLLs (Microsoft.VC143.CRT)")
    parser.add_argument("--llvm", type=Path, help="unpacked clang+llvm release")
    parser.add_argument("--cmake", type=Path, help="unpacked CMake release (contains bin/)")
    parser.add_argument("--ninja", type=Path, help="ninja executable")
    parser.add_argument("--linux-names", action="store_true",
                        help="look for programs without .exe (for testing the script on Linux)")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    if args.sdk:
        make_package(args.out / "harmonyrecomp-package.zip", args.sdk, args.python_embed,
                     args.vc_runtime)
    if args.llvm:
        make_toolchain(args.out / "harmonyrecomp-toolchain.zip", args.llvm, args.cmake, args.ninja,
                       windows=not args.linux_names)
    return 0


if __name__ == "__main__":
    sys.exit(main())
