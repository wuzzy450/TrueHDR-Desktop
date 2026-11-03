#!/usr/bin/env bash
# TrueHDR Desktop installer. Just run it: ./install.sh
#   ./install.sh            everything (recommended)
#   ./install.sh build      rebuild after a Plasma/KWin update
#   ./install.sh chromefix  fix dark/blotchy video in Chrome-based browsers
#   ./install.sh uninstall  remove it (keeps the NVIDIA SDK and your settings)
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

APP_HOME="${XDG_DATA_HOME:-$HOME/.local/share}/truehdr-desktop"
SDK="$APP_HOME/VideoFX"
NGC_DIR="$APP_HOME/ngc-cli"
BUILD="${BUILD_DIR:-$PWD/build}"
VER=1.3.0.0
NGC_CLI_URL="https://api.ngc.nvidia.com/v2/resources/nvidia/ngc-apps/ngc_cli/versions/4.36.6/files/ngccli_linux.zip"
KEY_PAGE="https://org.ngc.nvidia.com/setup/api-keys"

bold=$'\033[1m'; dim=$'\033[2m'; off=$'\033[0m'
step() { printf '\n\033[1;36m━━ %s\033[0m\n' "$*"; }
say()  { printf '   %s\n' "$*"; }
ok()   { printf '   \033[1;32m✔\033[0m %s\n' "$*"; }
warn() { printf '   \033[1;33m!\033[0m %s\n' "$*" >&2; }
die()  { printf '\n   \033[1;31m✘ %s\033[0m\n\n' "$*" >&2; exit 1; }
yes_no() { local a; read -rp "   $1 [Y/n] " a </dev/tty; [[ -z "$a" || "$a" =~ ^[Yy] ]]; }
pause() { read -rp "   Press Enter to continue… " _ </dev/tty; }

check_system() {
  step "Checking your computer"
  command -v kwin_wayland >/dev/null || die "KDE Plasma not found. TrueHDR Desktop only works on KDE Plasma (6.7 or newer)."
  local kv; kv=$(kwin_wayland --version 2>/dev/null | awk '{print $2}')
  if [[ -n "$kv" ]] && [[ "$(printf '%s\n6.7\n' "$kv" | sort -V | head -1)" != 6.7 ]]; then
    die "You have Plasma $kv. TrueHDR Desktop needs Plasma 6.7 or newer. Update your system first."
  fi
  ok "KDE Plasma $kv"
  [[ "${XDG_SESSION_TYPE:-}" == wayland ]] && ok "Wayland session" || \
    warn "You're not in a Wayland session. On the login screen pick 'Plasma (Wayland)' before using this."
  command -v nvidia-smi >/dev/null || die "NVIDIA driver not found. Install your distro's proprietary NVIDIA driver first, then reboot."
  local gpu cc
  gpu=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)
  cc=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | sort -rn | head -1 || true)
  if [[ -n "$cc" ]] && awk -v c="$cc" 'BEGIN{exit !(c<7.5)}'; then
    die "$gpu isn't an RTX card. TrueHDR needs an NVIDIA RTX GPU (20-series or newer)."
  fi
  ok "${gpu:-NVIDIA GPU}"
  if grep -qiE 'ostree|silverblue|kinoite|bazzite' /etc/os-release 2>/dev/null; then
    die "Immutable distros (Bazzite, Kinoite…) can't install KWin plugins this way yet."
  fi
}

deps() {
  step "Installing the tools needed to build it (asks for your password)"
  if command -v pacman >/dev/null; then
    sudo pacman -S --needed --noconfirm base-devel cmake extra-cmake-modules kwin qt6-base kconfig kglobalaccel kcoreaddons unzip curl
  elif command -v dnf >/dev/null; then
    sudo dnf install -y gcc-c++ cmake extra-cmake-modules kwin-devel qt6-qtbase-devel qt6-qtbase-private-devel \
      kf6-kconfig-devel kf6-kglobalaccel-devel kf6-kcoreaddons-devel unzip curl
  elif command -v zypper >/dev/null; then
    sudo zypper --non-interactive install gcc-c++ cmake kf6-extra-cmake-modules kwin6-devel qt6-base-devel qt6-base-private-devel \
      kf6-kconfig-devel kf6-kglobalaccel-devel kf6-kcoreaddons-devel unzip curl
  elif command -v apt-get >/dev/null; then
    sudo apt-get install -y g++ cmake extra-cmake-modules kwin-dev qt6-base-dev qt6-base-private-dev \
      libkf6config-dev libkf6globalaccel-dev libkf6coreaddons-dev unzip curl
  else
    die "Couldn't recognise your package manager. Install by hand: a C++ compiler, cmake, extra-cmake-modules, KWin development files, Qt6 base development files, KF6 Config/GlobalAccel/CoreAddons development files, unzip, curl. Then run ./install.sh again."
  fi
  ok "Build tools installed"
}

ngc_bin() {
  if command -v ngc >/dev/null; then command -v ngc; return; fi
  if [[ ! -x "$NGC_DIR/ngc" ]]; then
    say "Downloading NVIDIA's download tool (NGC CLI)…"
    mkdir -p "$APP_HOME"
    curl -fL# "$NGC_CLI_URL" -o "$APP_HOME/ngccli.zip"
    rm -rf "$NGC_DIR" "$APP_HOME/ngc-cli.tmp"
    unzip -q "$APP_HOME/ngccli.zip" -d "$APP_HOME/ngc-cli.tmp"
    mv "$APP_HOME/ngc-cli.tmp/ngc-cli" "$NGC_DIR"; rm -rf "$APP_HOME/ngc-cli.tmp" "$APP_HOME/ngccli.zip"
    chmod +x "$NGC_DIR/ngc"
  fi
  echo "$NGC_DIR/ngc"
}

sdk() {
  step "Getting NVIDIA's AI models (TrueHDR + upscaler)"
  if [[ -f "$SDK/lib/libVideoFX.so" && -d "$SDK/features/nvvfxtruehdr" && -d "$SDK/features/nvvfxvideosuperres" ]]; then
    ok "Already downloaded"; return
  fi
  local ngc; ngc=$(ngc_bin)
  local key="${NGC_CLI_API_KEY:-}" org="${NGC_CLI_ORG:-}"
  # reuse an existing NGC login if there is one
  if [[ -z "$key" && -f "$HOME/.ngc/config" ]] && "$ngc" config current >/dev/null 2>&1; then
    ok "Using your existing NGC login"
  else
    if [[ -z "$key" ]]; then
      cat <<EOF

   NVIDIA lets anyone download these for free, but you need a free NVIDIA account
   and a "key" (a long password-like code). It takes about 2 minutes:

     ${bold}1.${off} A web page will open. Sign in, or make a free NVIDIA account.
     ${bold}2.${off} Click ${bold}"Generate Personal Key"${off} (or "Generate API Key").
        Any name is fine. If it asks which services, tick ${bold}"NGC Catalog"${off}.
     ${bold}3.${off} Click ${bold}Copy${off}, come back here, and paste it (Ctrl+Shift+V).

   ${dim}Page: $KEY_PAGE${off}
EOF
      pause
      xdg-open "$KEY_PAGE" >/dev/null 2>&1 || say "Open this page in your browser: $KEY_PAGE"
      while [[ -z "$key" ]]; do
        read -rsp "   Paste your key here and press Enter (it stays hidden): " key </dev/tty; echo
        key=$(tr -d '[:space:]' <<<"$key")
      done
    fi
    if [[ -z "$org" ]]; then
      say "Checking your key…"
      local tmp; tmp=$(mktemp -d)
      org=$(printf '%s\nascii\n\n' "$key" | HOME="$tmp" timeout 30 "$ngc" config set 2>&1 \
            | grep -o "Enter org.*Choices: \[[^]]*\]" | head -1 | grep -oE "'[^']+'" | head -1 | tr -d "'" || true)
      rm -rf "$tmp"
      [[ -n "$org" ]] || die "NVIDIA didn't accept that key. Make a new one on $KEY_PAGE and run ./install.sh again."
      ok "Key works"
    fi
    export NGC_CLI_API_KEY="$key" NGC_CLI_ORG="$org"
  fi
  export NGC_CLI_FORMAT_TYPE=ascii
  local dl="$APP_HOME/download"
  mkdir -p "$dl"
  say "Downloading (about 2.5 GB, this can take a while)…"
  "$ngc" registry resource download-version "nvidia/maxine/vfx_sdk_core:${VER}_linux" --dest "$dl"
  "$ngc" registry model download-version "nvidia/maxine/nvvfxtruehdr:${VER}_lib_linux" --dest "$dl"
  "$ngc" registry model download-version "nvidia/maxine/nvvfxvideosuperres:${VER}_lib_linux" --dest "$dl"
  say "Unpacking…"
  rm -rf "$SDK"
  tar xzf "$(find "$dl" -name "VFXSDK_linux_${VER}.tgz" | head -1)" -C "$APP_HOME"
  for f in nvvfxtruehdr nvvfxvideosuperres; do
    tar xzf "$(find "$dl" -path "*${f}_v*" -name '*.tar.gz' | head -1)" -C "$SDK/features"
  done
  [[ -f "$SDK/lib/libVideoFX.so" ]] || die "Unpacking failed. Delete $APP_HOME and run ./install.sh again."
  rm -rf "$dl"
  ok "NVIDIA models ready"
}

build() {
  [[ -f "$SDK/include/nvVideoEffects.h" ]] || die "NVIDIA models are missing. Run ./install.sh first."
  step "Building TrueHDR Desktop for your Plasma version"
  rm -rf "$BUILD"
  cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DVFX_SDK="$SDK" >/dev/null
  cmake --build "$BUILD" -j"$(nproc)" >/dev/null 2>"$BUILD/errors.log" || { tail -20 "$BUILD/errors.log" >&2; die "Build failed (details above). Please open an issue on GitHub with this output."; }
  say "Installing (asks for your password)…"
  sudo cmake --install "$BUILD" >/dev/null
  ok "Installed"
  first_setup
}

first_setup() {
  local G=Effect-truehdr
  # defaults only where there's no value yet, so reinstalling never overwrites your settings
  for kv in Luminance=1000 Contrast=100 Saturation=100 MiddleGray=50 DebandingOff=false \
            Upscale=true UpscaleQuality=17 Cleanup=off DetectThreshold=0.4 OnlyHdrOutputs=true EnabledAtStart=true; do
    [[ -n "$(kreadconfig6 --file kwinrc --group $G --key "${kv%%=*}")" ]] || \
      kwriteconfig6 --file kwinrc --group $G --key "${kv%%=*}" "${kv#*=}"
  done
  kwriteconfig6 --file kwinrc --group Plugins --key truehdrEnabled true
  local as="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
  mkdir -p "$as"
  printf '[Desktop Entry]\nType=Application\nName=TrueHDR Desktop\nExec=truehdr-tray\nIcon=video-display\nX-KDE-autostart-phase=2\nNoDisplay=true\n' > "$as/truehdr-tray.desktop"
  pgrep -x truehdr-tray >/dev/null || (setsid truehdr-tray >/dev/null 2>&1 &)
  ok "Tray icon added (starts at login)"
}

chromefix() {
  step "Browser fix (dark video looking blotchy/blue in Chrome-based browsers)"
  local found=0 f name dst
  for name in google-chrome chromium chromium-browser microsoft-edge brave-browser vivaldi-stable opera; do
    f="/usr/share/applications/$name.desktop"
    [[ -f "$f" ]] || continue
    found=1
    dst="$HOME/.local/share/applications/$name.desktop"
    if [[ -f "$dst" ]] && grep -q WaylandWpColorManagerV1 "$dst"; then ok "$name already fixed"; continue; fi
    [[ -f "$dst" ]] || { mkdir -p "$(dirname "$dst")"; cp "$f" "$dst"; }
    # add the feature to an existing --disable-features list, or add a new flag
    if grep -q -- '--disable-features=' "$dst"; then
      sed -i '/^Exec=/ s/--disable-features=\([^ ]*\)/--disable-features=\1,WaylandWpColorManagerV1/' "$dst"
    else
      sed -i 's|^Exec=\([^ ]*\)|Exec=\1 --disable-features=WaylandWpColorManagerV1|' "$dst"
    fi
    ok "Fixed $name (close it completely and reopen it)"
  done
  (( found )) || say "No Chrome-based browser found. Nothing to do."
}

finish() {
  step "All done!"
  cat <<EOF
   Last steps:
     ${bold}1.${off} Turn HDR on: System Settings → Display & Monitor → your screen → ${bold}Enable HDR${off}.
     ${bold}2.${off} ${bold}Log out and log back in${off} (needed once, so Plasma loads TrueHDR).

   Afterwards, look for the ${bold}HDR${off} icon by the clock:
     • left-click = on/off      • right-click = brightness & upscaling settings
     • Meta+Ctrl+H also toggles it

   ${dim}After a big Plasma update, run ./install.sh build again.${off}
EOF
  if yes_no "Log out now?"; then
    qdbus6 org.kde.Shutdown /Shutdown org.kde.Shutdown.logout >/dev/null 2>&1 || \
      qdbus6 org.kde.ksmserver /KSMServer logout 0 0 0 >/dev/null 2>&1 || say "Please log out from the start menu."
  fi
}

uninstall() {
  step "Removing TrueHDR Desktop"
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
  ok "Removed. To also free ~4 GB, delete $APP_HOME"
}

case "${1:-all}" in
  all)
    printf '\n\033[1m TrueHDR Desktop installer\033[0m  — AI HDR + upscaling for KDE Plasma (NVIDIA RTX)\n'
    check_system; deps; sdk; build
    if yes_no "Also fix the dark-video bug in Chrome/Edge/Brave (recommended)?"; then chromefix; fi
    finish;;
  build) check_system; build; say "Log out and back in to load the new build.";;
  sdk) sdk;;
  deps) deps;;
  chromefix) chromefix;;
  uninstall) uninstall;;
  update) exec bash <(curl -fsSL https://raw.githubusercontent.com/wuzzy450/TrueHDR-Desktop/main/get.sh) build;;
  *) sed -n '2,6p' "$0" | sed 's/^# *//'; exit 1;;
esac
