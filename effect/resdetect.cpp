#include "resdetect.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace resdetect
{

namespace
{
using cpx = std::complex<float>;

// in-place radix-2 FFT, n a power of two
void fft(cpx *a, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * float(M_PI) / float(len);
        const cpx wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            cpx w(1.0f, 0.0f);
            for (int k = 0; k < len / 2; ++k) {
                const cpx u = a[i + k];
                const cpx v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// sum of power over frequencies [f0, f1) in cycles/pixel (0..0.5)
double band(const std::vector<double> &p, double f0, double f1)
{
    const int n = kPatch;
    double s = 0;
    for (int k = int(std::ceil(f0 * n)); k < n / 2 && k < f1 * n; ++k) {
        s += p[k];
    }
    return s;
}
} // namespace

Result analyse(const uint8_t *rgba, double threshold, double threshold2x, double topNative)
{
    const int n = kPatch;
    // luma, with the mean removed
    std::vector<float> y(n * n);
    double mean = 0;
    for (int i = 0; i < n * n; ++i) {
        const uint8_t *p = rgba + 4 * i;
        y[i] = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        mean += y[i];
    }
    mean /= n * n;

    std::vector<float> hann(n);
    for (int i = 0; i < n; ++i) {
        hann[i] = 0.5f - 0.5f * std::cos(2.0f * float(M_PI) * i / (n - 1));
    }

    // power spectrum averaged over all rows and all columns
    std::vector<double> power(n / 2, 0.0);
    std::vector<cpx> line(n);
    // 2x fingerprint: a 2x stretch makes the spectrum at f and at 0.5-f echoes of
    // each other (same source frequency), so bins k and n/2-k stay phase-locked
    // across every line. Native content gives random phases that average out.
    std::complex<double> pairSum[2];
    double pairMag[2] = {0, 0};
    for (int dir = 0; dir < 2; ++dir) {
        for (int r = 0; r < n; ++r) {
            for (int i = 0; i < n; ++i) {
                const float v = dir == 0 ? y[r * n + i] : y[i * n + r];
                line[i] = cpx((v - float(mean)) * hann[i], 0.0f);
            }
            fft(line.data(), n);
            for (int k = 1; k < n / 2; ++k) {
                power[k] += std::norm(line[k]);
            }
            for (int k = int(0.15 * n); k < n / 4; ++k) {
                const std::complex<double> a(line[k]), b(line[n / 2 - k]);
                pairSum[dir] += a * b;
                pairMag[dir] += std::abs(a) * std::abs(b);
            }
        }
    }

    Result res;
    // "textured": enough mid-frequency detail to judge. 8-bit rounding noise alone
    // gives roughly n*0.375/12 per bin per line; require clearly more than that.
    const double lines = 2.0 * n;
    const double noiseFloorPerBin = lines * (n * 0.375) / 12.0;
    const double mid = band(power, 0.03, 0.15) / (0.12 * n);
    res.textured = mid > 50.0 * noiseFloorPerBin;

    // Resampling fingerprint: when an image is stretched by a fixed factor the
    // new pixels are blends of the old ones, so local sharpness (|second
    // difference|) repeats with the stretch period: every 2 pixels for 2x
    // (1080p->4K), every 3 for 3x (720p->4K) and for 1.5x (1440p->4K, 2 source
    // pixels per 3 screen pixels). Native content has no such rhythm.
    bool oddIsSharp = false;
    auto periodicity = [&](int period) {
        std::vector<double> sum(period, 0.0);
        for (int r = 0; r < n; ++r) {
            for (int i = 1; i < n - 1; ++i) {
                const float h = std::fabs(y[r * n + i - 1] - 2 * y[r * n + i] + y[r * n + i + 1]); // along rows
                const float v = std::fabs(y[(i - 1) * n + r] - 2 * y[i * n + r] + y[(i + 1) * n + r]); // along columns
                sum[i % period] += h + v;
            }
        }
        double lo = sum[0], hi = sum[0], tot = 0;
        for (double v : sum) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            tot += v;
        }
        if (period == 3 && tot > 0) {
            // One of the three phases stands out from the other two (which mirror
            // each other). Measured with bilinear and bicubic stretches: for 3x the
            // odd phase is the sharpest, for 1.5x it is the softest.
            int odd = 0;
            double best = -1;
            for (int j = 0; j < 3; ++j) {
                const double others = (tot - sum[j]) / 2;
                if (std::fabs(sum[j] - others) > best) {
                    best = std::fabs(sum[j] - others);
                    odd = j;
                }
            }
            oddIsSharp = sum[odd] > (tot - sum[odd]) / 2;
        }
        return tot > 0 ? (hi - lo) / (tot / period) : 0.0;
    };
    res.period2 = 0;
    for (int dir = 0; dir < 2; ++dir) {
        res.period2 += pairMag[dir] > 0 ? 0.5 * std::abs(pairSum[dir]) / pairMag[dir] : 0.0;
    }
    res.period3 = periodicity(3);
    res.sharpPhase = oddIsSharp;
    // Finest-detail check: every stretch filter (bilinear, bicubic, Lanczos)
    // removes almost everything near the screen's own pixel frequency (0.5
    // cycles/px). Strong detail there (1-px stripes, sharp native text) means
    // the content is native, whatever rhythm it might also show.
    const double base = band(power, 0.03, 0.15);
    res.topDetail = base > 0 ? band(power, 0.45, 0.5) / base * (0.12 / 0.05) : 0.0;

    if (res.textured && res.topDetail < topNative) {
        // 2x first: 3x/1.5x stretches barely register on period2 (<0.1), while a
        // 2x stretch can show a weak 3-px rhythm near the threshold.
        if (res.period2 > threshold2x) {
            res.factor = 2.0;
        } else if (res.period3 > threshold) {
            res.factor = oddIsSharp ? 3.0 : 1.5;
        }
    }
    return res;
}

} // namespace resdetect
