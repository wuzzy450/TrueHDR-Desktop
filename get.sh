#!/usr/bin/env bash
# One-line installer: bash <(curl -fsSL https://raw.githubusercontent.com/wuzzy450/TrueHDR-Desktop/main/get.sh)
set -euo pipefail
dir="${XDG_DATA_HOME:-$HOME/.local/share}/truehdr-desktop/src"
echo "Downloading TrueHDR Desktop to $dir"
rm -rf "$dir"; mkdir -p "$dir"
curl -fsSL https://github.com/wuzzy450/TrueHDR-Desktop/archive/refs/heads/main.tar.gz | tar xz -C "$dir" --strip-components=1
exec "$dir/install.sh" "$@"
