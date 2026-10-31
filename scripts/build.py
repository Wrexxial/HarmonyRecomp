#!/usr/bin/env python3
"""Build HarmonyRecomp from scratch.

Runs every step in order and skips the ones that are already done:

  1. Extract the XBLA package in game/ into assets/   (needs assets/default.xex),
     any DLC packages in dlc/ into assets/dlc/, and a title update in dlc/
     (if present) over assets/
  2. Clone the ReXGlue SDK at the pinned commit      (../rexglue-sdk, next to the project)
  3. Build and install the SDK, including the rexglue codegen tool
  4. Run codegen: PowerPC -> C++ for default.xex, the six player DLLs and the
     modules of your DLC
  5. Build the game executable

Usage:
  python scripts/build.py                 # release build
  python scripts/build.py --debug         # debug build
  python scripts/build.py --codegen-only  # stop after step 4
  python scripts/build.py --force-codegen # rerun codegen even if output exists

Used by the installer (HarmonyRecomp-Setup.exe) and the release workflow:
  --game FILE        game package to use instead of the file in game/
  --dlc-dir DIR      DLC packages (and title update) to use instead of dlc/
  --sdk-dir DIR      prebuilt SDK (install folder from a release) instead of
                     cloning and building ../rexglue-sdk
  --sdk-only         clone, patch, build and install the SDK, then stop
  --install-to DIR   after building, put the playable game into DIR
  --jobs N           parallel compile jobs (default: all CPU cores)

Requirements (Windows): scoop install llvm cmake ninja git python
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# The SDK must live OUTSIDE the project folder: before codegen, rexglue scans
# the whole project (everything except generated/) for legacy identifiers and
# asks to rewrite them. With the SDK inside, it finds hits in the SDK's own
# sources and tests and waits for a [y/N] answer, which must not be "yes".
SDK_DIR = ROOT.parent / "rexglue-sdk"
SDK_PIN_FILE = ROOT / "sdk-pin.txt"
MANIFEST = ROOT / "harmonyrecomp_manifest.toml"
# Your own DLC packages (the files from 58410A7A/00000002/) and what they unpack
# to. Like game/ and assets/ these hold game content and are never committed.
# Codegen only reads modules inside the game root, hence assets/dlc/.
DLC_DIR = ROOT / "dlc"
DLC_ASSETS = ROOT / "assets" / "dlc"
# Manifest used for codegen when DLC modules are present: the base manifest
# plus one [[modules]] entry per DLC player DLL. Generated, not committed.
LOCAL_MANIFEST = ROOT / "harmonyrecomp_manifest.local.toml"
# DLC packages whose modules made it through codegen. The game only installs
# these (src/dlc.cpp): a package whose code was not recompiled would crash.
DLC_LIST = ROOT / "generated" / "dlc_packages.txt"
DLC_LIST_NAME = "dlc_packages.txt"
# DLC packages left out of the build because codegen failed on one of their
# modules (filled in by step_codegen, read by step_dlc).
DLC_SKIPPED = ROOT / "generated" / "dlc_skipped.txt"
# Which title update is applied over assets/ ("none" or size:mtime:name).
TU_STATE = ROOT / "assets" / ".title-update"
CONTENT_TYPE_TITLE_UPDATE = 0x000B0000
# Title update 2 is used when present (HARMONYRECOMP_TITLE_UPDATE=0 ignores it).
TITLE_UPDATE_SUPPORTED = os.environ.get("HARMONYRECOMP_TITLE_UPDATE", "1") != "0"
# Codegen config for the executable with title update 2 (version 2.04).
TU_CONFIG = "harmonyrecomp_config_tu2.toml"
TITLE_ID = 0x58410A7A


def run(args, cwd=None):
    print("+ " + " ".join(str(a) for a in args), flush=True)
    result = subprocess.run([str(a) for a in args], cwd=cwd)
    if result.returncode != 0:
        sys.exit(f"error: command failed with exit code {result.returncode}")


def progress(stage: str) -> None:
    """Machine-readable stage marker for the installer (one line, flushed)."""
    print(f"##stage {stage}", flush=True)


def git(*args) -> list:
    """git command that works on drives without file ownership (exFAT/FAT32).

    safe.directory given with -c counts as protected configuration and is
    passed on to the git processes spawned for submodules.
    """
    return ["git", "-c", "safe.directory=*", *args]


def platform_id() -> str:
    os_name = platform.system()
    arch = platform.machine().lower()
    os_id = {"Windows": "win", "Linux": "linux"}.get(os_name)
    if os_id is None:
        sys.exit(f"error: unsupported OS {os_name}")
    if arch in ("x86_64", "amd64"):
        return f"{os_id}-amd64"
    if arch in ("aarch64", "arm64"):
        return f"{os_id}-arm64"
    sys.exit(f"error: unsupported architecture {arch}")


def check_tools(need_git: bool = True) -> None:
    tools = ["cmake", "ninja", "clang++"] + (["git"] if need_git else [])
    missing = [t for t in tools if shutil.which(t) is None]
    if missing:
        sys.exit(
            "error: missing tool(s): " + ", ".join(missing) +
            "\n  Windows: scoop install llvm cmake ninja git"
        )


def read_pin() -> tuple[str, str]:
    url, commit = "", ""
    for line in SDK_PIN_FILE.read_text().splitlines():
        line = line.strip()
        if line.startswith("url="):
            url = line[4:]
        elif line.startswith("commit="):
            commit = line[7:]
    if not url or not commit:
        sys.exit(f"error: {SDK_PIN_FILE.name} needs url= and commit= lines")
    return url, commit


def step_extract(game: Path | None = None) -> None:
    if (ROOT / "assets" / "default.xex").exists():
        print("[1/5] assets/default.xex present, skipping extraction")
        return
    print("[1/5] Extracting game package")
    progress("extract")
    if game is None:
        packages = [p for p in (ROOT / "game").glob("*") if p.is_file()]
        if len(packages) != 1:
            sys.exit("error: put exactly one XBLA package file (the 'LIVE' file) into game/")
        game = packages[0]
    sys.path.insert(0, str(ROOT / "scripts"))
    import extract_game  # noqa: E402  (scripts/ is not a package)
    try:
        package = extract_game.Package(game)
    except (OSError, RuntimeError) as err:
        sys.exit(f"error: {game} is not a usable game package: {err}")
    if package.title_id != TITLE_ID or package.content_type != extract_game.CONTENT_TYPE_ARCADE:
        sys.exit(f"error: {game.name} is not Castlevania: Harmony of Despair "
                 f"(title {package.title_id:08X}, content type {package.content_type:08X}); "
                 f"use the file from 58410A7A/000D0000/")
    extract_game.extract(game, ROOT / "assets", require_xex=True)


def active_manifest() -> Path:
    return LOCAL_MANIFEST if LOCAL_MANIFEST.exists() else MANIFEST


def base_guest_paths() -> set[str]:
    paths = set()
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("guest_path"):
            paths.add(line.split('"')[1].lower())
    return paths


def package_files(folder: Path) -> list[Path]:
    if not folder.is_dir():
        return []
    return sorted(p for p in folder.glob("*")
                  if p.is_file() and not p.name.startswith(".") and p.suffix.lower() != ".txt")


def file_stamp(path: Path) -> str:
    return f"{path.stat().st_size}:{int(path.stat().st_mtime)}:{path.name}"


def step_title_update(update: Path | None, game: Path | None) -> None:
    """Apply a title update over assets/, or remove one that is no longer wanted.

    A title update package (content type 000B0000) holds default.xexp and
    data/player_dll/*.dllp (binary patches, applied by the SDK when it loads
    default.xex and the player DLLs, at codegen time and in the game) plus
    replacement data files. Everything is copied over assets/; the base files
    are restored by extracting the game package again.
    """
    want = file_stamp(update) if update else "none"
    have = TU_STATE.read_text().strip() if TU_STATE.exists() else "none"
    if want == have:
        if update:
            print(f"      title update {update.name} already applied")
        return
    import extract_game  # noqa: E402
    if have != "none":
        print("      removing the old title update: extracting the game package again")
        assets = ROOT / "assets"
        for entry in assets.iterdir():
            if entry.name == "dlc":
                continue
            shutil.rmtree(entry) if entry.is_dir() else entry.unlink()
        step_extract(game)
    if update:
        staging = ROOT / "generated" / "title-update"
        if staging.exists():
            shutil.rmtree(staging)
        package = extract_game.extract(update, staging, require_xex=False)
        if not (staging / "default.xexp").exists():
            shutil.rmtree(staging)
            sys.exit(f"error: {update.name} contains no default.xexp")
        for src in staging.rglob("*"):
            if src.is_file() and src.name != "ArcadeInfo.xml":
                dst = ROOT / "assets" / src.relative_to(staging)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
        shutil.rmtree(staging)
        print(f"      applied {package.display_name or update.name}")
    TU_STATE.write_text(want + "\n")


def read_skipped() -> set[str]:
    """DLC packages left out by an earlier codegen run with the same SDK
    changes (a newer SDK patch may handle them, so they are retried then)."""
    if not DLC_SKIPPED.exists():
        return set()
    lines = [l.strip() for l in DLC_SKIPPED.read_text().splitlines() if l.strip()]
    if not lines or lines[0] != f"#sdk {sdk_patch_hash()}":
        return set()
    return set(lines[1:])


def write_skipped(names: set[str]) -> None:
    DLC_SKIPPED.parent.mkdir(parents=True, exist_ok=True)
    DLC_SKIPPED.write_text(f"#sdk {sdk_patch_hash()}\n" + "".join(n + "\n" for n in sorted(names)))


def step_dlc(dlc_dir: Path | None = None, game: Path | None = None) -> None:
    """Unpack DLC packages from dlc/ and add their modules to codegen.

    The game loads DLC code with XexLoadImage from the mounted package
    ("<device>:\\data\\player\\dllXyz.dll", "<device>:\\dll\\enemy\\..."); the
    runtime matches the path without the device prefix, so a DLC module only
    needs a manifest entry with the path inside its package. The packages
    themselves are installed into the runtime's content folder when the game
    starts (see src/dlc.cpp). A title update in the same folder is applied
    over assets/ (step_title_update).
    """
    dlc_dir = dlc_dir or DLC_DIR
    sys.path.insert(0, str(ROOT / "scripts"))
    import extract_game  # noqa: E402  (scripts/ is not a package)

    skipped = read_skipped()

    updates: list[tuple[int, Path]] = []
    known = base_guest_paths()
    modules: list[tuple[str, str, str]] = []  # (guest path, file path, name)
    included: list[str] = []
    for package_path in package_files(dlc_dir):
        try:
            package = extract_game.Package(package_path)
        except (OSError, RuntimeError) as err:
            print(f"      skipping {package_path.name}: {err}")
            continue
        if package.title_id != TITLE_ID:
            print(f"      skipping {package_path.name}: belongs to title {package.title_id:08X}")
            continue
        if package.content_type == CONTENT_TYPE_TITLE_UPDATE:
            updates.append((package_path.stat().st_size, package_path))
            continue
        if package.content_type != extract_game.CONTENT_TYPE_MARKETPLACE:
            print(f"      skipping {package_path.name}: not a DLC package "
                  f"(content type {package.content_type:08X})")
            continue
        label = package.display_name or package_path.name
        if package_path.name in skipped:
            print(f"      DLC: {label} - LEFT OUT, its code could not be recompiled "
                  f"(see {DLC_SKIPPED.relative_to(ROOT)})")
            continue
        out_dir = DLC_ASSETS / package_path.name
        marker = out_dir / ".extracted"
        stamp = f"{package_path.stat().st_size}:{int(package_path.stat().st_mtime)}"
        if not marker.exists() or marker.read_text().strip() != stamp:
            if out_dir.exists():
                shutil.rmtree(out_dir)
            extract_game.extract(package_path, out_dir, require_xex=False)
            marker.write_text(stamp + "\n")
        count = 0
        for dll in sorted(out_dir.rglob("*")):
            if not dll.is_file() or dll.suffix.lower() != ".dll":
                continue
            guest = dll.relative_to(out_dir).as_posix().lower()
            if guest in known:
                continue
            known.add(guest)
            modules.append((guest, dll.relative_to(ROOT).as_posix(), dll.stem))
            count += 1
        print(f"      DLC: {label} ({count} code modules)")
        included.append(package_path.name)

    # A console dump keeps the title update next to the DLC and the game, in
    # 58410A7A/000B0000/: look there too, so the DLC folder (00000002) can be
    # selected as it is (GitHub issue #17).
    if not updates and included:
        near = [dlc_dir.parent / "000B0000"]
        if game:
            near.append(Path(game).parent.parent / "000B0000")
        for folder in dict.fromkeys(f.resolve() for f in near if f.is_dir()):
            for path in package_files(folder):
                try:
                    package = extract_game.Package(path)
                except (OSError, RuntimeError):
                    continue
                if package.title_id == TITLE_ID and package.content_type == CONTENT_TYPE_TITLE_UPDATE:
                    print(f"      title update found next to the DLC: {path}")
                    updates.append((path.stat().st_size, path))
    # Several title updates: the newest is the biggest one (they are cumulative).
    update = max(updates)[1] if updates else None
    if update and not TITLE_UPDATE_SUPPORTED:
        print(f"      note: ignoring title update {update.name} (HARMONYRECOMP_TITLE_UPDATE=0)")
        update = None
    step_title_update(update, game)
    # The DLC needs the updated executable: its modules use interfaces that
    # version 1.04 calls differently (crash at the title screen without it).
    if included and not update:
        # Built anyway, the game would crash at the title screen; without the
        # DLC it works, so leave it out and say why.
        print("      warning: the DLC needs the title update (the 000B0000 package from your "
              "console). It is not in the DLC folder, so the DLC is left out of this build.")
        included, modules = [], []

    DLC_LIST.parent.mkdir(parents=True, exist_ok=True)
    DLC_LIST.write_text("".join(name + "\n" for name in included))
    if not included:
        print("      no DLC packages in dlc/")
    if not modules and not update:
        if LOCAL_MANIFEST.exists():
            LOCAL_MANIFEST.unlink()
        return
    text = MANIFEST.read_text(encoding="utf-8").rstrip() + "\n"
    if update:
        # Version 2.04: its own codegen config. The base characters' configs
        # list 1.04 addresses; their patched modules rely on the pointer scan
        # (SDK change 8) instead.
        text = text.replace('includes = ["harmonyrecomp_config.toml"]',
                            f'includes = ["{TU_CONFIG}"]')
        text = re.sub(r'includes = \["harmonyrecomp_dll[a-z]+\.toml"\]\n', "", text)
    text = ("# Generated by scripts/build.py from harmonyrecomp_manifest.toml, the\n"
            "# title update and the DLC packages in dlc/. Do not edit or commit.\n\n") + text
    for guest, file_path, name in modules:
        key = name.lower()
        text += (f"\n[[modules]]\n"
                 f'guest_path = "{guest}"\n'
                 f'file_path = "{file_path}"\n'
                 f'out_directory_path = "generated/{key}"\n')
        # A committed config with functions only reachable through pointers,
        # like the ones for the base characters, is used when present.
        if not update and (ROOT / f"harmonyrecomp_{key}.toml").exists():
            text += f'includes = ["harmonyrecomp_{key}.toml"]\n'
    if not LOCAL_MANIFEST.exists() or LOCAL_MANIFEST.read_text(encoding="utf-8") != text:
        LOCAL_MANIFEST.write_text(text, encoding="utf-8")


def resolve_symlink_placeholders(repo: Path) -> int:
    """Replace git symlink placeholders with copies of their targets.

    On file systems without symlink support (exFAT/FAT32, or NTFS without
    Developer Mode) git checks out a symlink as a small text file holding the
    link target, e.g. "../../libmspack/mspack/lzxd.c". The compiler then
    tries to parse that path as source code. libmspack and o1heap in the SDK
    contain such links, so turn every placeholder into a real copy.
    """
    proc = subprocess.run(
        git("ls-files", "-s", "-z", "--recurse-submodules"),
        cwd=repo, capture_output=True,
    )
    if proc.returncode != 0:
        print("warning: could not list SDK files for the symlink check:")
        print("  " + proc.stderr.decode("utf-8", "replace").strip())
        return 0
    out = proc.stdout.decode("utf-8", "replace")
    links = []
    for record in out.split("\0"):
        if record.startswith("120000 "):
            links.append(repo / record.split("\t", 1)[1])

    pending = {link.resolve() for link in links
               if not link.is_symlink() and link.is_file() and link.stat().st_size <= 1024}
    fixed = 0
    # Repeat so links that point at other links resolve too.
    for _ in range(5):
        changed = False
        for link in links:
            if link.resolve() not in pending:
                continue
            target_rel = link.read_text(encoding="utf-8", errors="replace").strip()
            target = (link.parent / target_rel).resolve()
            if target in pending or not target.exists():
                continue  # target not real yet (or missing); try next round
            link.unlink()
            if target.is_dir():
                shutil.copytree(target, link)
            else:
                shutil.copy2(target, link)
            pending.discard(link.resolve())
            fixed += 1
            changed = True
        if not changed:
            break
    if pending:
        print(f"warning: {len(pending)} symlink placeholder(s) could not be resolved:")
        for path in sorted(pending):
            print(f"  {path}")
    return fixed


# Local changes to the pinned SDK, applied idempotently after cloning.
# Each entry is (file, original text, replacement text). They are applied as
# plain text replacements (not a .patch file) so CRLF checkouts on Windows
# work too. When the set changes, step 3 rebuilds the SDK incrementally.
# A fourth element OPTIONAL marks a patch that is skipped when it does not fit.
OPTIONAL = "optional"

SDK_PATCHES = [
    # 1) The block scanner honours [[switch_tables]] from the project config.
    #    Without this, a jump table with no bounds check (sub_8277ECF0 in this
    #    game) is read for up to max_jump_table_entries entries and the
    #    function swallows its neighbour, which then fails to compile with
    #    "use of undeclared label".
    (
        "include/rex/codegen/function_scanner.h",
        """                                    const std::unordered_set<uint32_t>& knownFunctions,
                                    uint32_t pdataSize = 0);""",
        """                                    const std::unordered_set<uint32_t>& knownFunctions,
                                    uint32_t pdataSize = 0,
                                    const std::unordered_map<uint32_t, JumpTable>*
                                        manualSwitchTables = nullptr);""",
    ),
    (
        "include/rex/codegen/function_scanner.h",
        "#include <unordered_set>\n",
        "#include <unordered_map>\n#include <unordered_set>\n",
    ),
    (
        "src/codegen/function_scanner.cpp",
        """                                    const std::unordered_set<uint32_t>& knownFunctions,
                                    uint32_t pdataSize) {""",
        """                                    const std::unordered_set<uint32_t>& knownFunctions,
                                    uint32_t pdataSize,
                                    const std::unordered_map<uint32_t, JumpTable>*
                                        manualSwitchTables) {""",
    ),
    (
        "src/codegen/function_scanner.cpp",
        """          auto jt = detectJumpTable(decoded, addr, containingRegion, entryPoint, funcEnd);""",
        """          // A [[switch_tables]] entry from the project config overrides the
          // heuristic. Tables without a bounds check otherwise read up to
          // max_jump_table_entries bytes and can swallow the next function.
          std::optional<JumpTable> jt;
          if (manualSwitchTables) {
            auto manual = manualSwitchTables->find(addr);
            if (manual != manualSwitchTables->end()) {
              jt = manual->second;
              REXCODEGEN_TRACE("discoverBlocks: bctr at 0x{:08X} uses config switch table ({} targets)",
                               addr, jt->targets.size());
            }
          }
          if (!jt) {
            jt = detectJumpTable(decoded, addr, containingRegion, entryPoint, funcEnd);
          }""",
    ),
    (
        "src/codegen/phase_discover.cpp",
        "  auto result = discoverBlocks(decoded, funcAddr, *region, knownFunctions, pdataSize);",
        "  auto result = discoverBlocks(decoded, funcAddr, *region, knownFunctions, pdataSize,\n"
        "                               &ctx.Config().switchTables);",
    ),
]
# SDK change 2: relocatable indirect-call table.
#
# The dispatch table of a module always sat right after its image
# (image_base + image_size). For Harmony of Despair the main table is ~13 MB
# and ends at 0x836BE990, but the player DLLs load at fixed addresses from
# 0x83000000, so loading dllAlucard.dll failed ("Module image range ...
# overlaps existing ..."). This adds an optional table_base: 0 keeps the old
# placement, a project can move one module's table by defining
# REX_FUNC_TABLE_BASE for that module's generated code.
SDK_PATCHES += [
    (
        "include/rex/image_info.h",
        "  RegisterModulesFunc register_modules = nullptr;  ///< Set by codegen for multi-binary projects\n",
        "  RegisterModulesFunc register_modules = nullptr;  ///< Set by codegen for multi-binary projects\n"
        "  u32 table_base = 0;  ///< Guest address of the indirect-call table; 0 = image_base + image_size\n",
    ),
    (
        "resources/templates/codegen/init_h.inja",
        "#define REX_THUNK_RESERVE_SIZE {{ thunk_reserve_size }}ull\n",
        "#define REX_THUNK_RESERVE_SIZE {{ thunk_reserve_size }}ull\n"
        "// Guest address of this module's indirect-call table. Defaults to right\n"
        "// after the image; define it for the module's target to move the table.\n"
        "#ifndef REX_FUNC_TABLE_BASE\n"
        "#define REX_FUNC_TABLE_BASE (REX_IMAGE_BASE + REX_IMAGE_SIZE)\n"
        "#endif\n",
    ),
    (
        "resources/templates/codegen/init_cpp.inja",
        "    .register_modules = RegisterRecompiledModules,\n{% endif %}\n};\n",
        "    .register_modules = RegisterRecompiledModules,\n{% endif %}\n"
        "    .table_base = static_cast<u32>(REX_FUNC_TABLE_BASE),\n};\n",
    ),
    (
        "resources/templates/codegen/_indirect_call.inja",
        "  (*(PPCFunc**)(x + REX_IMAGE_BASE + REX_IMAGE_SIZE + (u64(u32(y) - REX_CODE_BASE) * 2)))",
        "  (*(PPCFunc**)(x + REX_FUNC_TABLE_BASE + (u64(u32(y) - REX_CODE_BASE) * 2)))",
    ),
    (
        "include/rex/system/xmemory.h",
        "  bool InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,\n"
        "                               uint32_t image_size);\n",
        "  bool InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,\n"
        "                               uint32_t image_size, uint32_t table_base = 0);\n",
    ),
    (
        "src/system/xmemory.cpp",
        "bool Memory::InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,\n"
        "                                     uint32_t image_size) {\n"
        "  constexpr uint32_t kThunkReserveSize = runtime::FunctionDispatcher::kThunkReserveSize;\n"
        "  uint32_t table_base = image_base + image_size;\n",
        "bool Memory::InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,\n"
        "                                     uint32_t image_size, uint32_t table_base_override) {\n"
        "  constexpr uint32_t kThunkReserveSize = runtime::FunctionDispatcher::kThunkReserveSize;\n"
        "  uint32_t table_base = table_base_override ? table_base_override : image_base + image_size;\n",
    ),
    (
        "include/rex/system/function_dispatcher.h",
        "  bool InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,\n"
        "                               uint32_t image_size, bool is_entrypoint = false);\n",
        "  bool InitializeFunctionTable(uint32_t code_base, uint32_t code_size, uint32_t image_base,\n"
        "                               uint32_t image_size, bool is_entrypoint = false,\n"
        "                               uint32_t table_base = 0);\n",
    ),
    (
        "include/rex/system/function_dispatcher.h",
        "    uint32_t image_size;\n    uint32_t next_thunk_address;\n",
        "    uint32_t image_size;\n    uint32_t table_base;\n    uint32_t next_thunk_address;\n",
    ),
    (
        "src/system/function_dispatcher.cpp",
        "                                                 uint32_t image_base, uint32_t image_size,\n"
        "                                                 bool is_entrypoint) {\n",
        "                                                 uint32_t image_base, uint32_t image_size,\n"
        "                                                 bool is_entrypoint, uint32_t table_base) {\n",
    ),
    (
        "src/system/function_dispatcher.cpp",
        "  uint32_t new_table_end = image_base + image_size + (code_size + kThunkReserveSize) * 2;\n"
        "  uint32_t new_code_end = code_base + code_size + kThunkReserveSize;\n"
        "  for (const auto& existing : module_tables_) {\n"
        "    uint32_t existing_table_end =\n"
        "        existing.image_base + existing.image_size + (existing.code_size + kThunkReserveSize) * 2;\n"
        "    uint32_t existing_code_end = existing.code_base + existing.code_size + kThunkReserveSize;\n"
        "    if (image_base < existing_table_end && new_table_end > existing.image_base) {\n"
        "      REXLOG_ERROR(\"Module image range [{:08X}, {:08X}) overlaps existing [{:08X}, {:08X})\",\n"
        "                   image_base, new_table_end, existing.image_base, existing_table_end);\n"
        "      return false;\n"
        "    }\n",
        "  if (table_base == 0) {\n"
        "    table_base = image_base + image_size;\n"
        "  }\n"
        "  uint32_t new_table_end = table_base + (code_size + kThunkReserveSize) * 2;\n"
        "  uint32_t new_image_end = image_base + image_size;\n"
        "  uint32_t new_code_end = code_base + code_size + kThunkReserveSize;\n"
        "  auto overlaps = [](uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {\n"
        "    return a0 < b1 && b0 < a1;\n"
        "  };\n"
        "  for (const auto& existing : module_tables_) {\n"
        "    uint32_t existing_image_end = existing.image_base + existing.image_size;\n"
        "    uint32_t existing_table_end =\n"
        "        existing.table_base + (existing.code_size + kThunkReserveSize) * 2;\n"
        "    uint32_t existing_code_end = existing.code_base + existing.code_size + kThunkReserveSize;\n"
        "    // Image and table of the new module must not overlap the image or the\n"
        "    // table of any module already registered.\n"
        "    if (overlaps(image_base, new_image_end, existing.image_base, existing_image_end) ||\n"
        "        overlaps(image_base, new_image_end, existing.table_base, existing_table_end) ||\n"
        "        overlaps(table_base, new_table_end, existing.image_base, existing_image_end) ||\n"
        "        overlaps(table_base, new_table_end, existing.table_base, existing_table_end)) {\n"
        "      REXLOG_ERROR(\n"
        "          \"Module image [{:08X}, {:08X}) / table [{:08X}, {:08X}) overlaps existing image \"\n"
        "          \"[{:08X}, {:08X}) / table [{:08X}, {:08X})\",\n"
        "          image_base, new_image_end, table_base, new_table_end, existing.image_base,\n"
        "          existing_image_end, existing.table_base, existing_table_end);\n"
        "      return false;\n"
        "    }\n",
    ),
    (
        "src/system/function_dispatcher.cpp",
        "  if (!memory_->InitializeFunctionTable(code_base, code_size, image_base, image_size)) {\n",
        "  if (!memory_->InitializeFunctionTable(code_base, code_size, image_base, image_size,\n"
        "                                        table_base)) {\n",
    ),
    (
        "src/system/function_dispatcher.cpp",
        "      .image_size = image_size,\n      .next_thunk_address = code_base + code_size,\n",
        "      .image_size = image_size,\n      .table_base = table_base,\n"
        "      .next_thunk_address = code_base + code_size,\n",
    ),
    (
        "src/system/function_dispatcher.cpp",
        "  REXLOG_INFO(\"Function table initialized for module: code={:08X}-{:08X}, image={:08X}-{:08X}\",\n"
        "              code_base, code_base + code_size, image_base, image_base + image_size);\n",
        "  REXLOG_INFO(\n"
        "      \"Function table initialized for module: code={:08X}-{:08X}, image={:08X}-{:08X}, \"\n"
        "      \"table={:08X}-{:08X}\",\n"
        "      code_base, code_base + code_size, image_base, image_base + image_size, table_base,\n"
        "      new_table_end);\n",
    ),
    (
        "src/system/runtime.cpp",
        "                                                     image_info.image_base, image_info.image_size,\n"
        "                                                     /*is_entrypoint=*/true)) {\n",
        "                                                     image_info.image_base, image_info.image_size,\n"
        "                                                     /*is_entrypoint=*/true,\n"
        "                                                     image_info.table_base)) {\n",
    ),
]

# SDK change 3: XamShowMessageBoxUI read title and text without byte-swapping.
# Guest UTF-16 is big-endian, so "S" (0x0053) arrived as 0x5300, a CJK glyph
# missing from the overlay font, and the save prompts showed only "?". The
# buttons already used load_and_swap and were fine. Also log the text so we
# can see what the game asks.
SDK_PATCHES += [
    (
        "src/kernel/xam/xam_ui.cpp",
        "    title = rex::string::to_utf8(title_ptr.value());\n",
        "    // Guest strings are big-endian UTF-16; value() returns them unswapped.\n"
        "    title = rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(\n"
        "        REX_KERNEL_MEMORY()->TranslateVirtual(title_ptr.guest_address())));\n",
    ),
    (
        "src/kernel/xam/xam_ui.cpp",
        "  std::vector<std::string> buttons;\n",
        "  std::string text;\n"
        "  if (text_ptr) {\n"
        "    text = rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(\n"
        "        REX_KERNEL_MEMORY()->TranslateVirtual(text_ptr.guest_address())));\n"
        "  }\n"
        "  REXKRNL_INFO(\"XamShowMessageBoxUI: title=\\\"{}\\\" text=\\\"{}\\\"\", title, text);\n"
        "\n"
        "  std::vector<std::string> buttons;\n",
    ),
    (
        "src/kernel/xam/xam_ui.cpp",
        "new MessageBoxDialog(imgui_drawer, title, rex::string::to_utf8(text_ptr.value()), buttons,",
        "new MessageBoxDialog(imgui_drawer, title, text, buttons,",
    ),
]

# SDK change 4: SaveConfig wrote string values without TOML escaping. A
# Windows path such as game_data_root = "H:\Castlevania HD\..." is invalid
# TOML, so on the next start the whole settings file failed to parse and every
# setting in it (resolution, anti-aliasing, license_mask) was silently ignored.
SDK_PATCHES += [
    (
        "src/core/cvar.cpp",
        "std::string SerializeToTOML() {\n",
        "// Quote a value as a TOML basic string (escapes \\, \" and control chars).\n"
        "static std::string TomlQuote(const std::string& value) {\n"
        "  std::string out = \"\\\"\";\n"
        "  for (unsigned char c : value) {\n"
        "    switch (c) {\n"
        "      case '\\\\': out += \"\\\\\\\\\"; break;\n"
        "      case '\"': out += \"\\\\\\\"\"; break;\n"
        "      case '\\b': out += \"\\\\b\"; break;\n"
        "      case '\\t': out += \"\\\\t\"; break;\n"
        "      case '\\n': out += \"\\\\n\"; break;\n"
        "      case '\\f': out += \"\\\\f\"; break;\n"
        "      case '\\r': out += \"\\\\r\"; break;\n"
        "      default:\n"
        "        if (c < 0x20 || c == 0x7F) {\n"
        "          static const char kHex[] = \"0123456789ABCDEF\";\n"
        "          out += \"\\\\u00\";\n"
        "          out += kHex[c >> 4];\n"
        "          out += kHex[c & 0xF];\n"
        "        } else {\n"
        "          out += static_cast<char>(c);\n"
        "        }\n"
        "    }\n"
        "  }\n"
        "  out += '\"';\n"
        "  return out;\n"
        "}\n"
        "\n"
        "std::string SerializeToTOML() {\n",
    ),
    (
        "src/core/cvar.cpp",
        "    if (entry.getter() != entry.default_value) {\n"
        "      if (entry.type == FlagType::String) {\n"
        "        result += entry.name + \" = \\\"\" + entry.getter() + \"\\\"\\n\";\n",
        "    if (entry.getter() != entry.default_value) {\n"
        "      if (entry.type == FlagType::String) {\n"
        "        result += entry.name + \" = \" + TomlQuote(entry.getter()) + \"\\n\";\n",
    ),
    (
        "src/core/cvar.cpp",
        "    if (entry.category == category && entry.getter() != entry.default_value) {\n"
        "      if (entry.type == FlagType::String) {\n"
        "        result += entry.name + \" = \\\"\" + entry.getter() + \"\\\"\\n\";\n",
        "    if (entry.category == category && entry.getter() != entry.default_value) {\n"
        "      if (entry.type == FlagType::String) {\n"
        "        result += entry.name + \" = \" + TomlQuote(entry.getter()) + \"\\n\";\n",
    ),
]

# SDK change 5: relocatable indirect-call tables for DLL modules too.
# Each player DLL's table sat right after its image and overlapped the load
# address of the next character (e.g. Alucard's table 0x83090000-0x830F0328 vs
# Soma's image at 0x83090000). A DLL now exports ReXModule_TableBase(), and
# the loader passes that address on; CMakeLists.txt gives every player DLL its
# own slot from 0x8D000000.
SDK_PATCHES += [
    (
        "resources/templates/codegen/register_cpp.inja",
        "extern \"C\" REX_MODULE_EXPORT\n"
        "void ReXModule_Register(rex::runtime::IModuleRegistrar* registrar) {\n",
        "// Guest address of this module's indirect-call table (REX_FUNC_TABLE_BASE).\n"
        "extern \"C\" REX_MODULE_EXPORT\n"
        "uint32_t ReXModule_TableBase() {\n"
        "  return static_cast<uint32_t>(REX_FUNC_TABLE_BASE);\n"
        "}\n"
        "\n"
        "extern \"C\" REX_MODULE_EXPORT\n"
        "void ReXModule_Register(rex::runtime::IModuleRegistrar* registrar) {\n",
    ),
    (
        "src/system/kernel_state.cpp",
        "        auto* xex = module->xex_module();\n",
        "        // Where the module keeps its indirect-call table; 0 = right after\n"
        "        // the image (modules generated before ReXModule_TableBase existed).\n"
        "        uint32_t table_base = 0;\n"
        "        using TableBaseFn = uint32_t (*)();\n"
        "        if (auto table_base_func = reinterpret_cast<TableBaseFn>(\n"
        "                library_local.GetSymbol(\"ReXModule_TableBase\"))) {\n"
        "          table_base = table_base_func();\n"
        "        }\n"
        "        auto* xex = module->xex_module();\n",
    ),
    (
        "src/system/kernel_state.cpp",
        "text->address, text->size, xex->base_address(), xex->image_size())) {",
        "text->address, text->size, xex->base_address(), xex->image_size(),\n"
        "                       /*is_entrypoint=*/false, table_base)) {",
    ),
]

# SDK change 6: stale handles in guest dispatcher headers.
#
# For guest-allocated KEVENTs / critical sections the SDK creates a host object
# on first use and caches its handle in the guest header (wait_list_flink =
# 'REX\0', wait_list_blink = handle). When the game frees that memory and later
# reuses it (RtlInitializeCriticalSection does not clear those two words), the
# cached handle may by then belong to a completely different object - a socket,
# a thread - and waits, sets and ObDereferenceObject act on that object. In
# Harmony of Despair online sessions this released the game's socket handle and
# crashed in RtlEnterCriticalSection when a co-op map started. The cached handle
# is now only used if it still refers to an object created for this header.
SDK_PATCHES += [
    (
        "src/system/xobject.cpp",
        "#include <vector>\n\n#include <rex/chrono/clock.h>\n",
        "#include <mutex>\n#include <unordered_map>\n#include <vector>\n\n#include <rex/chrono/clock.h>\n",
    ),
    (
        "src/system/xobject.cpp",
        "object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state, void* native_ptr,\n"
        "                                             int32_t as_type) {\n",
        "// Objects created by GetNativeObject, and the guest header each belongs to.\n"
        "static std::mutex g_native_owner_mutex;\n"
        "static std::unordered_map<const XObject*, const void*> g_native_owner;\n"
        "\n"
        "// True if the handle cached in a guest header still refers to the object that\n"
        "// was created for that header (and not to an unrelated object that reused the\n"
        "// handle number after the guest freed and recycled the memory).\n"
        "static bool NativeHandleStillValid(KernelState* kernel_state, const XObject* object,\n"
        "                                   const void* native_ptr, int32_t as_type) {\n"
        "  XObject::Type expected;\n"
        "  switch (as_type) {\n"
        "    case 0:\n"
        "    case 1:\n"
        "      expected = XObject::Type::Event;\n"
        "      break;\n"
        "    case 2:\n"
        "      expected = XObject::Type::Mutant;\n"
        "      break;\n"
        "    case 5:\n"
        "      expected = XObject::Type::Semaphore;\n"
        "      break;\n"
        "    case 6:\n"
        "      expected = XObject::Type::Thread;\n"
        "      break;\n"
        "    case 8:\n"
        "    case 9:\n"
        "      expected = XObject::Type::Timer;\n"
        "      break;\n"
        "    default:\n"
        "      return true;  // unknown kind: keep the old behaviour\n"
        "  }\n"
        "  if (object->type() != expected) {\n"
        "    return false;\n"
        "  }\n"
        "  if (object->guest_object() &&\n"
        "      object->guest_object() == kernel_state->memory()->HostToGuestVirtual(native_ptr)) {\n"
        "    return true;  // CreateNative object living in this header\n"
        "  }\n"
        "  std::lock_guard lock(g_native_owner_mutex);\n"
        "  auto it = g_native_owner.find(object);\n"
        "  return it != g_native_owner.end() && it->second == native_ptr;\n"
        "}\n"
        "\n"
        "object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state, void* native_ptr,\n"
        "                                             int32_t as_type) {\n",
    ),
    (
        "src/system/xobject.cpp",
        "    uint32_t handle = header->wait_list_blink;\n"
        "    auto object = kernel_state->object_table()->LookupObject<XObject>(handle);\n"
        "\n"
        "    // TODO(benvanik): assert nothing has been changed in the struct.\n"
        "    return object;\n"
        "  } else {\n",
        "    uint32_t handle = header->wait_list_blink;\n"
        "    auto object = kernel_state->object_table()->LookupObject<XObject>(handle);\n"
        "    if (object && NativeHandleStillValid(kernel_state, object.get(), native_ptr, as_type)) {\n"
        "      return object;\n"
        "    }\n"
        "    // Stale: the guest reused this memory. Create a fresh object below.\n"
        "  }\n"
        "  {\n",
    ),
    (
        "src/system/xobject.cpp",
        "    StashHandle(header, object->handle());\n"
        "\n"
        "    return object_ref<XObject>(object);\n",
        "    StashHandle(header, object->handle());\n"
        "    {\n"
        "      std::lock_guard lock(g_native_owner_mutex);\n"
        "      g_native_owner[object] = native_ptr;\n"
        "    }\n"
        "\n"
        "    return object_ref<XObject>(object);\n",
    ),
]

PATCH_STAMP = SDK_DIR / ".harmonyrecomp-patches"


# SDK change 7: DLL modules that share a load address.
#
# Codegen loads every DLL module into one guest address space before analysing
# them. The DLC stage bosses (DLL_Region, DLL_FcDracula, DLL_Ryukotsu,
# DLL_PsBeel, ...) all load at 0x8BF00000 - the game only ever has one of them
# loaded - so every boss after the first failed to load properly: its import
# thunks stayed unresolved and codegen stopped with "target not in any
# function" (GitHub issues #1, #3, #4). Each module is now copied into its
# BinaryView (which owns its section data) and unloaded again before the next
# one is loaded.
SDK_PATCHES += [
    (
        "src/codegen/project_recompiler.cpp",
        "  std::vector<rex::system::object_ref<rex::system::UserModule>> dllModules;\n",
        "  // Copied out and unloaded one by one: DLC modules may share a load address.\n"
        "  std::vector<BinaryView> dllViews;\n",
    ),
    (
        "src/codegen/project_recompiler.cpp",
        "                     userMod->xex_module()->base_address());\n"
        "    dllModules.push_back(std::move(userMod));\n",
        "                     userMod->xex_module()->base_address());\n"
        "    dllViews.push_back(BinaryView::fromModule(*userMod->xex_module()));\n"
        "    runtime->kernel_state()->UnloadUserModule(userMod, /*call_entry=*/false);\n",
    ),
    (
        "src/codegen/project_recompiler.cpp",
        "  for (size_t i = 0; i < dllModules.size(); ++i) {\n"
        "    auto& userMod = dllModules[i];\n"
        "    auto bv = BinaryView::fromModule(*userMod->xex_module());\n",
        "  for (size_t i = 0; i < dllViews.size(); ++i) {\n"
        "    auto bv = std::move(dllViews[i]);\n",
    ),
    (
        "src/codegen/project_recompiler.cpp",
        "    for (size_t i = 0; i < dllModules.size(); ++i) {\n"
        "      auto& mod = targeted[i + 1];\n",
        "    for (size_t i = 0; i < dllViews.size(); ++i) {\n"
        "      auto& mod = targeted[i + 1];\n",
    ),
    (
        # Only the executable must not overlap a DLL; DLLs that share an
        # address are never loaded at the same time, and the runtime checks
        # the modules actually loaded (FunctionDispatcher).
        "src/codegen/project_recompiler.cpp",
        "      uint32_t b_end = b_base + contexts[j].ctx.binary().imageSize();\n"
        "      if (a_base < b_end && b_base < a_end) {\n",
        "      uint32_t b_end = b_base + contexts[j].ctx.binary().imageSize();\n"
        "      if (i == 0 && a_base < b_end && b_base < a_end) {\n",
    ),
]


# SDK change 8: small functions only reachable through pointers in data.
#
# Leaf functions have no .pdata entry, and the ones the game only reaches
# through a callback table (not a vtable) were never discovered; the first call
# ended with "Call to invalid or unregistered function". For the base game they
# are listed by hand in harmonyrecomp_config.toml / harmonyrecomp_dll*.toml;
# the ~100 DLC modules need the same, e.g. 0x83CA1488 in dllshotFCvkshot.dll
# (crash right after the splash screen with all DLC installed). DLL modules
# now get the same scan the hand-written lists came from: a 32-bit word in a
# data section that points into code, right after a return, branch or padding,
# not inside a function known from .pdata, with a blr/bctr/b within 64
# instructions, is registered as a function.
_POINTER_SCAN = """\
  // HarmonyRecomp: functions referenced only from data (callback tables).
  if (ctx.isDllModule()) {
    std::vector<std::pair<uint32_t, uint32_t>> pdataRanges(ctx.scan.pdataSizes.begin(),
                                                           ctx.scan.pdataSizes.end());
    std::sort(pdataRanges.begin(), pdataRanges.end());
    auto insidePdataFunction = [&](uint32_t addr) {
      auto it = std::upper_bound(pdataRanges.begin(), pdataRanges.end(),
                                 std::make_pair(addr, UINT32_MAX));
      if (it == pdataRanges.begin()) return false;
      --it;
      return addr > it->first && addr < it->first + it->second;
    };
    auto codeWord = [&](uint32_t addr, uint32_t* out) {
      const uint8_t* p = binary.isExecutable(addr) ? binary.translate(addr) : nullptr;
      if (!p) return false;
      *out = load_and_swap<uint32_t>(p);
      return true;
    };
    auto endsFlow = [](uint32_t insn) {
      return insn == 0x4E800020u || insn == 0x4E800420u ||   // blr, bctr
             ((insn >> 26) == 18 && (insn & 3) == 0);        // b
    };
    size_t added = 0;
    for (const auto& section : binary.sections()) {
      if (section.executable) continue;
      for (uint32_t off = 0; off + 4 <= section.size; off += 4) {
        uint32_t target = load_and_swap<uint32_t>(section.data + off);
        uint32_t insn = 0;
        if ((target & 3) || !codeWord(target, &insn) || insn == 0) continue;
        if (graph.isEntryPoint(target) || binary.isInImportExportRange(target)) continue;
        if (insidePdataFunction(target)) continue;
        uint32_t prev = 0;
        if (codeWord(target - 4, &prev) && prev != 0 && !endsFlow(prev)) continue;
        bool ends = false;
        for (uint32_t i = 0; i < 64; ++i) {
          uint32_t w = 0;
          if (!codeWord(target + i * 4, &w) || w == 0) break;
          if (endsFlow(w)) { ends = true; break; }
        }
        if (!ends) continue;
        graph.addFunction(target, 4, FunctionAuthority::DISCOVERED, true);
        ++added;
      }
    }
    if (added > 0) {
      REXCODEGEN_DEBUG("Analyze: data pointer scan found {} functions", added);
      auto knownFunctions = buildKnownFunctions(graph);
      discoverPendingFunctions(ctx, knownFunctions);
    }
  }

"""
SDK_PATCHES += [
    (
        "src/codegen/phase_discover.cpp",
        "#include <array>\n#include <bitset>\n",
        "#include <algorithm>\n#include <array>\n#include <bitset>\n#include <climits>\n"
        "#include <cstdint>\n#include <vector>\n",
    ),
    (
        "src/codegen/phase_discover.cpp",
        '  REXCODEGEN_TRACE("Analyze: {} total functions after vtable scan", graph.functionCount());\n',
        _POINTER_SCAN +
        '  REXCODEGEN_TRACE("Analyze: {} total functions after vtable scan", graph.functionCount());\n',
    ),
]


# SDK change 9: title update patches in codegen on Windows.
#
# build.py turns on xex_apply_patches for codegen with REX_XEX_APPLY_PATCHES.
# On Windows the cvar lives in rexruntime.dll while rexglue.exe applies the
# environment to its own copy of the cvar registry, so the patch was never
# applied there and codegen analysed version 1.04 with the 2.04 config. The
# loader now also reads the variable itself (the game sets it too).
SDK_PATCHES += [
    (
        "src/system/user_module.cpp",
        "  if (REXCVAR_GET(xex_apply_patches)) {\n",
        "  const char* apply_patches_env = std::getenv(\"REX_XEX_APPLY_PATCHES\");\n"
        "  if (REXCVAR_GET(xex_apply_patches) ||\n"
        "      (apply_patches_env && (apply_patches_env[0] == '1' || apply_patches_env[0] == 't'))) {\n",
    ),
    (
        "src/system/user_module.cpp",
        "#include <vector>\n",
        "#include <cstdlib>\n#include <vector>\n",
    ),
]


# SDK change 10: unloading a recompiled DLL never released it.
#
# LoadUserModule looks the recompiled code up by the path the title passed
# (normalized to "dll/enemy/dll_psbeel.dll"), UnloadUserModule by the module's
# resolved path ("\Device\Content\8\dll\enemy\DLL_PsBeel.dll", normalized to
# "device/content/8/dll/..."), which matches nothing. The code, its function
# table and the library stayed registered, so the next load of the same module
# was refused ("already loaded; refusing duplicate load") and the game crashed:
# replaying the Lord of Flies stage. Fall back to the loaded recompiled module
# whose guest path ends the resolved path.
SDK_PATCHES += [
    (
        "src/system/kernel_state.cpp",
        "    auto recomp = FindRecompiledModule(module->path());\n"
        "    if (recomp) {\n"
        "      const std::string& key = recomp->guest_path;\n"
        "      auto cleared_range = function_dispatcher_->UnregisterModule(key);\n",
        "    auto recomp = FindRecompiledModule(module->path());\n"
        "    if (!recomp) {\n"
        "      const std::string resolved = NormalizeGuestPath(module->path());\n"
        "      for (const auto& info : recompiled_modules_) {\n"
        "        const std::string& gp = info.guest_path;\n"
        "        if (!gp.empty() && resolved.size() > gp.size() && resolved.ends_with(gp) &&\n"
        "            resolved[resolved.size() - gp.size() - 1] == '/' &&\n"
        "            module_libraries_.contains(gp)) {\n"
        "          recomp = info;\n"
        "          break;\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "    if (recomp) {\n"
        "      const std::string& key = recomp->guest_path;\n"
        "      auto cleared_range = function_dispatcher_->UnregisterModule(key);\n",
    ),
]


# SDK change 11: heap corruption when the game closes (GitHub issue #10).
#
# ReXApp::OnDestroy destroyed the window first and the runtime after it. The
# runtime's input drivers keep a pointer to the window: the keyboard/mouse
# driver's destructor then called RemoveInputListener on the destroyed window,
# corrupting the heap ("exception 0xC0000374" in rexruntime.dll from
# Runtime::~Runtime -> InputSystem::Shutdown). Its OnClosing handler detaches
# it only when the window is closed by the user, not when the game quits. The
# input drivers are now shut down while the window still exists.
SDK_PATCHES += [
    (
        "src/ui/rex_app.cpp",
        "  if (window_) {\n"
        "    window_->RemoveInputListener(this);\n"
        "    window_->RemoveListener(this);\n"
        "  }\n"
        "  window_.reset();\n"
        "  runtime_.reset();\n",
        "  // Input drivers hold the window and detach from it in their destructors,\n"
        "  // so they go first, while the window still exists.\n"
        "  if (runtime_ && runtime_->input_system()) {\n"
        "    runtime_->input_system()->Shutdown();\n"
        "  }\n"
        "  if (window_) {\n"
        "    window_->RemoveInputListener(this);\n"
        "    window_->RemoveListener(this);\n"
        "  }\n"
        "  window_.reset();\n"
        "  runtime_.reset();\n",
    ),
]


# SDK change 12: a frame counter that counts frames (GitHub issue #11).
#
# CommandProcessor::counter() goes up on every guest vblank AND on every
# buffer swap (it is the value the game reads back as the GPU counter), so the
# FPS counter showed 60 + the real frame rate: 120 at a smooth 60 FPS, 90 when
# the game actually ran at 30. swap_count() counts the swaps alone.
SDK_PATCHES += [
    (
        "include/rex/graphics/command_processor.h",
        "  uint32_t counter() const { return counter_; }\n",
        "  uint32_t counter() const { return counter_; }\n"
        "  // Buffer swaps (frames the game presented); counter() also counts vblanks.\n"
        "  uint32_t swap_count() const { return swap_count_; }\n",
    ),
    (
        "include/rex/graphics/command_processor.h",
        "  uint32_t counter_ = 0;\n",
        "  uint32_t counter_ = 0;\n"
        "  uint32_t swap_count_ = 0;\n",
    ),
    (
        "src/graphics/command_processor.cpp",
        "  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n\n  ++counter_;\n",
        "  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n\n  ++counter_;\n"
        "  ++swap_count_;\n",
    ),
]


# SDK change 13: frame rate limit other than 60 Hz.
#
# The game's frame clock is the guest vblank, made by a host thread at the
# video mode's 60 Hz while "vsync" is on (every 1 ms when off). The game
# measures time itself, so faster vblanks only mean more frames, not a faster
# game (tested). vsync_rate sets that rate (0 = the video mode's 60 Hz) for
# the F6 menu's 60/90/120/144/unlocked choice; it applies immediately.
SDK_PATCHES += [
    (
        "src/graphics/graphics_system.cpp",
        'REXCVAR_DEFINE_STRING(trace_gpu_prefix, "", "GPU", "GPU trace file prefix");\n',
        'REXCVAR_DEFINE_STRING(trace_gpu_prefix, "", "GPU", "GPU trace file prefix");\n'
        'REXCVAR_DEFINE_INT32(vsync_rate, 0, "GPU",\n'
        '                     "Frame rate limit in Hz while vsync is on (0 = the video mode\'s, 60)");\n',
    ),
    (
        "src/graphics/graphics_system.cpp",
        "          uint64_t interval_ticks =\n"
        "              REXCVAR_GET(vsync) ? vsync_interval_ticks : no_vsync_interval_ticks;\n",
        "          const int32_t rate = REXCVAR_GET(vsync_rate);\n"
        "          const uint64_t rate_ticks =\n"
        "              rate > 0 ? std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / rate))\n"
        "                       : vsync_interval_ticks;\n"
        "          uint64_t interval_ticks = REXCVAR_GET(vsync) ? rate_ticks : no_vsync_interval_ticks;\n",
    ),
]


# SDK change 14: keyboard/mouse mode keeps the mouse while a dialog is open.
#
# With mnk_mode on, the keyboard/mouse driver captures the mouse (hidden, held
# in the window centre) whenever the game reads the controller. It only let go
# for the SDK's own F3/F4/console overlays, so the profile and sign-in dialogs
# (and the F6 menu) could not be clicked: the mouse was "held hostage in the
# centre". Dialogs now say whether they need the mouse; while one that does is
# open, the driver releases it and the game gets no keyboard/mouse input.
SDK_PATCHES += [
    (
        "include/rex/ui/imgui_dialog.h",
        "  void Draw();\n",
        "  void Draw();\n"
        "\n"
        "  // Whether the mouse (and keyboard) belong to this dialog while it is open;\n"
        "  // false for display-only overlays (FPS counter, frame times).\n"
        "  virtual bool WantsMouse() const { return true; }\n",
    ),
    (
        "include/rex/ui/imgui_drawer.h",
        "#include <cstddef>\n",
        "#include <atomic>\n#include <cstddef>\n",
    ),
    (
        "include/rex/ui/imgui_drawer.h",
        "  void AddDialog(ImGuiDialog* dialog);\n",
        "  void AddDialog(ImGuiDialog* dialog);\n"
        "  // True while a drawn dialog wants the mouse (updated every frame).\n"
        "  bool HasMouseDialogs() const { return mouse_dialog_count_.load() > 0; }\n",
    ),
    (
        "include/rex/ui/imgui_drawer.h",
        "  // All currently-attached dialogs that get drawn.\n",
        "  std::atomic<int> mouse_dialog_count_{0};\n"
        "  // All currently-attached dialogs that get drawn.\n",
    ),
    (
        "src/ui/imgui_drawer.cpp",
        "  if (dialogs_.empty()) {\n    return;\n  }\n",
        "  if (dialogs_.empty()) {\n    mouse_dialog_count_ = 0;\n    return;\n  }\n",
    ),
    (
        "src/ui/imgui_drawer.cpp",
        "  dialog_loop_next_index_ = SIZE_MAX;\n\n  ImGui::Render();\n",
        "  dialog_loop_next_index_ = SIZE_MAX;\n"
        "  int mouse_dialogs = 0;\n"
        "  for (auto* dialog : dialogs_) {\n"
        "    mouse_dialogs += dialog->WantsMouse() ? 1 : 0;\n"
        "  }\n"
        "  mouse_dialog_count_ = mouse_dialogs;\n"
        "\n  ImGui::Render();\n",
    ),
    (
        "include/rex/ui/overlay/debug_overlay.h",
        "  explicit DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider = {});\n",
        "  explicit DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider = {});\n"
        "  bool WantsMouse() const override { return false; }\n",
    ),
    (
        "src/ui/rex_app.cpp",
        "      input_sys->SetActiveCallback([this]() {\n",
        "      input_sys->SetActiveCallback([this]() {\n"
        "        if (imgui_drawer_->HasMouseDialogs())\n"
        "          return false;\n",
    ),
]


# SDK change 15: loading a DLL module wiped the image heap's bookkeeping.
#
# XexModule::ReadImage reset the whole 0x80000000 heap's page table before
# decoding every module - meant for the executable, but run for each DLL too.
# Every DLL load forgot all earlier allocations there (the executable, other
# DLLs, the indirect-call tables), so unloading them later failed with
# "BaseHeap::Release failed because address is not a region start" (one per
# module when leaving a DLC stage) and their memory was never given back;
# allocations could also silently overlap. Only the executable resets it now
# (by the DLL flag: game DLLs carry XEX_MODULE_TITLE as well).
SDK_PATCHES += [
    # Update for SDK folders that got the first version of this change, which
    # missed that game DLLs carry XEX_MODULE_TITLE too (is_executable() was
    # true for them, so nothing changed). Skipped when it does not apply.
    (
        "src/system/xex_module.cpp",
        "  // before it (the executable, other DLLs, the indirect-call tables).\n"
        "  if (is_executable()) {\n",
        "  // before it (the executable, other DLLs, the indirect-call tables).\n"
        "  // Game DLLs carry XEX_MODULE_TITLE too, so check the DLL flag.\n"
        "  if (is_executable() && !(xex_header()->module_flags & XEX_MODULE_DLL_MODULE)) {\n",
        OPTIONAL,
    ),
    (
        "src/system/xex_module.cpp",
        "    return 0;\n  }\n\n  memory()->LookupHeap(base_address_)->Reset();\n",
        "    return 0;\n  }\n\n"
        "  // Only for the executable: a DLL must not forget the allocations made\n"
        "  // before it (the executable, other DLLs, the indirect-call tables).\n"
        "  // Game DLLs carry XEX_MODULE_TITLE too, so check the DLL flag.\n"
        "  if (is_executable() && !(xex_header()->module_flags & XEX_MODULE_DLL_MODULE)) {\n"
        "    memory()->LookupHeap(base_address_)->Reset();\n"
        "  }\n",
    ),
]


# Update for SDK folders that got the first version of change 16 (without
# ignoring buttons held when a dialog opens). Must come before change 16.
SDK_PATCHES += [
    (
        "include/rex/ui/imgui_drawer.h",
        "  bool gamepad_fed_ = false;\n  void FeedGamepad(ImGuiIO& io);\n",
        "  bool gamepad_fed_ = false;\n  uint16_t gamepad_mask_ = 0;\n"
        "  void FeedGamepad(ImGuiIO& io);\n",
        OPTIONAL,
    ),
    (
        "src/ui/imgui_drawer.cpp",
        "    pad = UIGamepad();\n  }\n  if (want) {\n",
        "    pad = UIGamepad();\n  }\n"
        "  // Buttons already held when a dialog opens (the A press that chose\n"
        "  // \"Exit\" in the game's menu) count only after being released, so\n"
        "  // they cannot answer the dialog before the player has seen it.\n"
        "  if (want && !gamepad_fed_) {\n"
        "    gamepad_mask_ = pad.buttons;\n"
        "  }\n"
        "  gamepad_mask_ &= pad.buttons;\n"
        "  pad.buttons &= static_cast<uint16_t>(~gamepad_mask_);\n"
        "  if (want) {\n",
        OPTIONAL,
    ),
]


# SDK change 16: controller support in the SDK's own dialogs (#18).
#
# System dialogs ("create a new save?", sign-in, text entry) and the F-key
# menus could only be used with mouse and keyboard. While a dialog that takes
# input is open, the game gets no controller input (change 14), so a player on
# a controller was stuck. Dialogs now get Dear ImGui's gamepad navigation:
# D-pad/left stick move between buttons, A presses, B goes back, LB/RB switch
# tabs. The controller is read through a separate path that does not change
# what the game sees once the dialog closes. Message boxes also start on the
# game's default button instead of the first one.
SDK_PATCHES += [
    (
        "include/rex/input/input_driver.h",
        "  virtual void OnWindowAvailable(rex::ui::Window* /*window*/) {}\n",
        "  virtual void OnWindowAvailable(rex::ui::Window* /*window*/) {}\n"
        "\n"
        "  // Controller state for the SDK's own dialogs: read even while the game's\n"
        "  // input is paused, without changing what the game sees afterwards.\n"
        "  virtual X_RESULT GetStateForUI(uint32_t /*user_index*/, X_INPUT_STATE* /*out_state*/) {\n"
        "    return X_ERROR_DEVICE_NOT_CONNECTED;\n"
        "  }\n",
    ),
    (
        "include/rex/input/sdl/sdl_input_driver.h",
        "  void OnWindowAvailable(rex::ui::Window* window) override;\n",
        "  void OnWindowAvailable(rex::ui::Window* window) override;\n"
        "  X_RESULT GetStateForUI(uint32_t user_index, X_INPUT_STATE* out_state) override;\n",
    ),
    (
        "src/input/sdl/sdl_input_driver.cpp",
        "X_RESULT SDLInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {\n",
        "X_RESULT SDLInputDriver::GetStateForUI(uint32_t user_index, X_INPUT_STATE* out_state) {\n"
        "  if (!sdl_events_initialized_ || !SDL_Gamepad_initialized_ ||\n"
        "      user_index >= HID_SDL_USER_COUNT) {\n"
        "    return X_ERROR_DEVICE_NOT_CONNECTED;\n"
        "  }\n"
        "  QueueControllerUpdate();\n"
        "  auto guard = DrainAndLock();\n"
        "  auto controller = GetControllerState(user_index);\n"
        "  if (!controller) {\n"
        "    return X_ERROR_DEVICE_NOT_CONNECTED;\n"
        "  }\n"
        "  // Leave packet_number/state_changed alone: they belong to the game.\n"
        "  std::memcpy(out_state, &controller->state, sizeof(*out_state));\n"
        "  return X_ERROR_SUCCESS;\n"
        "}\n"
        "\n"
        "X_RESULT SDLInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {\n",
    ),
    (
        "include/rex/input/xinput/xinput_input_driver.h",
        "  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;\n",
        "  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;\n"
        "  X_RESULT GetStateForUI(uint32_t user_index, X_INPUT_STATE* out_state) override {\n"
        "    return GetState(user_index, out_state);\n"
        "  }\n",
    ),
    (
        "include/rex/input/input_system.h",
        "  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);\n",
        "  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);\n"
        "  // All drivers' controllers for user_index, for the SDK's dialogs.\n"
        "  bool GetStateForUI(uint32_t user_index, X_INPUT_GAMEPAD* out_gamepad);\n",
    ),
    (
        "src/input/input_system.cpp",
        "X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {\n",
        "bool InputSystem::GetStateForUI(uint32_t user_index, X_INPUT_GAMEPAD* out_gamepad) {\n"
        "  bool connected = false;\n"
        "  uint16_t buttons = 0;\n"
        "  int16_t lx = 0, ly = 0;\n"
        "  for (auto& driver : drivers_) {\n"
        "    X_INPUT_STATE state = {};\n"
        "    if (driver->GetStateForUI(user_index, &state) != X_ERROR_SUCCESS) {\n"
        "      continue;\n"
        "    }\n"
        "    connected = true;\n"
        "    buttons |= static_cast<uint16_t>(state.gamepad.buttons);\n"
        "    int16_t sx = state.gamepad.thumb_lx, sy = state.gamepad.thumb_ly;\n"
        "    if (std::abs(int(sx)) > std::abs(int(lx))) lx = sx;\n"
        "    if (std::abs(int(sy)) > std::abs(int(ly))) ly = sy;\n"
        "  }\n"
        "  if (connected && out_gamepad) {\n"
        "    *out_gamepad = {};\n"
        "    out_gamepad->buttons = buttons;\n"
        "    out_gamepad->thumb_lx = lx;\n"
        "    out_gamepad->thumb_ly = ly;\n"
        "  }\n"
        "  return connected;\n"
        "}\n"
        "\n"
        "X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {\n",
    ),
    (
        "include/rex/ui/imgui_drawer.h",
        "  bool HasMouseDialogs() const { return mouse_dialog_count_.load() > 0; }\n",
        "  bool HasMouseDialogs() const { return mouse_dialog_count_.load() > 0; }\n"
        "\n"
        "  // Controller used to navigate dialogs that take input. The source fills in\n"
        "  // the XInput button bits and left stick; false if no controller.\n"
        "  struct UIGamepad {\n"
        "    uint16_t buttons = 0;\n"
        "    int16_t lx = 0;\n"
        "    int16_t ly = 0;\n"
        "  };\n"
        "  void SetGamepadSource(std::function<bool(UIGamepad*)> source) {\n"
        "    gamepad_source_ = std::move(source);\n"
        "  }\n",
    ),
    (
        "include/rex/ui/imgui_drawer.h",
        "  ImGuiContext* internal_state_ = nullptr;\n",
        "  ImGuiContext* internal_state_ = nullptr;\n"
        "  std::function<bool(UIGamepad*)> gamepad_source_;\n"
        "  bool gamepad_fed_ = false;\n"
        "  uint16_t gamepad_mask_ = 0;\n"
        "  void FeedGamepad(ImGuiIO& io);\n",
    ),
    (
        "src/ui/imgui_drawer.cpp",
        "#include <cfloat>\n",
        "#include <algorithm>\n#include <cfloat>\n#include <utility>\n",
    ),
    (
        "src/ui/rex_app.cpp",
        "#include <algorithm>\n#include <filesystem>\n",
        "#include <algorithm>\n#include <cstdlib>\n#include <filesystem>\n",
    ),
    (
        "src/ui/imgui_drawer.cpp",
        "  ImGui::NewFrame();\n",
        "  FeedGamepad(io);\n"
        "\n"
        "  ImGui::NewFrame();\n",
    ),
    (
        "src/ui/imgui_drawer.cpp",
        "void ImGuiDrawer::OnKeyDown(KeyEvent& e) {\n",
        "void ImGuiDrawer::FeedGamepad(ImGuiIO& io) {\n"
        "  // XInput button bit -> ImGui gamepad key (A = activate, B = back).\n"
        "  static const std::pair<uint16_t, ImGuiKey> kButtons[] = {\n"
        "      {0x0001, ImGuiKey_GamepadDpadUp},    {0x0002, ImGuiKey_GamepadDpadDown},\n"
        "      {0x0004, ImGuiKey_GamepadDpadLeft},  {0x0008, ImGuiKey_GamepadDpadRight},\n"
        "      {0x0010, ImGuiKey_GamepadStart},     {0x0020, ImGuiKey_GamepadBack},\n"
        "      {0x0100, ImGuiKey_GamepadL1},        {0x0200, ImGuiKey_GamepadR1},\n"
        "      {0x1000, ImGuiKey_GamepadFaceDown},  {0x2000, ImGuiKey_GamepadFaceRight},\n"
        "      {0x4000, ImGuiKey_GamepadFaceLeft},  {0x8000, ImGuiKey_GamepadFaceUp},\n"
        "  };\n"
        "  bool want = gamepad_source_ && mouse_dialog_count_.load() > 0;\n"
        "  if (!want && !gamepad_fed_) {\n"
        "    return;\n"
        "  }\n"
        "  UIGamepad pad;\n"
        "  bool connected = want && gamepad_source_(&pad);\n"
        "  if (!connected) {\n"
        "    pad = UIGamepad();\n"
        "  }\n"
        "  // Buttons already held when a dialog opens (the A press that chose\n"
        "  // \"Exit\" in the game's menu) count only after being released, so\n"
        "  // they cannot answer the dialog before the player has seen it.\n"
        "  if (want && !gamepad_fed_) {\n"
        "    gamepad_mask_ = pad.buttons;\n"
        "  }\n"
        "  gamepad_mask_ &= pad.buttons;\n"
        "  pad.buttons &= static_cast<uint16_t>(~gamepad_mask_);\n"
        "  if (want) {\n"
        "    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;\n"
        "  }\n"
        "  if (connected) {\n"
        "    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;\n"
        "  } else {\n"
        "    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;\n"
        "  }\n"
        "  for (const auto& [bit, key] : kButtons) {\n"
        "    io.AddKeyEvent(key, (pad.buttons & bit) != 0);\n"
        "  }\n"
        "  auto stick = [&io](ImGuiKey neg, ImGuiKey pos, int16_t value) {\n"
        "    constexpr float kDead = 8689.0f;  // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE\n"
        "    float v = float(value);\n"
        "    float n = v < -kDead ? std::min(1.0f, (-v - kDead) / (32768.0f - kDead)) : 0.0f;\n"
        "    float p = v > kDead ? std::min(1.0f, (v - kDead) / (32767.0f - kDead)) : 0.0f;\n"
        "    io.AddKeyAnalogEvent(neg, n > 0.1f, n);\n"
        "    io.AddKeyAnalogEvent(pos, p > 0.1f, p);\n"
        "  };\n"
        "  stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, pad.lx);\n"
        "  stick(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, pad.ly);\n"
        "  gamepad_fed_ = want;\n"
        "}\n"
        "\n"
        "void ImGuiDrawer::OnKeyDown(KeyEvent& e) {\n",
    ),
    (
        "src/ui/rex_app.cpp",
        "        return !imgui_drawer_->GetIO().WantCaptureMouse;\n      });\n",
        "        return !imgui_drawer_->GetIO().WantCaptureMouse;\n      });\n"
        "      imgui_drawer_->SetGamepadSource([input_sys](rex::ui::ImGuiDrawer::UIGamepad* pad) {\n"
        "        bool any = false;\n"
        "        for (uint32_t user = 0; user < 4; ++user) {\n"
        "          rex::input::X_INPUT_GAMEPAD gamepad = {};\n"
        "          if (!input_sys->GetStateForUI(user, &gamepad)) {\n"
        "            continue;\n"
        "          }\n"
        "          any = true;\n"
        "          pad->buttons |= static_cast<uint16_t>(gamepad.buttons);\n"
        "          int16_t lx = gamepad.thumb_lx, ly = gamepad.thumb_ly;\n"
        "          if (std::abs(int(lx)) > std::abs(int(pad->lx))) pad->lx = lx;\n"
        "          if (std::abs(int(ly)) > std::abs(int(pad->ly))) pad->ly = ly;\n"
        "        }\n"
        "        return any;\n"
        "      });\n",
    ),
    (
        "src/ui/rex_app.cpp",
        "  if (runtime_ && runtime_->input_system()) {\n    runtime_->input_system()->Shutdown();\n",
        "  if (imgui_drawer_) {\n    imgui_drawer_->SetGamepadSource(nullptr);\n  }\n"
        "  if (runtime_ && runtime_->input_system()) {\n    runtime_->input_system()->Shutdown();\n",
    ),
    (
        "src/kernel/xam/xam_ui.cpp",
        "      if (first_draw) {\n        ImGui::SetKeyboardFocusHere();\n      }\n"
        "      for (size_t i = 0; i < buttons_.size(); ++i) {\n"
        "        if (ImGui::Button(buttons_[i].c_str())) {\n",
        "      for (size_t i = 0; i < buttons_.size(); ++i) {\n"
        "        if (first_draw && (i == default_button_ ||\n"
        "                           (i == 0 && default_button_ >= buttons_.size()))) {\n"
        "          ImGui::SetKeyboardFocusHere();\n"
        "        }\n"
        "        if (ImGui::Button(buttons_[i].c_str())) {\n",
    ),
]


# SDK change 17: a hook to remap controller buttons (in-game Controls menu).
#
# The app can install a filter that rewrites each physical controller's state
# before the game sees it. Keyboard/mouse emulation is not filtered: it has
# its own key bindings.
SDK_PATCHES += [
    (
        "include/rex/input/input_driver.h",
        "  void set_is_active_callback(std::function<bool()> is_active_callback) {\n",
        "  // False for drivers that emulate a controller (keyboard/mouse).\n"
        "  virtual bool IsPhysicalController() const { return true; }\n\n"
        "  void set_is_active_callback(std::function<bool()> is_active_callback) {\n",
    ),
    (
        "include/rex/input/mnk/mnk_input_driver.h",
        "  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;\n",
        "  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;\n"
        "  bool IsPhysicalController() const override { return false; }\n",
    ),
    (
        "include/rex/input/input_system.h",
        "  void SetActiveCallback(std::function<bool()> callback);\n",
        "  void SetActiveCallback(std::function<bool()> callback);\n"
        "  // Rewrites each physical controller's state before the game sees it\n"
        "  // (button remapping). Set once at startup.\n"
        "  using GamepadFilter = std::function<void(uint32_t user_index, X_INPUT_GAMEPAD* gamepad)>;\n"
        "  void SetGamepadFilter(GamepadFilter filter) { gamepad_filter_ = std::move(filter); }\n",
    ),
    (
        "include/rex/input/input_system.h",
        "  std::vector<std::unique_ptr<InputDriver>> drivers_;\n",
        "  std::vector<std::unique_ptr<InputDriver>> drivers_;\n"
        "  GamepadFilter gamepad_filter_;\n",
    ),
    (
        "src/input/input_system.cpp",
        "    if (result == X_ERROR_SUCCESS) {\n      if (first_result) {\n",
        "    if (result == X_ERROR_SUCCESS) {\n"
        "      if (gamepad_filter_ && driver->IsPhysicalController()) {\n"
        "        gamepad_filter_(user_index, &state.gamepad);\n"
        "      }\n"
        "      if (first_result) {\n",
    ),
]


# SDK change 19: dialogs opened by the game could stay invisible.
#
# The game's system dialogs (e.g. "Any unsaved data will be lost. Are you
# sure?" when leaving a stage) are created on a game thread, and their
# constructor adds them to the ImGui drawer's dialog list while the UI thread
# is drawing that list (always, with the FPS counter on). The second such
# dialog in a session never appeared; the game waited for it and ignored all
# input. The drawer now adds dialogs on the UI thread only, and logs when a
# dialog is added or removed so a missing one shows up in the log.
SDK_PATCHES += [
    (
        "src/ui/imgui_drawer.cpp",
        "void ImGuiDrawer::AddDialog(ImGuiDialog* dialog) {\n  assert_not_null(dialog);\n",
        "void ImGuiDrawer::AddDialog(ImGuiDialog* dialog) {\n  assert_not_null(dialog);\n"
        "  // The game creates its system dialogs on its own threads; the list is\n"
        "  // only touched on the UI thread (the caller waits for the dialog through\n"
        "  // CallInUIThreadSynchronous, which runs after this queued call).\n"
        "  if (!window_->app_context().IsInUIThread()) {\n"
        "    window_->app_context().CallInUIThread([this, dialog]() { AddDialog(dialog); });\n"
        "    return;\n"
        "  }\n"
        "  REXLOG_INFO(\"ImGui dialog opened ({} open)\", dialogs_.size() + 1);\n",
    ),
    (
        "src/ui/imgui_drawer.cpp",
        "  dialogs_.erase(it);\n  DetachIfLastDialogRemoved();\n",
        "  dialogs_.erase(it);\n"
        "  REXLOG_INFO(\"ImGui dialog closed ({} open)\", dialogs_.size());\n"
        "  DetachIfLastDialogRemoved();\n",
    ),
]


# SDK change 20: the game's dialogs could close unseen and hang the game.
#
# Leaving a stage twice through the pause menu: the second "Any unsaved data
# will be lost" box closed itself on its first frame (Dear ImGui did not open
# the popup again under the same name) before the waiting thread had
# registered for the result. The game then waited forever with input blocked,
# and closing the window left the process running.
# - Every game dialog gets its own popup ID.
# - A message box that did not open tries again instead of closing.
# - A game dialog that closes before anyone waits for it stays until the
#   waiter registers, so the result is always delivered.
# - ~ImGuiDialog is virtual (dialogs delete themselves through the base).
SDK_PATCHES += [
    (
        "include/rex/ui/imgui_dialog.h",
        " public:\n  ~ImGuiDialog();\n",
        " public:\n  virtual ~ImGuiDialog();\n",
    ),
    (
        "include/rex/ui/imgui_dialog.h",
        "  // Closes the dialog and returns to any waiters.\n  void Close();\n",
        "  // Closes the dialog and returns to any waiters.\n  void Close();\n"
        "  // The dialog is only deleted after a waiter registered with Then().\n"
        "  void set_close_waits_for_fence(bool waits) { close_waits_for_fence_ = waits; }\n",
    ),
    (
        "include/rex/ui/imgui_dialog.h",
        "  bool has_close_pending_ = false;\n",
        "  bool has_close_pending_ = false;\n  bool close_waits_for_fence_ = false;\n",
    ),
    (
        "src/ui/imgui_dialog.cpp",
        "  // Draw UI.\n  OnDraw(GetIO());\n\n"
        "  // Check to see if the UI closed itself and needs to be deleted.\n"
        "  if (has_close_pending_) {\n",
        "  // Draw UI.\n  if (!has_close_pending_) {\n    OnDraw(GetIO());\n  }\n\n"
        "  // Check to see if the UI closed itself and needs to be deleted. A dialog\n"
        "  // someone waits for stays until the waiter has registered.\n"
        "  if (has_close_pending_ && (!close_waits_for_fence_ || !waiting_fences_.empty())) {\n",
    ),
    (
        "src/kernel/xam/xam_ui.cpp",
        "  XamDialog(rex::ui::ImGuiDrawer* imgui_drawer) : rex::ui::ImGuiDialog(imgui_drawer) {}\n",
        "  XamDialog(rex::ui::ImGuiDrawer* imgui_drawer) : rex::ui::ImGuiDialog(imgui_drawer) {\n"
        "    set_close_waits_for_fence(true);\n"
        "  }\n"
        "\n"
        "  // Own popup ID per dialog: reusing one name made Dear ImGui skip the\n"
        "  // second popup. \"###\" keeps the visible title unchanged.\n"
        "  static std::string UniquePopupSuffix() {\n"
        "    static std::atomic<uint32_t> counter{0};\n"
        "    return \"###xam_dialog_\" + std::to_string(++counter);\n"
        "  }\n",
    ),
    (
        "src/kernel/xam/xam_ui.cpp",
        "#include <imgui.h>\n",
        "#include <imgui.h>\n\n#include <atomic>\n#include <string>\n",
    ),
    (
        "src/kernel/xam/xam_ui.cpp",
        "    if (!title_.size()) {\n      title_ = \"Message Box\";\n    }\n",
        "    if (!title_.size()) {\n      title_ = \"Message Box\";\n    }\n"
        "    title_ += UniquePopupSuffix();\n",
    ),
    (
        "src/kernel/xam/xam_ui.cpp",
        "    text_ = default_text;\n    text_buffer_.resize(max_length);\n",
        "    title_ += UniquePopupSuffix();\n"
        "    text_ = default_text;\n    text_buffer_.resize(max_length);\n",
    ),
    (
        "src/kernel/xam/xam_ui.cpp",
        "      if (description_.size()) {\n        ImGui::Text(\"%s\", description_.c_str());\n      }\n",
        "      shown_ = true;\n"
        "      if (description_.size()) {\n        ImGui::Text(\"%s\", description_.c_str());\n      }\n",
    ),
    (
        "src/kernel/xam/xam_ui.cpp",
        "      ImGui::Spacing();\n      ImGui::Spacing();\n      ImGui::EndPopup();\n"
        "    } else {\n      Close();\n    }\n",
        "      ImGui::Spacing();\n      ImGui::Spacing();\n      ImGui::EndPopup();\n"
        "    } else if (shown_) {\n      Close();\n    } else {\n"
        "      // The popup did not open (yet): try again next frame instead of\n"
        "      // answering for the player.\n"
        "      if (!retry_logged_) {\n"
        "        REXLOG_WARN(\"Message box '{}' did not open, retrying\", title_);\n"
        "        retry_logged_ = true;\n"
        "      }\n"
        "      has_opened_ = false;\n"
        "    }\n",
    ),
    (
        "src/kernel/xam/xam_ui.cpp",
        "  bool has_opened_ = false;\n  std::string title_;\n  std::string description_;\n"
        "  std::vector<std::string> buttons_;\n",
        "  bool has_opened_ = false;\n  bool shown_ = false;\n  bool retry_logged_ = false;\n"
        "  std::string title_;\n  std::string description_;\n"
        "  std::vector<std::string> buttons_;\n",
    ),
]


# SDK change 21: the A press that chose "Exit" answered the next dialog.
#
# Change 16 now ignores controller buttons that are already held when a dialog
# opens. With the FPS counter off the drawer detaches between dialogs and its
# draw loop stops, so the gamepad state is reset there too.
SDK_PATCHES += [
    (
        "src/ui/imgui_drawer.cpp",
        "  // properties.\n  ClearInput();\n",
        "  // properties.\n  ClearInput();\n  gamepad_fed_ = false;\n",
    ),
]


# SDK change 22: the game's language follows the user_language setting (#19).
#
# XGetLanguage always answered English (a Xenia TODO), so games that ask it
# ignored user_language, which XConfig already reports. It now returns the
# setting (1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian,
# ...), falling back to the old region rule for invalid values.
SDK_PATCHES += [
    (
        "src/kernel/xam/xam_info.cpp",
        "u32 XGetLanguage_entry() {\n  auto desired_language = XLanguage::kEnglish;\n",
        "u32 XGetLanguage_entry() {\n"
        "  // The user_language setting decides (XConfig reports the same value).\n"
        "  const uint32_t configured = REXCVAR_GET(user_language);\n"
        "  if (configured > 0 && configured < uint32_t(XLanguage::kMaxLanguages)) {\n"
        "    return configured;\n"
        "  }\n"
        "  auto desired_language = XLanguage::kEnglish;\n",
    ),
    (
        "src/kernel/xam/xam_info.cpp",
        "#include <rex/system/kernel_state.h>\n",
        "#include <rex/system/flags.h>\n#include <rex/system/kernel_state.h>\n",
    ),
]


def sdk_patch_hash() -> str:
    h = hashlib.sha256()
    for rel, old, new, *_ in SDK_PATCHES:
        h.update(rel.encode() + b"\0" + old.encode() + b"\0" + new.encode() + b"\0")
    return h.hexdigest()[:16]


def apply_sdk_patches() -> None:
    applied = 0
    for rel, old, new, *flags in SDK_PATCHES:
        path = SDK_DIR / rel
        data = path.read_bytes().decode("utf-8")
        crlf = "\r\n" in data
        o = old.replace("\n", "\r\n") if crlf else old
        n = new.replace("\n", "\r\n") if crlf else new
        if n in data:
            continue  # already applied
        if data.count(o) != 1:
            if OPTIONAL in flags:
                continue
            sys.exit(f"error: SDK patch does not fit {rel}; is the SDK at the commit in sdk-pin.txt?")
        path.write_bytes(data.replace(o, n, 1).encode("utf-8"))
        applied += 1
    if applied:
        print(f"      applied {applied} local SDK change(s) (see SDK_PATCHES in build.py)")

def step_clone_sdk() -> None:
    url, commit = read_pin()
    if (SDK_DIR / "CMakeLists.txt").exists():
        print(f"[2/5] SDK already at {SDK_DIR}, skipping clone")
        n = resolve_symlink_placeholders(SDK_DIR)
        if n:
            print(f"      replaced {n} symlink placeholder file(s) with real copies")
        apply_sdk_patches()
        return
    print(f"[2/5] Cloning ReXGlue SDK {commit[:12]}")
    SDK_DIR.mkdir(parents=True, exist_ok=True)
    run(git("init", "-q"), cwd=SDK_DIR)
    run(git("remote", "add", "origin", url), cwd=SDK_DIR)
    run(git("fetch", "--depth", "1", "origin", commit), cwd=SDK_DIR)
    run(git("checkout", "-q", "FETCH_HEAD"), cwd=SDK_DIR)
    run(git("submodule", "update", "--init", "--depth", "1", "--jobs", "8"), cwd=SDK_DIR)
    n = resolve_symlink_placeholders(SDK_DIR)
    if n:
        print(f"      replaced {n} symlink placeholder file(s) with real copies")
    apply_sdk_patches()


def sdk_install_dir(pid: str) -> Path:
    return SDK_DIR / "out" / "install" / pid


def find_rexglue(pid: str) -> Path | None:
    exe = "rexglue.exe" if platform.system() == "Windows" else "rexglue"
    for base in (sdk_install_dir(pid), SDK_DIR / "out" / pid):
        for candidate in base.rglob(exe):
            if candidate.is_file():
                return candidate
    return None


def stale_cmake_cache(build_dir: Path) -> bool:
    """True if build_dir was configured for a different SDK source folder."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return False
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_HOME_DIRECTORY:"):
            recorded = line.split("=", 1)[1].strip()

            def norm(p) -> str:
                return os.path.normcase(os.path.normpath(str(p)))

            return norm(recorded) != norm(SDK_DIR.resolve())
    return False


def reembed_codegen_templates(build_dir: Path) -> None:
    """Regenerate the codegen templates compiled into the rexglue tool.

    The SDK embeds resources/templates/**/*.inja into C++ headers through a
    custom command that Ninja Multi-Config attaches to a single configuration,
    so an incremental build of another configuration can keep stale templates.
    Running the embed script directly refreshes them; the compiler's header
    dependencies then rebuild the code that includes them.
    """
    out_dir = build_dir / "src" / "codegen" / "generated" / "templates"
    if not out_dir.is_dir():
        return  # fresh build: the custom command generates them
    run(["cmake",
         f"-DTEMPLATE_SOURCE_DIR={SDK_DIR / 'resources' / 'templates'}",
         f"-DTEMPLATE_OUTPUT_DIR={out_dir}",
         "-P", SDK_DIR / "cmake" / "embed_templates.cmake"], cwd=SDK_DIR)


def step_build_sdk(pid: str, config: str) -> Path:
    tool = find_rexglue(pid)
    stamp_ok = PATCH_STAMP.exists() and PATCH_STAMP.read_text().strip() == sdk_patch_hash()
    if tool and (sdk_install_dir(pid) / "lib").exists() and stamp_ok:
        print(f"[3/5] SDK already built ({tool}), skipping")
        return tool
    build_dir = SDK_DIR / "out" / "build" / pid
    if stale_cmake_cache(build_dir):
        # CMake remembers the absolute source path; after the SDK folder was
        # moved, the old build tree cannot be reused and must be rebuilt.
        print(f"[3/5] SDK build folder belongs to an old location, removing {build_dir}")
        shutil.rmtree(build_dir)
        tool = None
    if tool:
        print("[3/5] SDK changed, rebuilding (incremental, only the changed parts)")
    else:
        print("[3/5] Building and installing the ReXGlue SDK (this takes a while)")
    # The SDK uses one multi-config configure preset per platform (e.g.
    # "win-amd64") plus build presets "<platform>-<config>".
    run(["cmake", "--preset", pid], cwd=SDK_DIR)
    reembed_codegen_templates(build_dir)
    run(["cmake", "--build", "--preset", f"{pid}-{config.lower()}"], cwd=SDK_DIR)
    run(["cmake", "--install", f"out/build/{pid}", "--config", config], cwd=SDK_DIR)
    PATCH_STAMP.write_text(sdk_patch_hash() + "\n")
    tool = find_rexglue(pid)
    if tool is None:
        sys.exit("error: SDK built, but the rexglue tool was not found")
    return tool


CODEGEN_STAMP = ROOT / "generated" / ".harmonyrecomp-codegen"


def codegen_inputs_hash() -> str:
    """Hash of everything codegen output depends on besides the game binary:
    the manifest, every config file it includes, the SDK changes and the
    title update."""
    h = hashlib.sha256(sdk_patch_hash().encode())
    files = [MANIFEST, *sorted(ROOT.glob("harmonyrecomp_*.toml"))]  # includes the local manifest
    for path in dict.fromkeys(files):
        h.update(path.name.encode() + b"\0" + path.read_bytes() + b"\0")
    h.update((TU_STATE.read_text() if TU_STATE.exists() else "none").encode())
    return h.hexdigest()[:16]


def check_codegen_output() -> None:
    """Every DLL module must report its table address (SDK change 5), or the
    loader and the DLL's own code would disagree about where the table is."""
    for manifest_line in active_manifest().read_text(encoding="utf-8").splitlines():
        line = manifest_line.strip()
        if not line.startswith('out_directory_path = "generated/') or line.endswith('/default"'):
            continue
        out_dir = ROOT / line.split('"')[1]
        register = out_dir / "harmonyrecomp_register.cpp"
        text = register.read_text(encoding="utf-8") if register.exists() else ""
        if "ReXModule_TableBase" not in text:
            sys.exit(
                f"error: {register.relative_to(ROOT)} lacks ReXModule_TableBase.\n"
                "  The rexglue tool still uses old codegen templates. Delete the folder\n"
                f"  {SDK_DIR / 'out'} and run build.py again for a clean SDK build."
            )


def run_codegen(tool: Path) -> tuple[int, str]:
    """Run codegen, show its output live and return (exit code, output)."""
    args = [str(tool), "codegen", str(active_manifest())]
    print("+ " + " ".join(args), flush=True)
    # Title update patches (default.xexp, *.dllp next to the binaries) are
    # applied when codegen loads the modules, as the game does at runtime.
    env = dict(os.environ, REX_XEX_APPLY_PATCHES="true")
    proc = subprocess.Popen(args, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace")
    lines = []
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        lines.append(line)
    return proc.wait(), "".join(lines)


# Windows status codes of a program that could not start at all.
START_FAILURES = {
    0xC0000135: "a DLL it needs was not found",
    0xC0000142: "a DLL it needs failed to initialize",
    0xC000007B: "a DLL it needs is for the wrong architecture",
}


def codegen_failed(code: int) -> None:
    status = code & 0xFFFFFFFF
    if status in START_FAILURES:
        sys.exit(f"error: the rexglue tool could not start ({START_FAILURES[status]}, "
                 f"0x{status:08X}). Usually the Microsoft Visual C++ Redistributable is missing "
                 "or old: install the latest x64 version from "
                 "https://aka.ms/vs/17/release/vc_redist.x64.exe and run the build again.")
    sys.exit(f"error: codegen failed with exit code {code}")


def dlc_package_of_module(name: str) -> str | None:
    """DLC package (file name) a failed module belongs to, from the manifest."""
    text = active_manifest().read_text(encoding="utf-8") if LOCAL_MANIFEST.exists() else ""
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('file_path = "assets/dlc/'):
            path = line.split('"')[1]
            if Path(path).stem.lower() == name.lower():
                return path.split("/")[2]
    return None


def step_codegen(tool: Path, force: bool, dlc_dir: Path | None, game: Path | None) -> None:
    done = (ROOT / "generated" / "default" / "sources.cmake").exists()
    current = codegen_inputs_hash()
    up_to_date = (done and CODEGEN_STAMP.exists()
                  and CODEGEN_STAMP.read_text().strip() == current)
    if up_to_date and not force:
        print("[4/5] Codegen output up to date, skipping (use --force-codegen to redo)")
        check_codegen_output()
        return
    if done and not force:
        print("[4/5] Codegen config or SDK changed, running codegen again")
    else:
        print("[4/5] Running codegen (2 to 10 minutes, longer with all DLC)")
    progress("codegen")
    while True:
        code, output = run_codegen(tool)
        if code == 0:
            break
        # A DLC module the recompiler cannot handle must not stop the whole
        # build: leave its package out (the game then does not install it)
        # and try again. Failures in the base game still stop here.
        failed = re.findall(r"Analysis failed for '([^']+)'", output)
        packages = {dlc_package_of_module(name) for name in failed}
        if not failed or None in packages:
            codegen_failed(code)
        skipped = read_skipped()
        if packages <= skipped:
            codegen_failed(code)
        skipped |= packages
        write_skipped(skipped)
        for package in sorted(packages):
            print(f"\nwarning: codegen failed for module(s) {', '.join(failed)} of DLC package "
                  f"{package}; building without that package", flush=True)
        step_dlc(dlc_dir, game)
    check_codegen_output()
    CODEGEN_STAMP.write_text(codegen_inputs_hash() + "\n")


def fix_future_mtimes(*folders: Path) -> None:
    """Set file times that lie in the future to now.

    CMake re-runs whenever an input is newer than its build files; an input
    dated in the future (unpacked from an archive made in another time zone,
    or a wrong clock) makes it re-run forever: "build.ninja still dirty after
    100 tries"."""
    now = time.time()
    fixed = 0
    for folder in folders:
        if not folder.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(folder):
            dirnames[:] = [d for d in dirnames if d not in (".git", "out", "generated", "assets")]
            for name in filenames:
                path = os.path.join(dirpath, name)
                try:
                    if os.stat(path).st_mtime > now + 60:
                        os.utime(path, (now, now))
                        fixed += 1
                except OSError:
                    pass
    if fixed:
        print(f"      reset {fixed} file time(s) that were in the future")


def step_build_game(pid: str, config: str, prefix: Path, jobs: int | None) -> Path:
    preset = f"{pid}-{config.lower()}"
    print(f"[5/5] Building the game ({preset})")
    progress("compile")
    fix_future_mtimes(ROOT, prefix)
    run(["cmake", "--preset", preset, f"-DCMAKE_PREFIX_PATH={prefix}"], cwd=ROOT)
    build = ["cmake", "--build", f"out/build/{preset}"]
    if jobs:
        build += ["--parallel", str(jobs)]
    run(build, cwd=ROOT)
    out_dir = ROOT / "out" / "build" / preset
    runtime_cfg = out_dir / "harmonyrecomp.toml"
    if not runtime_cfg.exists():
        # Runtime settings read at startup (e.g. license_mask). Copied only
        # once, so your own edits next to the exe are kept.
        shutil.copy2(ROOT / "harmonyrecomp.toml", runtime_cfg)
        print(f"      wrote {runtime_cfg}")
    # Which DLC packages the game may install (src/dlc.cpp).
    (out_dir / DLC_LIST_NAME).write_text(DLC_LIST.read_text() if DLC_LIST.exists() else "")
    print(f"\nDone. Output is in {out_dir}")
    return out_dir


RUNTIME_FILES = ("*.exe", "*.dll", "*.map")


# Microsoft's C++ runtime, shipped in the prebuilt SDK's bin/ (package_release.py).
VC_RUNTIME = ("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
              "msvcp140_codecvt_ids.dll", "vcruntime140.dll", "vcruntime140_1.dll",
              "concrt140.dll")


def step_install(out_dir: Path, target: Path, dlc_dir: Path | None,
                 runtime_dir: Path | None = None) -> None:
    """Put the playable game into target: executable, modules, settings, the
    extracted game data and the DLC packages (installed at first start)."""
    print(f"[6/6] Installing into {target}")
    progress("install")
    target.mkdir(parents=True, exist_ok=True)
    for pattern in RUNTIME_FILES:
        for src in out_dir.glob(pattern):
            shutil.copy2(src, target / src.name)
    if runtime_dir:
        # Built with the portable Microsoft components: the PC may have no (or
        # an old) Visual C++ Redistributable, so the runtime goes next to the exe.
        copied = [n for n in VC_RUNTIME if (runtime_dir / n).is_file()]
        for name in copied:
            shutil.copy2(runtime_dir / name, target / name)
        if copied:
            print(f"      C++ runtime next to the exe: {', '.join(copied)}")
    settings = target / "harmonyrecomp.toml"
    if not settings.exists():  # keep the player's settings on updates
        shutil.copy2(out_dir / "harmonyrecomp.toml", settings)
    assets = ROOT / "assets"
    target_assets = target / "assets"
    if assets.resolve() != target_assets.resolve():
        if target_assets.exists():
            shutil.rmtree(target_assets)
        shutil.copytree(assets, target_assets, ignore=shutil.ignore_patterns("dlc"))
    shutil.copy2(out_dir / DLC_LIST_NAME, target / DLC_LIST_NAME)
    built = set((out_dir / DLC_LIST_NAME).read_text().split())
    packages = [p for p in package_files(dlc_dir or DLC_DIR) if p.name in built]
    if packages:
        (target / "dlc").mkdir(exist_ok=True)
        for package in packages:
            shutil.copy2(package, target / "dlc" / package.name)
    print(f"      done: start {target / 'harmonyrecomp.exe'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--debug", action="store_true", help="debug instead of release build")
    parser.add_argument("--codegen-only", action="store_true", help="stop after codegen")
    parser.add_argument("--force-codegen", action="store_true", help="rerun codegen")
    parser.add_argument("--game", type=Path, help="game package (default: the file in game/)")
    parser.add_argument("--dlc-dir", type=Path, help="folder with DLC packages (default: dlc/)")
    parser.add_argument("--sdk-dir", type=Path,
                        help="prebuilt SDK install folder instead of building ../rexglue-sdk")
    parser.add_argument("--sdk-only", action="store_true", help="build the SDK, then stop")
    parser.add_argument("--install-to", type=Path, help="put the playable game into this folder")
    parser.add_argument("--jobs", type=int, help="parallel compile jobs")
    parser.add_argument("--app-local-runtime", action="store_true",
                        help="copy the C++ runtime from the SDK's bin/ next to the installed exe")
    args = parser.parse_args()

    pid = platform_id()
    config = "Debug" if args.debug else "Release"

    if args.sdk_only:
        check_tools(need_git=True)
        progress("sdk")
        step_clone_sdk()
        step_build_sdk(pid, config)
        return 0

    step_extract(args.game)  # checks the game file first: the clearest error for a wrong file
    step_dlc(args.dlc_dir, args.game)
    check_tools(need_git=args.sdk_dir is None)
    if args.sdk_dir:
        prefix = args.sdk_dir.resolve()
        tool = next((p for p in prefix.rglob("rexglue.exe" if platform.system() == "Windows"
                                             else "rexglue") if p.is_file()), None)
        if tool is None:
            sys.exit(f"error: no rexglue tool in {prefix}")
        print(f"[2/5] Using prebuilt SDK {prefix}")
        print("[3/5] (prebuilt SDK, nothing to build)")
    else:
        step_clone_sdk()
        tool = step_build_sdk(pid, config)
        prefix = sdk_install_dir(pid)
    step_codegen(tool, args.force_codegen, args.dlc_dir, args.game)
    if args.codegen_only:
        return 0
    out_dir = step_build_game(pid, config, prefix, args.jobs)
    if args.install_to:
        runtime_dir = tool.parent if args.app_local_runtime else None
        step_install(out_dir, args.install_to.resolve(), args.dlc_dir, runtime_dir)
    progress("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
