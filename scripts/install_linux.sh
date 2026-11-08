#!/usr/bin/env bash
# HarmonyRecomp for Linux and Steam Deck - builds the game from YOUR game file.
#
#   ./harmonyrecomp-install.sh                 asks for everything
#   ./harmonyrecomp-install.sh --game FILE [--dlc-dir DIR] [--dir INSTALL_DIR]
#
# Downloads the HarmonyRecomp build package and a self-contained compiler
# toolchain (clang, CMake, Ninja and the system headers it builds against, so
# nothing has to be installed, also not on SteamOS), translates the game and
# installs it into ~/Games/HarmonyRecomp. Nothing of the game is downloaded:
# the game file comes from your Xbox 360.
#
# Running it again with a newer version updates the game; settings and saves
# (in ~/.local/share/harmonyrecomp) are kept.
set -euo pipefail

REPO="Wrexxial/HarmonyRecomp"
TAG="__RELEASE_TAG__"            # set by the release workflow
[ "$TAG" = "__RELEASE_TAG__" ] && TAG="${HARMONYRECOMP_TAG:-latest}"

INSTALL_DIR="$HOME/Games/HarmonyRecomp"
GAME=""
DLC_DIR=""
JOBS="$(nproc 2>/dev/null || echo 4)"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/harmonyrecomp"

while [ $# -gt 0 ]; do
  case "$1" in
    --game) GAME="$2"; shift 2 ;;
    --dlc-dir) DLC_DIR="$2"; shift 2 ;;
    --dir) INSTALL_DIR="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 1 ;;
  esac
done

have() { command -v "$1" >/dev/null 2>&1; }
gui() { have zenity && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; }
say() { echo; echo "==> $*"; }
fail() {
  echo "error: $*" >&2
  gui && zenity --error --title="HarmonyRecomp" --text="$*" 2>/dev/null || true
  exit 1
}

have python3 || fail "python3 is needed (it is part of SteamOS and of every desktop distribution)."
have curl || fail "curl is needed."
have tar || fail "tar is needed."

# --- What to build from ------------------------------------------------------
if [ -z "$GAME" ]; then
  if gui; then
    GAME="$(zenity --file-selection --title='Your Harmony of Despair game file (58410A7A/000D0000/...)' 2>/dev/null)" || exit 1
  else
    read -r -p "Path of your game file (from 58410A7A/000D0000): " GAME
  fi
fi
[ -f "$GAME" ] || fail "game file not found: $GAME"
if [ -z "$DLC_DIR" ]; then
  if gui; then
    if zenity --question --title="HarmonyRecomp" --text="Do you have DLC (and the title update) to include?" 2>/dev/null; then
      DLC_DIR="$(zenity --file-selection --directory --title='Folder with your DLC packages' 2>/dev/null)" || DLC_DIR=""
    fi
  else
    read -r -p "Folder with your DLC packages (empty: none): " DLC_DIR
  fi
fi
[ -z "$DLC_DIR" ] || [ -d "$DLC_DIR" ] || fail "DLC folder not found: $DLC_DIR"

# --- Downloads ----------------------------------------------------------------
if [ "$TAG" = "latest" ]; then
  BASE="https://github.com/$REPO/releases/latest/download"
else
  BASE="https://github.com/$REPO/releases/download/$TAG"
fi
mkdir -p "$CACHE"
HERE="$(cd "$(dirname "$0")" && pwd)"
fetch() {
  local name="$1"
  # Offline install: the archives next to this script are used as they are.
  if [ -f "$HERE/$name" ]; then
    echo "using $HERE/$name"
    cp "$HERE/$name" "$CACHE/$name"
    return
  fi
  # Reused only if it is the same file as on the release now: a file of a
  # release can be replaced with a fixed one (same tag), which changes where
  # GitHub redirects the download to.
  local id
  id="$(curl -fsI -o /dev/null -w '%{redirect_url}' "$BASE/$name" 2>/dev/null | sed 's/[?].*//')"
  if [ -f "$CACHE/$name" ] && [ -n "$id" ] && [ "$(cat "$CACHE/$name.ok" 2>/dev/null)" = "$TAG $id" ]; then
    echo "already downloaded: $name"
    return
  fi
  say "Downloading $name"
  curl -fL --retry 3 -o "$CACHE/$name.part" "$BASE/$name" || fail "download of $name failed"
  mv "$CACHE/$name.part" "$CACHE/$name"
  echo "$TAG $id" > "$CACHE/$name.ok"
}
fetch harmonyrecomp-linux-package.tar.gz
fetch harmonyrecomp-linux-toolchain.tar.xz

WORK="$CACHE/work"
rm -rf "$WORK"
mkdir -p "$WORK"
say "Unpacking"
tar -xzf "$CACHE/harmonyrecomp-linux-package.tar.gz" -C "$WORK"
tar -xJf "$CACHE/harmonyrecomp-linux-toolchain.tar.xz" -C "$WORK"

TC="$WORK/toolchain"
export PATH="$TC/llvm/bin:$TC/cmake/bin:$TC/ninja:$PATH"
# The toolchain's own system headers and libraries (glibc, libstdc++, GTK):
# the build does not depend on what is installed (SteamOS has no headers).
if [ -d "$TC/sysroot" ]; then
  export HARMONYRECOMP_SYSROOT="$TC/sysroot"
fi

# --- Build and install -----------------------------------------------------------
say "Building (10 to 40 minutes; the Steam Deck takes about 30)"
args=(--game "$GAME" --sdk-dir "$WORK/sdk" --install-to "$INSTALL_DIR" --jobs "$JOBS")
[ -n "$DLC_DIR" ] && args+=(--dlc-dir "$DLC_DIR")
if ! ( cd "$WORK/src" && python3 -u scripts/build.py "${args[@]}" ) 2>&1 | tee "$CACHE/build.log"; then
  fail "the build failed; the log is $CACHE/build.log"
fi
[ -x "$INSTALL_DIR/harmonyrecomp" ] || fail "the build finished, but $INSTALL_DIR/harmonyrecomp is missing"

# --- Launcher ---------------------------------------------------------------------
cat > "$INSTALL_DIR/harmonyrecomp.sh" <<EOF
#!/usr/bin/env bash
cd "$INSTALL_DIR" && exec ./harmonyrecomp "\$@"
EOF
chmod +x "$INSTALL_DIR/harmonyrecomp.sh"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$APPS"
cat > "$APPS/harmonyrecomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Castlevania: Harmony of Despair (HarmonyRecomp)
Exec=$INSTALL_DIR/harmonyrecomp.sh
Path=$INSTALL_DIR
Terminal=false
Categories=Game;
EOF

rm -rf "$WORK"
say "Done: $INSTALL_DIR/harmonyrecomp.sh"
echo "It is in your application menu. Steam Deck Game Mode: in Desktop Mode, Steam >"
echo "Games > Add a Non-Steam Game to My Library, and pick HarmonyRecomp."
gui && zenity --info --title="HarmonyRecomp" --text="Installed in $INSTALL_DIR.\n\nSteam Deck Game Mode: in Desktop Mode open Steam, Games > Add a Non-Steam Game, and pick HarmonyRecomp." 2>/dev/null || true
