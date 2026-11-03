# TrueHDR Desktop: RTX TrueHDR + RTX Video Super Resolution for Linux

**Community:** join the [Discord](https://discord.gg/nuvpXycag8) for help and updates · support the project on [Ko-fi](https://ko-fi.com/wuzzy450)

NVIDIA's **RTX Video HDR** (AI SDR→HDR) and **RTX Video Super Resolution** (AI upscaling) for the
**whole KDE Plasma desktop** on Linux. That covers YouTube, Netflix (DRM video too), mpv, SDR games
and the desktop itself. Windows only offers this inside Chrome/Edge. NVIDIA doesn't ship it for
Linux at all.

- **TrueHDR on everything**: every pixel KWin composites is converted SDR→HDR10 with NVIDIA's
  TrueHDR network, at the peak brightness you choose (400–2000 nits).
- **Automatic AI upscaling**: when a fullscreen window shows 720p/1080p/1440p content stretched to
  your screen, it's detected, shrunk back to its real size and rebuilt with NVIDIA VSR, then HDR'd.
- **Lives in the system tray**: starts at login, left-click toggles, right-click for brightness
  and upscaling settings. Also **Meta+Ctrl+H** and a `truehdr` command.
- **Runs inside the compositor**, so there's no overlay window. Input goes straight to your apps and
  games, and fullscreen games are covered.
- **Any resolution, any refresh rate, multiple monitors**: each HDR output gets its own buffers.
  Upscale detection is time-based, so it acts the same at 60, 120 or 240 Hz.

## Requirements

| | |
|---|---|
| Desktop | **KDE Plasma 6.7 or newer, Wayland session.** Any distro: Arch, Fedora, openSUSE, Debian/Ubuntu/Kubuntu, KDE neon, CachyOS, PikaOS… (immutable distros like Bazzite/Kinoite need a writable /usr, so they aren't supported yet) |
| GPU | **NVIDIA RTX** (20-series or newer) with the proprietary driver (tested on 580) |
| Display | An HDR monitor/TV with **HDR turned on** in System Settings → Display & Monitor |
| Account | A free NVIDIA NGC account, used to download the SDK (NVIDIA doesn't allow redistributing it) |

GNOME, Hyprland, Sway etc. aren't supported. The effect is a KWin plugin, and those compositors
have no equivalent hook. AMD/Intel GPUs can't run NVIDIA's networks.

## Install (about 10 minutes, mostly waiting)

**1.** Open a terminal: press the Meta (Windows) key, type **Konsole**, press Enter.

**2.** Copy this line, paste it into the terminal with **Ctrl+Shift+V**, and press **Enter**:

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/wuzzy450/TrueHDR-Desktop/main/get.sh)
```

**3.** Follow what it says. It will:
- check that your PC can run it (and explain in plain words if it can't),
- ask for **your password** to install build tools (nothing shows while you type a password, that's normal),
- open NVIDIA's website so you can get a **free key**. Sign in or make a free account, click
  **Generate Personal Key** (tick *NGC Catalog* if asked), click **Copy**, then paste it into
  the terminal and press Enter. NVIDIA requires this. It's free, and you only do it once.
- download NVIDIA's AI models (~2.5 GB), build and install everything,
- offer to fix dark-video blotches in Chrome/Edge/Brave (say yes).

**4.** Turn on HDR: **System Settings → Display & Monitor → your screen → Enable HDR**.

**5.** **Log out and back in** (the installer offers to do this). Then an **HDR** icon shows up next
to the clock. Left-click turns it on or off, right-click changes brightness and upscaling.

### After a big Plasma update
Plasma updates need a quick rebuild, or TrueHDR stays off (nothing breaks). Paste this, then log out and in:
```bash
bash <(curl -fsSL https://raw.githubusercontent.com/wuzzy450/TrueHDR-Desktop/main/get.sh) build
```

<details><summary>Advanced: install from a git clone / use an SDK you already have</summary>

```bash
git clone https://github.com/wuzzy450/TrueHDR-Desktop && cd TrueHDR-Desktop && ./install.sh
```
`./install.sh build | chromefix | uninstall` run single steps. Already logged in with the NGC CLI?
The installer reuses it. You can also set `NGC_CLI_API_KEY` beforehand to skip the prompt.
To use an existing Maxine VideoFX 1.3 SDK (with the `nvvfxtruehdr` and `nvvfxvideosuperres` features):
```bash
kwriteconfig6 --file kwinrc --group Effect-truehdr --key SdkPath /path/to/VideoFX
cmake -S . -B build -DVFX_SDK=/path/to/VideoFX -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build && sudo cmake --install build
```
The installer puts the SDK in `~/.local/share/truehdr-desktop/VideoFX`.
</details>

## Using it

| What | Tray (right-click) | Command |
|---|---|---|
| On / off | left-click the icon | `truehdr on` / `truehdr off`, or **Meta+Ctrl+H** |
| Peak brightness | *Peak brightness…* (slider, previews live) or *Brightness presets* | `truehdr nits 1000` (400–2000) |
| AI upscaling | *AI upscaling of fullscreen video/games* | `truehdr upscale on\|off` |
| Upscaling quality | *Upscaling quality* | `truehdr upscale quality 17` |
| Block cleanup on native content | *Block cleanup…* | `truehdr cleanup off\|fullscreen\|always` |
| Debanding | *AI debanding* | `truehdr deband on\|off` |
| Everything else | – | `truehdr` (lists all), `truehdr status` |
| Back to defaults | *Reset to defaults* | `truehdr reset` |

**Set the nits to your display's real peak brightness** (check a review such as rtings.com, or
your display's HDR info). Overshooting clips highlights. Undershooting looks dull.

### Defaults
These were tuned on a 4K HDR TV and are what you get out of the box:

| Setting | Default | Why |
|---|---|---|
| Peak brightness | 1000 nits | typical HDR TV peak. Change it to match yours |
| Contrast / saturation / middle gray | 100 / 100 / 50 | NVIDIA's defaults |
| AI debanding | on | smoother gradients |
| AI upscaling | on, quality **17** | measured most faithful on real video (mode 4 oversharpens) |
| Block cleanup on native content | **off** | it changes clean native content |
| Upscale detector | threshold 0.4, 7 of 8 votes | 0% false triggers on native 4K, ~100% hits on 720p in testing |

Settings live in `~/.config/kwinrc` under `[Effect-truehdr]` and apply live. Reinstalling never
overwrites them.

### Games
- **SDR games**: just play. TrueHDR covers fullscreen games too.
- **Games with native HDR** (e.g. via gamescope `--hdr-enabled` / `PROTON_ENABLE_HDR=1`): **turn
  TrueHDR off** (Meta+Ctrl+H) or the image is converted to HDR twice and looks blown out.

### Resolutions & refresh rates
- Works at any output resolution and refresh rate, with mixed HDR + SDR monitors (SDR outputs are
  left alone). Fractional scaling is fine because it works on real device pixels.
- Upscaling triggers on content stretched **1.5×, 2× or 3×** (720p/1080p/1440p on 4K,
  720p on 1080p/1440p, 480p on 1440p…). Other ratios, such as 1080p on a 1440p screen, play normally
  with TrueHDR only.
- Cost per frame grows with pixel count. At 4K on an RTX 3080: TrueHDR ≈ 3.6 ms, plus upscaling
  ≈ 3–14 ms depending on quality. That fits 60 Hz easily. At **4K 120 Hz+** you may drop frames
  while upscaling is active. Use `truehdr upscale quality 2` (fast) or turn upscaling off for
  high-refresh gaming. Lower resolutions are proportionally cheaper.

## Fix: dark, blotchy, blue-tinted video in Chrome / Chromium / Edge / Brave ⚠️

**If dark scenes in browser video look blocky with a blue/grey haze, the browser is the cause,
not TrueHDR.** Recent Chromium builds (≈ v150+) enable Wayland colour management
(`WaylandWpColorManagerV1`). On Plasma it hands KWin video with **lifted, blue-tinted shadows**
(pure black 0 becomes ~14 in blue, and near-black 8–16 becomes ~22). TrueHDR then expands that
error into visible blotches. Measured against an ffmpeg decode of the same frame: default 0 → 14,
with the fix 0 → 0.

**Fix:** start the browser with `--disable-features=WaylandWpColorManagerV1`.

**The installer does this for you** (or run `~/.local/share/truehdr-desktop/src/install.sh chromefix` any time). To do it by hand,
make a user copy of the launcher, which survives browser updates:
```bash
# Chrome (use chromium.desktop / microsoft-edge.desktop / brave-browser.desktop for others)
cp /usr/share/applications/google-chrome.desktop ~/.local/share/applications/
sed -i 's|^Exec=\([^ ]*\)|Exec=\1 --disable-features=WaylandWpColorManagerV1|' \
  ~/.local/share/applications/google-chrome.desktop
```
Then fully quit the browser (check it isn't still in the tray) and reopen it. Check
`chrome://version` → *Command Line* contains the flag.

Notes:
- If you already pass `--disable-features=Something`, **merge** the two
  (`--disable-features=Something,WaylandWpColorManagerV1`). Chromium only honours the last
  `--disable-features` flag.
- Taskbar/dock pins that point at the system launcher may bypass your copy. Re-pin it after the fix.
- Flatpak browsers: `flatpak override --user` can't add flags. Put the flag in
  `~/.var/app/<app-id>/config/chrome-flags.conf` (Chrome) or `chromium-flags.conf` (Chromium).
- Firefox isn't affected.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Tray says on, but the picture is unchanged | HDR must be on for that display in System Settings. Log out/in after installing or rebuilding. |
| Nothing after a Plasma update | run the rebuild line from *After a big Plasma update*, then log out/in |
| Washed out / blown out in a game | the game already outputs HDR, so turn TrueHDR off |
| Dark browser video blotchy/blue | the Chrome fix above |
| Upscaling never/always kicks in | `truehdr upscale log on`, then `journalctl --user -f -o cat \| grep truehdr` shows each reading. `truehdr upscale force 0\|720\|1080\|1440` overrides detection. |
| "CUDA … illegal memory access" in the log | toggle off/on (Meta+Ctrl+H) to reset CUDA. Check `sudo journalctl -k \| grep -i xid` for driver faults |
| Mouse cursor isn't HDR | expected: it's a separate hardware plane |

Still stuck? Ask on the [Discord](https://discord.gg/nuvpXycag8) and include the output of `truehdr status`.

Logs: `journalctl --user -b -o cat | grep truehdr:`. On any error the effect turns itself off and logs
why, and KWin keeps running normally.

## How it works

Per frame, on each HDR output:
1. KWin paints the finished frame into an offscreen texture.
2. KWin's own colour pipeline turns it into 8-bit sRGB SDR.
3. *(Optional)* If a fullscreen window shows stretched content, the detector (which runs ~3×/s on a
   512×512 centre patch, on a worker thread) finds the stretch fingerprint. The frame is shrunk back
   to its source size and NVIDIA VSR rebuilds it at full resolution.
4. CUDA–GL interop hands the frame to **TrueHDR** → BT.2020 PQ HDR10.
5. KWin draws the result to the real output with absolute nits.

The SDK and its private CUDA 12.8 runtime are loaded with `dlopen` by full path, so they never
shadow your system CUDA, and a missing SDK just disables the effect.

An optional final pass ports Lilium's *HDR black floor fix* (`truehdr black …`, off by default).

## Uninstall
```bash
~/.local/share/truehdr-desktop/src/install.sh uninstall
```
This keeps the SDK (`~/.local/share/truehdr-desktop`, ~4 GB) and your settings. Delete them by hand
if you want them gone.

## Support
TrueHDR Desktop is free. If you enjoy it, you can [support it on Ko-fi](https://ko-fi.com/wuzzy450). Questions, bugs and ideas: [Discord](https://discord.gg/nuvpXycag8) or GitHub issues.

## Credits & licence
- GPL-3.0-or-later (see `LICENSE`). Built on KWin's effect API (GPL). The black floor pass is a port
  of [Lilium's ReShade HDR shaders](https://github.com/EndlesslyFlowering/ReShade_HDR_shaders) (GPL-3.0).
- NVIDIA Maxine, TrueHDR, RTX Video and VSR are NVIDIA's, under NVIDIA's own licence. They're
  downloaded from NGC by you and are **not** part of this repository. This project isn't affiliated
  with or endorsed by NVIDIA or KDE.
