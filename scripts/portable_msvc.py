#!/usr/bin/env python3
"""Fetch the parts of the Microsoft C++ toolchain that Clang needs on Windows.

Clang compiles for the MSVC ABI and uses Microsoft's C/C++ runtime headers and
libraries plus the Windows SDK. They are normally installed with Visual Studio
or the Build Tools. Users of the installer usually have neither, and the files
may not be redistributed, so this script downloads them directly from
Microsoft's Visual Studio servers (the same packages the Build Tools installer
uses), after the user accepted Microsoft's license, and unpacks them into a
folder. No administrator rights are needed.

  portable_msvc.py --detect            exit 0 if Visual Studio C++ tools and a
                                       Windows SDK are installed already
  portable_msvc.py --license           print the license URL
  portable_msvc.py --accept-license OUT
                                       download into OUT and write OUT/env.json
                                       with the INCLUDE and LIB search paths

Only what Clang uses is downloaded: C++ standard library and CRT headers, the
x64 runtime libraries and the Windows SDK headers and x64 import libraries.
Download size is roughly 300-500 MB. The approach (manifests, MSI extraction
with msiexec /a) follows the well-known portable-msvc script by Martins
Mozeiko (public domain).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

CHANNEL_URL = "https://aka.ms/vs/17/release/channel"
MANIFEST_ITEM = "Microsoft.VisualStudio.Manifests.VisualStudio"
BUILD_TOOLS_ITEM = "Microsoft.VisualStudio.Product.BuildTools"

# Windows SDK installers with the headers and x64 import libraries.
SDK_MSI_PATTERNS = (
    "Windows SDK Desktop Headers x86-x86_en-us.msi",
    "Windows SDK Desktop Headers x64-x86_en-us.msi",
    "Windows SDK for Windows Store Apps Headers-x86_en-us.msi",
    "Windows SDK for Windows Store Apps Headers OnecoreUap-x86_en-us.msi",
    "Windows SDK Desktop Libs x64-x86_en-us.msi",
    "Windows SDK for Windows Store Apps Libs-x86_en-us.msi",
    "Universal CRT Headers Libraries and Sources-x86_en-us.msi",
)


def log(message: str) -> None:
    print(message, flush=True)


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()


def download(url: str, target: Path, sha256: str | None, size: int | None, label: str) -> None:
    if target.exists() and (size is None or target.stat().st_size == size):
        if not sha256 or file_sha256(target) == sha256.lower():
            return
    target.parent.mkdir(parents=True, exist_ok=True)
    part = target.with_suffix(target.suffix + ".part")
    digest = hashlib.sha256()
    done = 0
    with urllib.request.urlopen(url, timeout=300) as response, part.open("wb") as out:
        while True:
            chunk = response.read(1 << 20)
            if not chunk:
                break
            out.write(chunk)
            digest.update(chunk)
            done += len(chunk)
            if size:
                print(f"##download {label} {done} {size}", flush=True)
    if sha256 and digest.hexdigest() != sha256.lower():
        part.unlink(missing_ok=True)
        raise RuntimeError(f"checksum mismatch for {target.name}")
    part.replace(target)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifests() -> tuple[dict, dict]:
    channel = json.loads(fetch(CHANNEL_URL))
    item = next(i for i in channel["channelItems"] if i["id"] == MANIFEST_ITEM)
    vsman = json.loads(fetch(item["payloads"][0]["url"]))
    return channel, vsman


def license_url(channel: dict) -> str:
    tools = next(i for i in channel["channelItems"] if i["id"] == BUILD_TOOLS_ITEM)
    resources = tools.get("localizedResources", [])
    english = next((r for r in resources if r.get("language", "").lower() == "en-us"), None)
    return (english or resources[0])["license"]


def index_packages(vsman: dict) -> dict[str, list[dict]]:
    packages: dict[str, list[dict]] = {}
    for package in vsman["packages"]:
        packages.setdefault(package["id"].lower(), []).append(package)
    return packages


def first_package(packages: dict[str, list[dict]], package_id: str) -> dict:
    candidates = packages[package_id.lower()]
    # Prefer neutral or English language variants.
    for package in candidates:
        if package.get("language") in (None, "neutral", "en-US"):
            return package
    return candidates[0]


def newest_msvc(packages: dict[str, list[dict]]) -> str:
    versions = []
    for package_id in packages:
        m = re.fullmatch(r"microsoft\.vc\.([0-9.]+)\.tools\.hostx64\.targetx64\.base", package_id)
        if m:
            versions.append(m.group(1))
    if not versions:
        raise RuntimeError("no MSVC version found in the Visual Studio manifest")
    return max(versions, key=lambda v: tuple(int(x) for x in v.split(".")))


def newest_sdk(packages: dict[str, list[dict]]) -> str:
    candidates = []
    for package_id in packages:
        m = re.fullmatch(r"microsoft\.visualstudio\.component\.windows1[01]sdk\.(\d+)", package_id)
        if m:
            candidates.append((int(m.group(1)), package_id))
    if not candidates:
        raise RuntimeError("no Windows SDK found in the Visual Studio manifest")
    return max(candidates)[1]


def extract_vsix(vsix: Path, out_dir: Path) -> None:
    """VSIX packages are zips; the payload lives under Contents/."""
    with zipfile.ZipFile(vsix) as archive:
        for name in archive.namelist():
            if not name.startswith("Contents/") or name.endswith("/"):
                continue
            target = out_dir / name[len("Contents/"):]
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(name) as src, target.open("wb") as dst:
                shutil.copyfileobj(src, dst)


def msi_cabs(msi: Path) -> list[str]:
    data = msi.read_bytes()
    cabs = []
    index = 0
    while True:
        index = data.find(b".cab", index + 4)
        if index < 0:
            return cabs
        name = data[index - 32:index + 4]
        if re.fullmatch(rb"[0-9a-fA-F]{32}\.cab", name):
            cabs.append(name.decode("ascii"))


def install(out_dir: Path) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    downloads = out_dir / "downloads"
    _, vsman = load_manifests()
    packages = index_packages(vsman)

    msvc_version = newest_msvc(packages)
    log(f"MSVC packages {msvc_version}")
    msvc_ids = [
        f"microsoft.vc.{msvc_version}.crt.headers.base",
        f"microsoft.vc.{msvc_version}.crt.x64.desktop.base",
        f"microsoft.vc.{msvc_version}.crt.x64.store.base",
        f"microsoft.vc.{msvc_version}.tools.hostx64.targetx64.base",
    ]
    for package_id in msvc_ids:
        if package_id not in packages:
            continue
        package = first_package(packages, package_id)
        for payload in package.get("payloads", []):
            name = payload["fileName"].replace("\\", "/").split("/")[-1]
            target = downloads / "msvc" / name
            download(payload["url"], target, payload.get("sha256"), payload.get("size"), name)
            extract_vsix(target, out_dir)

    sdk_component = newest_sdk(packages)
    sdk_package = first_package(packages, sdk_component)
    # The component depends on the actual installer package (Win11SDK_10.0.x).
    sdk_installer_id = next((d for d in sdk_package.get("dependencies", {})
                             if re.match(r"win1[01]sdk_", d.lower())), None)
    if sdk_installer_id is None:
        raise RuntimeError(f"{sdk_component} has no Windows SDK installer package")
    sdk_installer = first_package(packages, sdk_installer_id)
    log(f"Windows SDK {sdk_installer_id}")
    payloads = {p["fileName"].replace("\\", "/").split("/")[-1]: p
                for p in sdk_installer.get("payloads", [])}
    sdk_dir = downloads / "sdk"
    msis = []
    # Headers are spread over more installers than the fixed list (lmcons.h, for
    # example, is not in the desktop headers): take every x86/x64 header MSI.
    wanted = list(SDK_MSI_PATTERNS)
    for name in sorted(payloads):
        lower = name.lower()
        if (lower.endswith(".msi") and "headers" in lower and "arm" not in lower
                and name not in wanted):
            wanted.append(name)
    log("SDK installers: " + ", ".join(n for n in wanted if n in payloads))
    for name in wanted:
        if name in payloads:
            p = payloads[name]
            download(p["url"], sdk_dir / name, p.get("sha256"), p.get("size"), name)
            msis.append(sdk_dir / name)
    if not msis:
        raise RuntimeError("Windows SDK installers not found in the manifest")
    for msi in msis:
        for cab in msi_cabs(msi):
            if cab in payloads:
                p = payloads[cab]
                download(p["url"], sdk_dir / cab, p.get("sha256"), p.get("size"), cab)
    for msi in msis:
        log(f"unpacking {msi.name}")
        # Administrative install: only unpacks files, needs no admin rights.
        # msiexec parses its own command line: quote paths (they may contain spaces).
        command = f'msiexec.exe /a "{msi}" /quiet /qn TARGETDIR="{out_dir}"'
        for attempt in range(3):
            result = subprocess.run(command, check=False)
            if result.returncode != 1618:  # another installation is running
                break
            time.sleep(10)
        if result.returncode != 0:
            raise RuntimeError(f"msiexec failed for {msi.name} (exit code {result.returncode})")

    env = environment(out_dir)
    (out_dir / "env.json").write_text(json.dumps(env, indent=2), encoding="utf-8")
    shutil.rmtree(downloads, ignore_errors=True)
    for leftover in out_dir.glob("*.msi"):
        leftover.unlink()
    return env


def environment(root: Path) -> dict:
    msvc = sorted((root / "VC" / "Tools" / "MSVC").glob("*"))[-1]
    kits = root / "Windows Kits" / "10"
    sdk_version = sorted((kits / "Include").glob("10.*"))[-1].name
    include = [msvc / "include"] + [kits / "Include" / sdk_version / part
                                    for part in ("ucrt", "shared", "um", "winrt", "cppwinrt")]
    lib = [msvc / "lib" / "x64"] + [kits / "Lib" / sdk_version / part / "x64"
                                    for part in ("ucrt", "um")]
    return {
        "INCLUDE": ";".join(str(p) for p in include if p.is_dir()),
        "LIB": ";".join(str(p) for p in lib if p.is_dir()),
        "msvc_version": msvc.name,
        "sdk_version": sdk_version,
    }


def detect() -> bool:
    """True if Visual Studio with the C++ x64 tools and a Windows 10/11 SDK exist."""
    if os.name != "nt":
        return False
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return False
    # VS 2022 or newer: the prebuilt SDK is compiled against a current STL.
    result = subprocess.run([str(vswhere), "-latest", "-products", "*", "-version", "[17.0,",
                             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                             "-property", "installationPath"],
                            capture_output=True, text=True, check=False)
    if not result.stdout.strip():
        return False
    import winreg  # noqa: PLC0415 (Windows only)
    try:
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                             r"SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0")
        folder, _ = winreg.QueryValueEx(key, "InstallationFolder")
    except OSError:
        return False
    return (Path(folder) / "Include").is_dir()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--detect", action="store_true")
    parser.add_argument("--license", action="store_true")
    parser.add_argument("--accept-license", metavar="OUT", type=Path)
    args = parser.parse_args()
    if args.detect:
        found = detect()
        log("Visual Studio C++ tools found" if found else "Visual Studio C++ tools not found")
        return 0 if found else 1
    if args.license:
        channel, _ = load_manifests()
        print(license_url(channel))
        return 0
    if args.accept_license:
        env = install(args.accept_license.resolve())
        log(f"MSVC {env['msvc_version']}, Windows SDK {env['sdk_version']}")
        return 0
    parser.print_help()
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as err:  # noqa: BLE001  (the installer shows the message)
        print(f"error: {err}", file=sys.stderr, flush=True)
        sys.exit(1)
