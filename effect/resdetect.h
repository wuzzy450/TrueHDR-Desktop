#pragma once
// Estimates the real resolution of content that has been stretched to fill the
// screen (e.g. a 1080p Netflix stream that Chrome scaled up to 4K).
//
// How: stretching an image by a fixed factor leaves a repeating rhythm in its
// pixel-to-pixel sharpness (every 3 px for 720p or 1440p->4K), and a 2x
// stretch (1080p->4K) leaves mirrored, phase-locked echoes in the spectrum.
// We measure both on a patch of the frame; native content has neither. Pure CPU code, no KWin or CUDA, so it can be tested on its own.
// Returned factor: 1 = native, 1.5 = 1440p, 2 = 1080p, 3 = 720p (on 2160 lines).

#include <cstdint>
#include <vector>

namespace resdetect
{

constexpr int kPatch = 512; // patch side in pixels (power of two)
constexpr double kTopDetailNative = 0.008; // default for DetectTopNative: topDetail above this means native. Real YouTube 4K sits ~0.001, stretched 1440p up to 0.006 (2026-10-25)

struct Result
{
    bool textured = false; // false: patch too flat to judge (black frame, credits...)
    double period2 = 0; // phase-lock between mirrored spectrum bins (2x stretch), 0..1
    double period3 = 0; // strength of a 3-pixel rhythm (3x or 1.5x stretch)
    bool sharpPhase = false; // odd phase of the 3-px rhythm is the sharpest: 3x, else 1.5x
    double topDetail = 0; // detail at 0.45-0.5 cycles/px vs 0.03-0.15, per unit bandwidth
    double factor = 1.0; // detected stretch factor; 1.0 = native
};

// rgba: kPatch*kPatch pixels, 4 bytes each, rows tightly packed.
// threshold: 3-pixel rhythm strength above which content counts as 3x/1.5x stretched.
// threshold2x: phase-lock (0..1) above which content counts as 2x stretched.
Result analyse(const uint8_t *rgba, double threshold, double threshold2x, double topNative = kTopDetailNative);

} // namespace resdetect
