#!/usr/bin/env python3
"""Detector test on real images, for every monitor shape and content size.

For each source image: make the "content" frame at its real resolution, stretch it
to fit the screen the way a video player does (keeping aspect, black bars), with
ffmpeg's bilinear / bicubic / lanczos scalers, take the screen-centre 512x512 patch
and run the real detector on it (analyse_raw).

usage: real_matrix.py ANALYSE_RAW WORKDIR IMAGE...
"""
import subprocess, sys, os, re, collections

analyser, work, images = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(work, exist_ok=True)

SCREENS = [("4K 16:9", 3840, 2160), ("1440p 16:9", 2560, 1440), ("1080p 16:9", 1920, 1080),
           ("UW 3440x1440", 3440, 1440), ("UW 2560x1080", 2560, 1080), ("SUW 5120x1440", 5120, 1440),
           ("16:10 2560x1600", 2560, 1600), ("16:10 1920x1200", 1920, 1200),
           ("4:3 1600x1200", 1600, 1200), ("4:3 1280x960", 1280, 960), ("4:3 1024x768", 1024, 768),
           ("5:4 1280x1024", 1280, 1024)]
CONTENT = [("native", 0, 0), ("1440p", 2560, 1440), ("1080p", 1920, 1080), ("720p", 1280, 720),
           ("480p", 854, 480), ("4:3 1024x768", 1024, 768), ("4:3 640x480", 640, 480)]
FILTERS = ["bilinear", "bicubic", "lanczos"]
SUPPORTED = [4 / 3, 1.5, 2.0, 3.0]  # stretches the effect can undo (after this change)


def ff(args):
    subprocess.run(["ffmpeg", "-v", "error", "-y"] + args, check=True)


def crop(src, out, w, h, filt):
    # src is already the content frame; stretch it to w x h, then take the centre patch
    ff(["-i", src, "-vf", f"scale={w}:{h}:flags={filt},crop=512:512", "-f", "rawvideo", "-pix_fmt", "rgba", out])


rows = []
jobs = []
for ii, img in enumerate(images):
    for sname, W, H in SCREENS:
        for cname, cw, ch in CONTENT:
            if cname == "native":
                f, fw, fh = 1.0, W, H
                cw, ch = W, H
            else:
                f = min(W / cw, H / ch)
                if f < 0.999:
                    continue
                fw, fh = round(cw * f), round(ch * f)
                if fw < 512 or fh < 512:
                    continue
            base = f"{work}/s{ii}_{W}x{H}_{cw}x{ch}"
            src = base + ".png"
            if not os.path.exists(src):  # content frame at its real resolution (area = clean downscale)
                ff(["-i", img, "-vf", f"scale={cw}:{ch}:force_original_aspect_ratio=increase:flags=area,crop={cw}:{ch}", src])
            for filt in (FILTERS if f > 1.001 else ["none"]):
                out = f"{base}_{filt}.raw"
                if not os.path.exists(out):
                    if filt == "none":
                        ff(["-i", src, "-vf", "crop=512:512", "-f", "rawvideo", "-pix_fmt", "rgba", out])
                    else:
                        crop(src, out, fw, fh, filt)
                jobs.append((sname, cname, f, filt, ii, out))

res = subprocess.run([analyser] + [j[-1] for j in jobs], capture_output=True, text=True).stdout.splitlines()
vals = {}
for line in res:
    path, rest = line.split(" ", 1)
    vals[path] = dict((k, float(v)) for k, v in re.findall(r"(\w+)=([\d.]+)", rest))

table = collections.OrderedDict()
for sname, cname, f, filt, ii, out in jobs:
    v = vals[out]
    expect = next((s for s in SUPPORTED if abs(f - s) < 0.02), 1.0)
    key = (sname, cname, f, expect)
    table.setdefault(key, {}).setdefault(filt, []).append(v)

print(f"{'screen':16} {'content':13} {'ratio':>6} {'expect':>6} | per filter: correct/total (wrong-factor count)")
bad = harm = 0
for (sname, cname, f, expect), byf in table.items():
    cells = []
    for filt, vs in byf.items():
        ok = sum(abs(v["factor"] - expect) < 0.05 for v in vs)
        wrong = sum(v["factor"] > 1.01 and abs(v["factor"] - expect) >= 0.05 for v in vs)
        bad += ok < len(vs)
        harm += wrong
        p = " ".join(f"{k}={sum(v[k] for v in vs)/len(vs):.2f}" for k in ("p2", "p3", "p4", "hi") if k in vs[0])
        cells.append(f"{filt[:4]} {ok}/{len(vs)}{'!'+str(wrong) if wrong else '  '} [{p}]")
    print(f"{sname:16} {cname:13} {f:6.3f} {('x%.2f' % expect) if expect > 1 else 'native':>6} | " + " | ".join(cells))
print(f"\ncells not fully right: {bad}; wrong-factor readings (harmful): {harm}")
