#!/bin/sh
# Build MangoHud with a native INPUT row (input-latency.patch) and install it as a per-user Vulkan
# layer that overrides the system one (same layer name; the loader reads ~/.local/share before
# /usr/share). 64-bit Vulkan only: OpenGL and 32-bit games keep the stock MangoHud, where the
# `input_latency` option is just ignored.
#
#   sh mangohud/build-mangohud.sh                 # version of the installed mangohud (pacman/dpkg/rpm)
#   MANGOHUD_VER=0.8.4 sh mangohud/build-mangohud.sh
#
# Rerun after your distro updates MangoHud. Undo: rm ~/.local/share/vulkan/implicit_layer.d/MangoHud.x86_64.json
# Needs: a C++ compiler, python3, curl, patch, and MangoHud's build deps (glslang, libX11, wayland, ...).
set -eu
installed_version() {
    { pacman -Q mangohud 2>/dev/null | sed 's/.* //; s/-.*//'; } ||
    { dpkg-query -W -f '${Version}' mangohud 2>/dev/null | sed 's/-.*//; s/^[0-9]*://'; } ||
    { rpm -q --qf '%{VERSION}' mangohud 2>/dev/null; }
}
VER=${MANGOHUD_VER:-$(installed_version || true)}
[ -n "$VER" ] || { echo "Can't tell which MangoHud you have: set MANGOHUD_VER (e.g. 0.8.4)" >&2; exit 1; }
SYSTEM_MANIFEST=$(ls /usr/share/vulkan/implicit_layer.d/MangoHud*.x86_64.json /usr/share/vulkan/implicit_layer.d/MangoHud.json 2>/dev/null | head -1 || true)
[ -n "$SYSTEM_MANIFEST" ] || { echo "Install MangoHud from your distro first (its layer manifest is copied)" >&2; exit 1; }
SRC=$(cd "$(dirname "$0")" && pwd)
W=${XDG_CACHE_HOME:-$HOME/.cache}/ember-latency-mangohud
LIB=$HOME/.local/lib/ember-latency/mangohud
LAYERS=$HOME/.local/share/vulkan/implicit_layer.d
mkdir -p "$W" "$LIB" "$LAYERS"
cd "$W"
[ -x venv/bin/meson ] || { python3 -m venv venv && venv/bin/pip -q install meson ninja mako; }
PATH=$W/venv/bin:$PATH
[ -f "src-$VER.tar.xz" ] || curl -fLo "src-$VER.tar.xz" \
  "https://github.com/flightlessmango/MangoHud/releases/download/v$VER/MangoHud-v$VER-Source.tar.xz"
rm -rf "MangoHud-v$VER" build
tar xf "src-$VER.tar.xz"
patch -d "MangoHud-v$VER" -p1 < "$SRC/input-latency.patch"
meson setup build "MangoHud-v$VER" --buildtype=release -Dwith_xnvctrl=disabled -Dtests=disabled \
  -Dmangoplot=disabled -Dinclude_doc=false -Dmangoapp=false -Dmangohudctl=false
meson compile -C build
install -m755 build/src/libMangoHud.so "$LIB/libMangoHud.so.new"
mv "$LIB/libMangoHud.so.new" "$LIB/libMangoHud.so"
sed "s#\"library_path\": \"[^\"]*\"#\"library_path\": \"$LIB/libMangoHud.so\"#" "$SYSTEM_MANIFEST" \
  > "$LAYERS/MangoHud.x86_64.json"
echo "Patched MangoHud $VER installed: $LIB/libMangoHud.so"
echo "Add a line with just  input_latency  to ~/.config/MangoHud/MangoHud.conf"
