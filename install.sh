#!/usr/bin/env bash
# Builds Canopy and installs it for the current user (no root needed).
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" --target canopy
PREFIX="${PREFIX:-$HOME/.local}"
mkdir -p "$PREFIX/bin" "$PREFIX/share/canopy/shell" "$PREFIX/share/applications"
install -m 755 build/canopy "$PREFIX/bin/canopy"
install -m 644 shell/canopy.bash shell/canopy-rcfile.bash "$PREFIX/share/canopy/shell/"
sed "s|^Exec=canopy|Exec=$PREFIX/bin/canopy|" packaging/canopy.desktop > "$PREFIX/share/applications/canopy.desktop"
update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
echo "Installed to $PREFIX/bin/canopy (shell integration in $PREFIX/share/canopy/shell)"
