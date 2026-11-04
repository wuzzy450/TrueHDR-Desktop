// Synthetic detector test: native 1/f image vs the same image shrunk and
// stretched back (bilinear and bicubic), plus flat and 1-px stripe patterns.
#include "../effect/resdetect.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace resdetect;
const int N = kPatch;
std::vector<float> nat(int n, unsigned seed) {
    std::mt19937 g(seed); std::uniform_real_distribution<float> u(0, 1);
    std::vector<float> im(n * n, 0);
    for (int c = 0; c < 1500; c++) {
        float f = 0.002f * powf(250, u(g)), th = u(g) * 6.283f, ph = u(g) * 6.283f, a = 40 / (f * 200 + 1);
        float fx = f * cosf(th), fy = f * sinf(th);
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) im[y * n + x] += a * sinf(6.283f * (fx * x + fy * y) + ph);
    }
    return im;
}
float samp(const std::vector<float> &s, int n, float x, float y) {
    x = std::clamp(x, 0.f, n - 1.001f); y = std::clamp(y, 0.f, n - 1.001f);
    int x0 = x, y0 = y; float fx = x - x0, fy = y - y0; auto at = [&](int a, int b) { return s[b * n + a]; };
    return (1 - fy) * ((1 - fx) * at(x0, y0) + fx * at(x0 + 1, y0)) + fy * ((1 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1));
}
float cr(float t) { t = fabsf(t); return t < 1 ? 1.5f * t * t * t - 2.5f * t * t + 1 : t < 2 ? -0.5f * t * t * t + 2.5f * t * t - 4 * t + 2 : 0; }
int MODE = 0; // 0 bilinear, 1 bicubic, 2 lanczos3
float lz(float t){t=fabsf(t); if(t<1e-6)return 1; if(t>=3)return 0; float a=3.14159265f*t; return 3*sinf(a)*sinf(a/3)/(a*a);}
float up(const std::vector<float> &s, int n, float x, float y) {
    if (MODE == 0) return samp(s, n, x, y);
    int x0 = floorf(x), y0 = floorf(y); float r = 0, w = 0; int R = MODE == 1 ? 2 : 3;
    for (int j = 1 - R; j <= R; j++) for (int i = 1 - R; i <= R; i++) {
        int xx = std::clamp(x0 + i, 0, n - 1), yy = std::clamp(y0 + j, 0, n - 1);
        float k = MODE == 1 ? cr(x - (x0 + i)) * cr(y - (y0 + j)) : lz(x - (x0 + i)) * lz(y - (y0 + j));
        r += s[yy * n + xx] * k; w += k;
    }
    return r / w;
}
std::vector<float> stretch(const std::vector<float> &src, double f) {
    int m = int(N / f + 0.999); std::vector<float> lo(m * m);
    for (int y = 0; y < m; y++) for (int x = 0; x < m; x++) {
        double s = 0; int c = 0;
        for (double yy = y * f; yy < (y + 1) * f - 1e-6; yy += 0.5) for (double xx = x * f; xx < (x + 1) * f - 1e-6; xx += 0.5) { s += samp(src, N, xx, yy); c++; }
        lo[y * m + x] = s / c;
    }
    std::vector<float> out(N * N);
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) out[y * N + x] = up(lo, m, (x + 0.5) / f - 0.5, (y + 0.5) / f - 0.5);
    return out;
}

Result analyseF(const std::vector<float> &im, unsigned seed) {
    std::vector<uint8_t> rgba(N * N * 4); std::mt19937 g(seed); std::uniform_real_distribution<float> d(-0.5, 0.5);
    for (int i = 0; i < N * N; i++) { int v = std::clamp(int(lroundf(128 + im[i] + d(g))), 0, 255); rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = v; rgba[4 * i + 3] = 255; }
    return analyse(rgba.data(), 0.4, 0.55, kTopDetailNative); // the effect's shipped defaults
}
int main() {
    // monitors of every common shape
    struct Screen { const char *name; int w, h; };
    Screen screens[] = {
        {"4K 16:9", 3840, 2160}, {"1440p 16:9", 2560, 1440}, {"1080p 16:9", 1920, 1080},
        {"UW 3440x1440", 3440, 1440}, {"UW 2560x1080", 2560, 1080}, {"SUW 5120x1440", 5120, 1440},
        {"16:10 2560x1600", 2560, 1600}, {"16:10 1920x1200", 1920, 1200},
        {"4:3 1600x1200", 1600, 1200}, {"4:3 1280x960", 1280, 960}, {"4:3 1024x768", 1024, 768}, {"5:4 1280x1024", 1280, 1024},
    };
    // content: 16:9 video sizes + classic 4:3 game/video sizes
    struct Content { const char *name; int w, h; };
    Content contents[] = {{"2160p", 3840, 2160}, {"1440p", 2560, 1440}, {"1080p", 1920, 1080}, {"720p", 1280, 720},
                          {"480p 16:9", 854, 480}, {"4:3 1024x768", 1024, 768}, {"4:3 640x480", 640, 480}};
    const int seeds = 4;
    std::vector<std::vector<float>> imgs;
    for (int s = 0; s < seeds; s++) imgs.push_back(nat(N, 10 + s));
    printf("%-16s %-13s %6s %-9s | %-12s %-12s %-12s\n", "screen", "content", "ratio", "expect", "bilinear", "bicubic", "lanczos");
    int bad = 0, total = 0;
    for (auto &sc : screens) for (auto &ct : contents) {
        const double f = std::min(double(sc.w) / ct.w, double(sc.h) / ct.h); // fit inside, black bars
        if (f < 0.999) continue; // content bigger than screen: shrunk, nothing to upscale
        // supported stretches; anything else should stay native (TrueHDR only)
        const double sup[] = {4.0 / 3, 1.5, 2, 3};
        double expect = 1;
        for (double v : sup) if (fabs(f - v) < 0.02) expect = v;
        printf("%-16s %-13s %6.3f %-9s |", sc.name, ct.name, f, expect > 1 ? "upscale" : "native");
        for (MODE = 0; MODE < 3; MODE++) {
            int hit = 0, wrong = 0;
            for (int s = 0; s < seeds; s++) {
                auto r = analyseF(f > 1.001 ? stretch(imgs[s], f) : imgs[s], s);
                if (fabs(r.factor - expect) < 0.05) hit++;
                else if (r.factor > 1.01) wrong++; // upscaling with the wrong factor = harmful
            }
            total++;
            if (hit < seeds) bad++;
            printf(" %d/%d%s       ", hit, seeds, wrong ? "!" : " ");
        }
        printf("\n");
    }
    printf("\n%d of %d cells not fully right ('!' = upscaled with a WRONG factor; otherwise it just stayed native)\n", bad, total);
}
