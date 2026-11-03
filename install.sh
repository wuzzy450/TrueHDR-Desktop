#!/usr/bin/env bash
# TrueHDR Desktop installer.
#   ./install.sh            everything: deps, NVIDIA SDK, build, install, tray at login
#   ./install.sh deps       build dependencies only (asks for sudo)
#   ./install.sh sdk        download the NVIDIA Maxine SDK + TrueHDR + VSR (needs a free NGC key)
#   ./install.sh build      build + install only (run this again after every Plasma/KWin update)
#   ./install.sh uninstall  remove the effect, tray app and autostart (keeps the SDK and settings)
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

SDK_HOME="${XDG_DATA_HOME:-$HOME/.local/share}/truehdr-desktop"
SDK="$SDK_HOME/VideoFX"
BUILD="${BUILD_DIR:-$PWD/build}"
VER=1.3.0.0

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

check_system() {
  [[ "${XDG_SESSION_TYPE:-}" == wayland ]] || warn "Not in a Wayland session. TrueHDR Desktop needs Plasma on Wayland."
  command -v kwin_wayland >/dev/null || die "KWin (KDE Plasma) not found. This only works on KDE Plasma 6.7+ (Wayland)."
  command -v nvidia-smi >/dev/null || die "NVIDIA driver not found (nvidia-smi missing). An RTX GPU is required."
  local cc
  cc=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | sort -rn | head -1 || true)
  if [[ -n "$cc" ]] && awk -v c="$cc" 'BEGIN{exit !(c<7.5)}'; then
    die "Best GPU has compute capability $cc. TrueHDR needs an RTX card (Turing / RTX 20-series or newer)."
  fi
}

deps() {
  say "Installing build dependencies"
  . /etc/os-release
  local like=" ${ID:-} ${ID_LIKE:-} "
  if command -v pacman >/dev/null; then
    sudo pacman -S --needed --noconfirm base-devel cmake extra-cmake-modules kwin qt6-base kconfig kglobalaccel kcoreaddons
  elif command -v dnf >/dev/null; then
    sudo dnf install -y gcc-c++ cmake extra-cmake-modules kwin-devel qt6-qtbase-devel qt6-qtbase-private-devel \
      kf6-kconfig-devel kf6-kglobalaccel-devel kf6-kcoreaddons-devel
  elif command -v zypper >/dev/null; then
    sudo zypper --non-interactive install gcc-c++ cmake kf6-extra-cmake-modules kwin6-devel qt6-base-devel qt6-base-private-devel \
      kf6-kconfig-devel kf6-kglobalaccel-devel kf6-kcoreaddons-devel
  elif command -v apt-get >/dev/null; then
    sudo apt-get install -y g++ cmake extra-cmake-modules kwin-dev qt6-base-dev qt6-base-private-dev \
      libkf6config-dev libkf6globalaccel-dev libkf6coreaddons-dev
  else
    die "Unknown package manager on '$like'. Install by hand: C++ compiler, cmake, extra-cmake-modules, KWin development headers, Qt6 base dev, KF6 Config/GlobalAccel/CoreAddons dev."
  fi
}

sdk() {
  if [[ -f "$SDK/lib/libVideoFX.so" && -d "$SDK/features/nvvfxtruehdr" && -d "$SDK/features/nvvfxvideosuperres" ]]; then
    say "NVIDIA SDK already installed in $SDK"; return
  fi
  if ! command -v ngc >/dev/null; then
    cat >&2 <<EOF

The NVIDIA Maxine SDK is free but can't be redistributed, so you download it with NVIDIA's NGC tool:
  1. Make a free account at https://ngc.nvidia.com and create an API key (Setup -> Generate API Key).
  2. Install the NGC CLI: https://org.ngc.nvidia.com/setup/installers/cli  (Linux AMD64 zip),
     unzip it and put 'ngc' on your PATH.
  3. Run: ngc config set     (paste the key; pick your org when asked; format ascii)
  4. Run ./install.sh again.
EOF
    exit 1
  fi
  ngc config current >/dev/null 2>&1 || { say "NGC isn't configured yet. Starting 'ngc config set' (paste your API key)"; ngc config set; }
  local dl="$SDK_HOME/download"
  mkdir -p "$dl" "$SDK_HOME"
  say "Downloading Maxine VideoFX SDK $VER (~2 GB)"
  ngc registry resource download-version "nvidia/maxine/vfx_sdk_core:${VER}_linux" --dest "$dl"
  say "Downloading TrueHDR feature"
  ngc registry model download-version "nvidia/maxine/nvvfxtruehdr:${VER}_lib_linux" --dest "$dl"
  say "Downloading Video Super Resolution feature"
  ngc registry model download-version "nvidia/maxine/nvvfxvideosuperres:${VER}_lib_linux" --dest "$dl"
  say "Unpacking"
  tar xzf "$(find "$dl" -name "VFXSDK_linux_${VER}.tgz" | head -1)" -C "$SDK_HOME"
  for f in nvvfxtruehdr nvvfxvideosuperres; do
    tar xzf "$(find "$dl" -path "*${f}_v*" -name '*.tar.gz' | head -1)" -C "$SDK/features"
  done
  [[ -f "$SDK/lib/libVideoFX.so" ]] || die "Unpacking failed: $SDK/lib/libVideoFX.so missing"
  rm -rf "$dl"
  say "SDK ready in $SDK"
}

build() {
  [[ -f "$SDK/include/nvVideoEffects.h" ]] || die "SDK missing. Run ./install.sh sdk first."
  say "Building against KWin $(kwin_wayland --version 2>/dev/null | awk '{print $2}')"
  rm -rf "$BUILD"
  cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DVFX_SDK="$SDK"
  cmake --build "$BUILD" -j"$(nproc)"
  say "Installing (sudo)"
  sudo cmake --install "$BUILD"
  first_setup
}

first_setup() {
  local G=Effect-truehdr
  # defaults only where the user has no value yet, so reinstalling never overwrites their settings
  for kv in Luminance=1000 Contrast=100 Saturation=100 MiddleGray=50 DebandingOff=false \
            Upscale=true UpscaleQuality=17 Cleanup=off DetectThreshold=0.4 OnlyHdrOutputs=true EnabledAtStart=true; do
    [[ -n "$(kreadconfig6 --file kwinrc --group $G --key "${kv%%=*}")" ]] || \
      kwriteconfig6 --file kwinrc --group $G --key "${kv%%=*}" "${kv#*=}"
  done
  kwriteconfig6 --file kwinrc --group Plugins --key truehdrEnabled true
  mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
  cat > "${XDG_CONFIG_HOME:-$HOME/.config}/autostart/truehdr-tray.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=TrueHDR Desktop
Exec=truehdr-tray
Icon=video-display
X-KDE-autostart-phase=2
NoDisplay=true
EOF
  pgrep -x truehdr-tray >/dev/null || (setsid truehdr-tray >/dev/null 2>&1 &)
  cat <<EOF

$(say "Installed.")
  * Turn HDR on for your display in System Settings -> Display & Monitor.
  * LOG OUT AND BACK IN once: KWin only loads a new or rebuilt effect at login.
  * Then use the HDR icon in the tray (left click = on/off, right click = settings),
    Meta+Ctrl+H, or the 'truehdr' command.
  * After every Plasma/KWin update: run ./install.sh build again.
EOF
}

uninstall() {
  say "Removing TrueHDR Desktop"
  pkill -x truehdr-tray || true
  qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.unloadEffect truehdr >/dev/null 2>&1 || true
  kwriteconfig6 --file kwinrc --group Plugins --key truehdrEnabled false
  rm -f "${XDG_CONFIG_HOME:-$HOME/.config}/autostart/truehdr-tray.desktop"
  if [[ -f "$BUILD/install_manifest.txt" ]]; then
    xargs -a "$BUILD/install_manifest.txt" sudo rm -f
  else
    sudo rm -f /usr/bin/truehdr /usr/bin/truehdr-tray /usr/share/applications/truehdr-tray.desktop
    sudo find /usr/lib /usr/lib64 -path '*kwin/effects/plugins/truehdr.so' -delete 2>/dev/null || true
  fi
  say "Done. SDK left in $SDK (delete it by hand to free ~4 GB); settings left in kwinrc [Effect-truehdr]."
}

case "${1:-all}" in
  deps) deps;;
  sdk) sdk;;
  build) check_system; build;;
  uninstall) uninstall;;
  all) check_system; deps; sdk; build;;
  *) sed -n '2,7p' "$0" | sed 's/^# *//'; exit 1;;
esac
