// Prints the detector's readings for 512x512 RGBA crops: analyse_raw a.raw b.raw ...
#include "../effect/resdetect.h"
#include <cstdio>
#include <vector>
int main(int argc, char **argv) {
    std::vector<uint8_t> buf(resdetect::kPatch * resdetect::kPatch * 4);
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f || fread(buf.data(), 1, buf.size(), f) != buf.size()) { printf("%s: unreadable\n", argv[i]); continue; }
        fclose(f);
        auto r = resdetect::analyse(buf.data(), 0.4, 0.55);
        printf("%s tex=%d p2=%.3f p2r=%.3f p3=%.3f p4=%.3f l43=%.3f lg=%.3f sharp=%d top=%.4f hi=%.4f factor=%.3f\n", argv[i], r.textured, r.period2, r.period2r, r.period3, r.period4, r.lock43, r.lockGrid, int(r.sharpPhase), r.topDetail, r.hiBand, r.factor);
    }
}
