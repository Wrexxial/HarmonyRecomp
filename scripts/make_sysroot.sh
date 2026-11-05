#!/usr/bin/env bash
# Builds the Linux sysroot that ships in harmonyrecomp-linux-toolchain.tar.xz
# (run by the release workflow on Ubuntu 24.04).
#
# The game is compiled on the player's machine. SteamOS (Steam Deck) has no
# C/C++ headers and no development files, so the toolchain brings its own:
# glibc, libstdc++ and GTK 3 (the SDK's window code) headers plus the
# libraries to link against, taken from this Ubuntu. The game then needs
# glibc 2.39 or newer at runtime (SteamOS 3.6+, Arch, Fedora 40+, Ubuntu 24.04+).
#
#   scripts/make_sysroot.sh OUT_DIR
set -euo pipefail
OUT="${1:?usage: make_sysroot.sh OUT_DIR}"
GCC_MAJOR="$(gcc -dumpversion | cut -d. -f1)"
ROOT_PACKAGES=(libc6-dev linux-libc-dev "libstdc++-${GCC_MAJOR}-dev" "libgcc-${GCC_MAJOR}-dev"
               libgtk-3-dev libx11-xcb-dev)

if [ -z "${SKIP_APT:-}" ]; then
  sudo apt-get install -y --no-install-recommends "${ROOT_PACKAGES[@]}" >/dev/null
fi

# The packages and everything they depend on that is installed.
mapfile -t packages < <(
  apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks \
    --no-replaces --no-enhances "${ROOT_PACKAGES[@]}" 2>/dev/null |
  grep -E '^[a-z0-9]' | sed 's/:amd64$//' | sort -u |
  while read -r p; do dpkg -s "$p" >/dev/null 2>&1 && echo "$p"; done)
echo "sysroot: ${#packages[@]} packages"

mkdir -p "$OUT"
for p in "${packages[@]}"; do
  dpkg -L "$p" 2>/dev/null
done | sort -u | while read -r f; do
  case "$f" in
    /usr/include/*|/usr/lib/x86_64-linux-gnu/*|/usr/lib/gcc/x86_64-linux-gnu/*|\
    /usr/share/pkgconfig/*|/lib/x86_64-linux-gnu/*|/lib64/*) ;;
    *) continue ;;
  esac
  case "$f" in
    */gconv/*|*/perl*|*/python3*|*/girepository*|*/gdk-pixbuf-2.0/*/loaders/*|*.la) continue ;;
  esac
  # Merged /usr: /lib/... is /usr/lib/..., the sysroot keeps the files there
  # only and links /lib to it below.
  dest="$f"
  case "$f" in /lib/*|/lib64/*) dest="/usr$f" ;; esac
  if [ -L "$f" ] || [ -f "$f" ]; then
    mkdir -p "$OUT$(dirname "$dest")"
    cp -a "$f" "$OUT$dest"
  fi
done

# Static libraries are not linked, except the small parts of the C and C++
# runtimes that every program links statically. Big shared libraries that
# GTK only pulls in indirectly (Mesa, LLVM, ICU, sanitizers) are not needed
# to link either: the linker does not look at libraries' own dependencies.
find "$OUT" \( -name '*.a' -o -name 'libLLVM*' -o -name 'libgallium*' -o -name 'libicu*' \
  -o -name 'lib[aht]san.so*' -o -name 'libubsan.so*' -o -name 'liblsan.so*' \) | while read -r f; do
  case "$(basename "$f")" in
    libc_nonshared.a|libgcc.a|libgcc_eh.a|libssp_nonshared.a|libmvec.a|libstdc++.a|\
    libstdc++fs.a|libsupc++.a|libatomic.a) ;;
    *) rm -f "$f" ;;
  esac
done
rm -rf "$OUT/usr/lib/x86_64-linux-gnu/systemd" "$OUT/usr/lib/x86_64-linux-gnu/dri"

# Absolute symlinks would point into the build machine: make them relative.
find "$OUT" -type l | while read -r link; do
  target="$(readlink "$link")"
  case "$target" in
    /*) ln -sfn "$(realpath -m --relative-to="$(dirname "$link")" "$OUT$target")" "$link" ;;
  esac
done

# Ubuntu's merged /usr: /lib is /usr/lib.
ln -sfn usr/lib "$OUT/lib"
mkdir -p "$OUT/usr/lib64"
ln -sfn usr/lib64 "$OUT/lib64"
[ -e "$OUT/usr/lib64/ld-linux-x86-64.so.2" ] ||
  ln -sfn ../lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$OUT/usr/lib64/ld-linux-x86-64.so.2"

# GTK's compiler and linker flags, so the build needs no pkg-config: include
# folders relative to the sysroot, then the libraries (build.py reads this).
export PKG_CONFIG_SYSROOT_DIR="$OUT"
export PKG_CONFIG_LIBDIR="$OUT/usr/lib/x86_64-linux-gnu/pkgconfig:$OUT/usr/share/pkgconfig"
{
  echo "includes $(pkg-config --cflags-only-I gtk+-3.0 | sed "s|-I$OUT||g")"
  echo "libs $(pkg-config --libs-only-l gtk+-3.0 | sed 's/-l//g')"
} > "$OUT/gtk3-flags.txt"
cat "$OUT/gtk3-flags.txt"

du -sh "$OUT"
