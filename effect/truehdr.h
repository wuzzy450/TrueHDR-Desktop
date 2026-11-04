#pragma once

#include "effect/effect.h"
#include "effect/effecthandler.h"
#include "opengl/glframebuffer.h"
#include "opengl/gltexture.h"
#include "core/colorspace.h"

#include "resdetect.h"
#include "blackfloor.h"

#include <chrono>
#include <deque>
#include <future>
#include <map>
#include <memory>

struct cudaGraphicsResource;
struct CUstream_st;
struct NvCVImage;
struct NvVFX_Object;

namespace KWin
{

class TrueHdrEffect : public Effect
{
    Q_OBJECT
public:
    // Video Super Resolution objects; owned by a shared_ptr so a load that is no
    // longer wanted can finish in the background and be cleaned up afterwards.
    struct Vsr
    {
        NvCVImage *in = nullptr; // content at its own resolution
        NvCVImage *out = nullptr; // upscaled, screen size
        NvVFX_Object *fx = nullptr;
        ~Vsr();
    };

    TrueHdrEffect();
    ~TrueHdrEffect() override;

    static bool supported();

    void reconfigure(ReconfigureFlags flags) override;
    void prePaintScreen(ScreenPrePaintData &data) override;
    void paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &region, LogicalOutput *screen) override;
    bool isActive() const override;
    bool blocksDirectScanout() const override;
    int requestedEffectChainPosition() const override
    {
        return 1; // outermost: see the finished frame, after every other effect
    }

private Q_SLOTS:
    void toggle();

private:
    struct Target
    {
        QSize size;
        std::unique_ptr<GLTexture> sceneTex; // what KWin painted, in the output's blending space
        std::unique_ptr<GLFramebuffer> sceneFbo;
        std::unique_ptr<GLTexture> sdrTex; // 8-bit sRGB copy fed to TrueHDR
        std::unique_ptr<GLFramebuffer> sdrFbo;
        std::unique_ptr<GLTexture> hdrTex; // TrueHDR result, PQ-encoded BT.2020 as half floats
        std::unique_ptr<GLTexture> fixTex; // hdrTex after the black floor fix (only when enabled)
        std::unique_ptr<GLFramebuffer> fixFbo;
        cudaGraphicsResource *sdrRes = nullptr;
        cudaGraphicsResource *hdrRes = nullptr;
        NvCVImage *in = nullptr;
        NvCVImage *out = nullptr;
        NvCVImage *half = nullptr;
        NvVFX_Object *fx = nullptr;
        bool paramsDirty = true;

        // resolution detector: async read-back of a patch of sdrTex, analysed off-thread
        GLuint pbo = 0;
        bool pboPending = false;
        std::chrono::steady_clock::time_point lastStep{}; // detector pacing, refresh-rate independent
        std::future<resdetect::Result> job;
        double factor = 1.0; // current decision (1 = native, no upscaling)
        double candidate = 1.0;
        int streak = 0;
        std::deque<double> recent; // last few detector readings, for a majority vote
        int nativeRun = 0; // consecutive "native" readings while upscaling (to drop back only after ~8 s)

        // AI upscaling: sdrTex -> lowTex (source resolution) -> VSR -> TrueHDR input
        double vsrFactor = 0; // factor the VSR objects below are built for (1 = same-size cleanup)
        uint vsrQuality = 0; // quality level they are built for
        std::unique_ptr<GLTexture> lowTex;
        std::unique_ptr<GLFramebuffer> lowFbo;
        cudaGraphicsResource *lowRes = nullptr;
        std::shared_ptr<Vsr> vsr; // built off the compositor thread (first load takes ~15 s)
        std::future<int> vsrLoad;
        bool vsrReady = false;
        std::chrono::steady_clock::time_point vsrLoadStart;
    };

    bool initCuda();
    Target *ensureTarget(LogicalOutput *screen, const QSize &size);
    void releaseTarget(Target &t);
    bool applyParams(Target &t);
    bool runTrueHdr(Target &t, bool upscale);
    void detect(Target &t, LogicalOutput *screen);
    bool fullscreenOn(LogicalOutput *screen) const;
    bool ensureVsr(Target &t, double factor, uint quality);
    void releaseVsr(Target &t);
    void drawTexture(GLTexture *tex, const RenderViewport &viewport, const std::shared_ptr<ColorDescription> &src,
                     const std::shared_ptr<ColorDescription> &dst, RenderingIntent intent);
    void fail(const QString &why);
    void failCuda(const QString &why);
    bool resetCuda();

    bool m_enabled = false;
    bool m_failed = false;
    bool m_cudaReady = false;
    CUstream_st *m_stream = nullptr;
    int m_device = 0;
    bool m_needReset = false; // a CUDA error happened: reset the context before reuse
    std::map<LogicalOutput *, Target> m_targets;
    std::vector<std::pair<std::future<int>, std::shared_ptr<Vsr>>> m_orphanLoads; // abandoned VSR loads
    void reapOrphans(bool wait);

    // settings (kwinrc [Effect-truehdr])
    uint m_luminance = 1000;
    uint m_contrast = 100;
    uint m_saturation = 100;
    uint m_middleGray = 50;
    bool m_debandingOff = false;
    bool m_onlyHdrOutputs = true;

    // upscaling settings
    bool m_upscale = true; // detect stretched fullscreen content and AI-upscale it
    uint m_upQuality = 4; // VSR QualityLevel for upscaling (4 = Ultra; 21 sharpens compression blocks)
    int m_cleanup = 1; // same-size VSR block cleanup when not upscaling: 0 off, 1 fullscreen only, 2 whole desktop
    uint m_cleanQuality = 2; // VSR QualityLevel for cleanup (2 = Medium, ~9 ms at 4K)
    uint m_upForce = 0; // 0 = detect; 720/1080/1440 = always treat content as that height (testing)
    double m_detectThreshold = 0.15; // 3-px rhythm strength for 720p/1440p
    int m_detectVotes = 7;
    double m_detect2xThreshold = 0.55; // spectral phase-lock for 1080p
    double m_detectTopNative = 0.008; // finest-detail veto: above this = native
    double m_detect43Threshold = 0.45; // 4/3 stretch (1080p on 1440p) lock score
    bool m_logDetect = false; // log every detector reading
    bool m_upscaleBroken = false; // VSR failed: keep TrueHDR, stop upscaling until next toggle
    BlackFloorSettings m_bff; // Lilium black floor fix, final pass
    std::unique_ptr<BlackFloorPass> m_bffPass;
};

} // namespace KWin
