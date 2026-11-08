#!/usr/bin/env python3
"""Create the Linux / Steam Deck downloads (run by the release workflow on Linux).

  harmonyrecomp-linux-package.tar.gz    sdk/      prebuilt ReXGlue SDK (linux-amd64 install folder)
                                        src/      this project (tracked files only)
                                        release.txt
  harmonyrecomp-linux-toolchain.tar.xz  toolchain/llvm/      clang, lld, llvm-ar, ... and clang's headers
                                        toolchain/cmake/     CMake
                                        toolchain/ninja/     Ninja
                                        toolchain/sysroot/   glibc, libstdc++ and GTK headers and
                                                             libraries (scripts/make_sysroot.sh)
  harmonyrecomp-install.sh              the install script with the release tag filled in

None of them contains anything of the game: scripts/install_linux.sh builds the
game on the player's machine from their own game file.
"""

from __future__ import annotations

import argparse
import io
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

LLVM_PROGRAMS = ("clang", "clang++", "lld", "llvm-ar", "llvm-ranlib", "llvm-nm",
                 "llvm-objcopy", "llvm-strip")
# LLVM's lld needs libxml2 (and through it ICU), which SteamOS does not have in
# that version ("libxml2.so.2: cannot open shared object file"); they come
# along in lib/lld-deps, and ld.lld is a script that starts lld with them.
LLD_DEPS = ("libxml2.so", "libicuuc.so", "libicudata.so", "liblzma.so")
LD_LLD_WRAPPER = """#!/usr/bin/env bash
# lld with the libraries it needs that the system may not have (libxml2, ICU).
d="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$d/../lib/lld-deps${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec -a "$0" "$d/lld" "$@"
"""


def lld_deps(lld: Path) -> list[tuple[str, Path]]:
    """(soname, file) of lld's libraries in LLD_DEPS, as ldd finds them here."""
    out = subprocess.run(["ldd", str(lld)], capture_output=True, text=True, check=True).stdout
    deps = []
    for line in out.splitlines():
        parts = line.split("=>")
        if len(parts) != 2:
            continue
        soname = parts[0].strip()
        target = parts[1].split("(")[0].strip()
        if soname.startswith(LLD_DEPS) and target:
            deps.append((soname, Path(target).resolve()))
    if not any(name.startswith("libxml2") for name, _ in deps):
        sys.exit(f"error: lld does not link libxml2 here? ldd says:\n{out}")
    return deps
CLANG_RT_UNUSED = ("asan", "ubsan", "tsan", "msan", "lsan", "hwasan", "memprof", "dfsan",
                   "fuzzer", "profile", "xray", "scudo", "orc", "stats", "cfi", "safestack",
                   "gwp_asan", "nsan", "rtsan", "ctx_profile")


def tracked_files() -> list[Path]:
    out = subprocess.run(["git", "-c", "safe.directory=*", "ls-files", "-z"], cwd=ROOT,
                         capture_output=True, check=True).stdout
    return [ROOT / name for name in out.decode("utf-8").split("\0") if name]


def normalized(info: tarfile.TarInfo) -> tarfile.TarInfo:
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    info.mtime = 1700000000
    return info


def make_package(out: Path, sdk: Path, tag: str | None) -> None:
    with tarfile.open(out, "w:gz", compresslevel=6) as archive:
        archive.add(sdk, "sdk", filter=normalized)
        for path in tracked_files():
            if path.is_file() or path.is_symlink():
                archive.add(path, f"src/{path.relative_to(ROOT).as_posix()}", recursive=False,
                            filter=normalized)
        if tag:
            data = (tag + "\n").encode()
            info = normalized(tarfile.TarInfo("release.txt"))
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    print(f"wrote {out} ({out.stat().st_size >> 20} MB)")


def make_toolchain(out: Path, llvm: Path, cmake: Path, ninja: Path, sysroot: Path) -> None:
    def skip_clang_rt(info: tarfile.TarInfo):
        name = info.name.lower()
        if "/include/" not in name and any(part in Path(name).name for part in CLANG_RT_UNUSED):
            return None
        return normalized(info)

    def skip_cmake(info: tarfile.TarInfo):
        parts = Path(info.name).parts
        if len(parts) > 2 and parts[2] in ("doc", "man") or Path(info.name).name == "cmake-gui":
            return None
        return normalized(info)

    with tarfile.open(out, "w:xz", preset=6) as archive:
        for program in LLVM_PROGRAMS:
            path = llvm / "bin" / program
            if not (path.exists() or path.is_symlink()):
                sys.exit(f"error: {program} missing in {llvm / 'bin'}")
            archive.add(path, f"toolchain/llvm/bin/{program}", filter=normalized)
        wrapper = LD_LLD_WRAPPER.encode()
        info = normalized(tarfile.TarInfo("toolchain/llvm/bin/ld.lld"))
        info.size = len(wrapper)
        info.mode = 0o755
        archive.addfile(info, io.BytesIO(wrapper))
        for soname, path in lld_deps(llvm / "bin" / "lld"):
            archive.add(path, f"toolchain/llvm/lib/lld-deps/{soname}", filter=normalized)
            print(f"  lld needs {soname}: {path}")
        # clang and clang++ are links to clang-NN in LLVM's release: add the target.
        for target in sorted((llvm / "bin").glob("clang-[0-9]*")):
            archive.add(target, f"toolchain/llvm/bin/{target.name}", filter=normalized)
        archive.add(llvm / "lib" / "clang", "toolchain/llvm/lib/clang", filter=skip_clang_rt)
        archive.add(cmake, "toolchain/cmake", filter=skip_cmake)
        archive.add(ninja, "toolchain/ninja/ninja", filter=normalized)
        archive.add(sysroot, "toolchain/sysroot", filter=normalized)
    print(f"wrote {out} ({out.stat().st_size >> 20} MB)")


def make_script(out: Path, tag: str | None) -> None:
    text = (ROOT / "scripts" / "install_linux.sh").read_text()
    if tag:
        text = text.replace('TAG="__RELEASE_TAG__"', f'TAG="{tag}"', 1)
    out.write_text(text)
    out.chmod(0o755)
    print(f"wrote {out}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, help="SDK install folder (out/install/linux-amd64)")
    parser.add_argument("--tag", help="release tag")
    parser.add_argument("--llvm", type=Path, help="unpacked LLVM release (contains bin/, lib/clang)")
    parser.add_argument("--cmake", type=Path, help="unpacked CMake release")
    parser.add_argument("--ninja", type=Path, help="ninja executable")
    parser.add_argument("--sysroot", type=Path, help="output of scripts/make_sysroot.sh")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    if args.sdk:
        make_package(args.out / "harmonyrecomp-linux-package.tar.gz", args.sdk, args.tag)
    if args.llvm:
        make_toolchain(args.out / "harmonyrecomp-linux-toolchain.tar.xz", args.llvm, args.cmake,
                       args.ninja, args.sysroot)
    make_script(args.out / "harmonyrecomp-install.sh", args.tag)
    return 0


if __name__ == "__main__":
    sys.exit(main())
