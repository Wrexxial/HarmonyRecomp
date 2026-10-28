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


def exe(name: str, windows: bool) -> str:
    return name + ".exe" if windows else name


def add_tree(archive: zipfile.ZipFile, src: Path, prefix: str) -> int:
    count = 0
    for path in sorted(src.rglob("*")):
        if path.is_file():
            archive.write(path, f"{prefix}/{path.relative_to(src).as_posix()}")
            count += 1
    return count


def tracked_files() -> list[Path]:
    out = subprocess.run(["git", "-c", "safe.directory=*", "ls-files", "-z"], cwd=ROOT,
                         capture_output=True, check=True).stdout
    return [ROOT / name for name in out.decode("utf-8").split("\0") if name]


def make_package(out: Path, sdk: Path, python_embed: Path) -> None:
    if not (sdk / "lib").is_dir():
        sys.exit(f"error: {sdk} does not look like an SDK install folder (no lib/)")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        n = add_tree(archive, sdk, "sdk")
        print(f"  sdk: {n} files")
        files = tracked_files()
        for path in files:
            if path.is_file() and path.suffix.lower() not in (".jpg", ".png"):
                archive.write(path, f"src/{path.relative_to(ROOT).as_posix()}")
        print(f"  src: {len(files)} tracked files (without images)")
        with zipfile.ZipFile(python_embed) as embed:
            for info in embed.infolist():
                archive.writestr(f"python/{info.filename}", embed.read(info))
        print("  python: embeddable distribution")
    print(f"wrote {out} ({out.stat().st_size >> 20} MB)")


def make_toolchain(out: Path, llvm: Path, cmake: Path, ninja: Path, windows: bool) -> None:
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        found = []
        for program in LLVM_PROGRAMS:
            path = llvm / "bin" / exe(program, windows)
            if path.is_file():
                archive.write(path, f"llvm/bin/{path.name}")
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
                archive.write(llvm / name, f"llvm/{name}")
        n = add_tree(archive, cmake, "cmake")
        print(f"  cmake: {n} files")
        archive.write(ninja, f"ninja/{ninja.name}")
    print(f"wrote {out} ({out.stat().st_size >> 20} MB)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True, help="output folder")
    parser.add_argument("--sdk", type=Path, help="SDK install folder")
    parser.add_argument("--python-embed", type=Path, help="python-*-embed-amd64.zip")
    parser.add_argument("--llvm", type=Path, help="unpacked clang+llvm release")
    parser.add_argument("--cmake", type=Path, help="unpacked CMake release (contains bin/)")
    parser.add_argument("--ninja", type=Path, help="ninja executable")
    parser.add_argument("--linux-names", action="store_true",
                        help="look for programs without .exe (for testing the script on Linux)")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    if args.sdk:
        make_package(args.out / "harmonyrecomp-package.zip", args.sdk, args.python_embed)
    if args.llvm:
        make_toolchain(args.out / "harmonyrecomp-toolchain.zip", args.llvm, args.cmake, args.ninja,
                       windows=not args.linux_names)
    return 0


if __name__ == "__main__":
    sys.exit(main())
