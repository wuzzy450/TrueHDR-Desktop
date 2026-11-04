#pragma once
// Estimates the real resolution of content that has been stretched to fill the
// screen (e.g. a 1080p Netflix stream that Chrome scaled up to 4K).
//
// How: stretching an image by a fixed factor leaves a repeating rhythm in its
// pixel-to-pixel sharpness (every 3 px for 720p or 1440p->4K), and a 2x
// stretch (1080p->4K) leaves mirrored, phase-locked echoes in the spectrum.
// We measure both on a patch of the frame; native content has neither. Pure CPU code, no KWin or CUDA, so it can be tested on its own.
// Returned factor (screen pixels per source pixel): 1 = native, 4/3, 1.5, 2 or 3.
// Works on any screen size or shape: e.g. 2 = 1080p on 4K or 720p on 1440p,
// 4/3 = 1080p on a 1440p, 3440x1440 or 2560x1600 screen.

#include <cstdint>
#include <vector>

namespace resdetect
{

constexpr int kPatch = 512; // patch side in pixels (power of two)
constexpr double kLock43 = 0.45; // default for Detect43Threshold: 4/3 lock minus block-grid lock (tuned 2026-11-03 on photos, x264 CRF 23-38)
constexpr double kTopDetail43 = 0.025; // finest-detail veto for 4/3 (a mild stretch keeps more fine detail)
constexpr double kTopDetailNative = 0.008; // default for DetectTopNative: topDetail above this means native. Real YouTube 4K sits ~0.001, stretched 1440p up to 0.006 (2026-10-25)

struct Result
{
    bool textured = false; // false: patch too flat to judge (black frame, credits...)
    double period2 = 0; // phase-lock between mirrored spectrum bins (2x stretch), 0..1
    double period3 = 0; // strength of a 3-pixel rhythm (3x or 1.5x stretch)
    double lock43 = 0; // phase-lock of spectrum bins mirrored about 3/8 cycles/px (4/3 stretch), 0..1
    double lockGrid = 0; // same lock about 5/16 and 7/16: only compression block grids produce it
    double period4 = 0; // strength of a 4-pixel rhythm (4/3 stretch, e.g. 1080p on a 1440p screen)
    double period2r = 0; // strength of a 2-pixel rhythm (to tell a 2x stretch from 4/3)
    double hiBand = 0; // detail at 0.2-0.3 cycles/px vs 0.03-0.15: a 3x stretch removes it, 1.5x keeps it
    bool sharpPhase = false; // odd phase of the 3-px rhythm is the sharpest: 3x, else 1.5x
    double topDetail = 0; // detail at 0.45-0.5 cycles/px vs 0.03-0.15, per unit bandwidth
    double factor = 1.0; // detected stretch factor; 1.0 = native
};

// rgba: kPatch*kPatch pixels, 4 bytes each, rows tightly packed.
// threshold: 3-pixel rhythm strength above which content counts as 3x/1.5x stretched.
// threshold2x: phase-lock (0..1) above which content counts as 2x stretched.
Result analyse(const uint8_t *rgba, double threshold, double threshold2x, double topNative = kTopDetailNative,
               double threshold43 = kLock43);

} // namespace resdetect
