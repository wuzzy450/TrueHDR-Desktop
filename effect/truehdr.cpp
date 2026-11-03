// TrueHDR Desktop: runs NVIDIA's RTX TrueHDR (Maxine VFX SDK) over the whole
// composited frame of each HDR output, inside KWin. Because it is part of the
// compositor there is no overlay window: input goes straight to apps and games.
//
// Per frame on an HDR output:
//   1. let KWin paint the scene into sceneTex (output's blending colour space)
//   2. convert to 8-bit sRGB SDR (sdrTex)                      [KWin shader]
//   2b. if a fullscreen window shows stretched content (720p/1080p/1440p on a
//       4K screen, see resdetect.cpp): shrink sdrTex back to that size (lowTex)
//       and let NVIDIA Video Super Resolution rebuild the 4K frame from it
//   3. CUDA: sdrTex (or the VSR result) -> TrueHDR -> RGB10A2 PQ -> half floats -> hdrTex
//   4. draw hdrTex (BT.2020 PQ, absolute nits) onto the real output
//
// The SDK and its private CUDA 12.8 runtime are loaded by full path with dlopen,
// so nothing else on the system sees them, and a missing SDK just disables the
// effect instead of breaking KWin.

#include "core/backendoutput.h"
#include "truehdr.h"

#include "core/output.h"
#include "effect/effectwindow.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"

#include <KConfigGroup>
#include <QDir>
#include <QFile>
#include <KGlobalAccel>
#include <KSharedConfig>
#include <QAction>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QScopeGuard>
#include <QStandardPaths>

#include <cuda_runtime_api.h>
#include "nvVideoEffects.h"

#include <chrono>
#include <dlfcn.h>
#include <epoxy/gl.h>

namespace KWin
{

namespace
{
// ---- runtime-loaded entry points -------------------------------------------
struct Api
{
    // CUDA runtime (the SDK's bundled copy)
    cudaError_t (*cudaSetDevice)(int);
    cudaError_t (*cudaGLGetDevices)(unsigned *, int *, unsigned, int);
    cudaError_t (*cudaStreamCreate)(cudaStream_t *);
    cudaError_t (*cudaStreamDestroy)(cudaStream_t);
    cudaError_t (*cudaStreamSynchronize)(cudaStream_t);
    cudaError_t (*cudaDeviceReset)();
    cudaError_t (*cudaGraphicsGLRegisterImage)(cudaGraphicsResource **, GLuint, GLenum, unsigned);
    cudaError_t (*cudaGraphicsUnregisterResource)(cudaGraphicsResource *);
    cudaError_t (*cudaGraphicsMapResources)(int, cudaGraphicsResource **, cudaStream_t);
    cudaError_t (*cudaGraphicsUnmapResources)(int, cudaGraphicsResource **, cudaStream_t);
    cudaError_t (*cudaGraphicsSubResourceGetMappedArray)(cudaArray_t *, cudaGraphicsResource *, unsigned, unsigned);
    cudaError_t (*cudaMemcpy2DFromArrayAsync)(void *, size_t, cudaArray_const_t, size_t, size_t, size_t, size_t, cudaMemcpyKind, cudaStream_t);
    cudaError_t (*cudaMemcpy2DToArrayAsync)(cudaArray_t, size_t, size_t, const void *, size_t, size_t, size_t, cudaMemcpyKind, cudaStream_t);
    const char *(*cudaGetErrorString)(cudaError_t);
    // VideoFX / NvCVImage
    decltype(&NvVFX_CreateEffect) CreateEffect;
    decltype(&NvVFX_DestroyEffect) DestroyEffect;
    decltype(&NvVFX_SetImage) SetImage;
    decltype(&NvVFX_SetU32) SetU32;
    decltype(&NvVFX_SetString) SetString;
    decltype(&NvVFX_SetCudaStream) SetCudaStream;
    decltype(&NvVFX_Load) Load;
    decltype(&NvVFX_Run) Run;
    decltype(&NvCVImage_Create) ImageCreate;
    decltype(&NvCVImage_Destroy) ImageDestroy;
    decltype(&NvCVImage_Transfer) ImageTransfer;
    decltype(&NvCV_GetErrorStringFromCode) StatusString;
};
Api api;
bool apiLoaded = false;
bool vsrAvailable = false; // Video Super Resolution feature installed

constexpr unsigned kRegReadOnly = 1; // cudaGraphicsRegisterFlagsReadOnly
constexpr unsigned kRegWriteDiscard = 2; // cudaGraphicsRegisterFlagsWriteDiscard
constexpr int kGLDeviceListAll = 1; // cudaGLDeviceListAll

QString sdkRoot()
{
    // first match wins: kwinrc [Effect-truehdr] SdkPath, user install, NVIDIA's default, build-time path
    static QString cached;
    if (!cached.isEmpty()) {
        return cached;
    }
    QStringList dirs;
    const QString cfg = KSharedConfig::openConfig(QStringLiteral("kwinrc"))->group(QStringLiteral("Effect-truehdr")).readEntry("SdkPath", QString());
    if (!cfg.isEmpty()) {
        dirs << cfg;
    }
    const QByteArray xdg = qgetenv("XDG_DATA_HOME");
    dirs << (xdg.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : QString::fromLocal8Bit(xdg)) + QStringLiteral("/truehdr-desktop/VideoFX");
    dirs << QStringLiteral("/opt/truehdr-desktop/VideoFX") << QStringLiteral("/usr/local/VideoFX") << QStringLiteral(VFX_SDK_DIR);
    for (const QString &d : dirs) {
        if (QFile::exists(d + QStringLiteral("/lib/libVideoFX.so"))) {
            cached = d + QLatin1Char('/');
            return cached;
        }
    }
    return dirs.first() + QLatin1Char('/');
}

QString loadApi()
{
    if (apiLoaded) {
        return {};
    }
    const QString root = sdkRoot();
    static const char *libs[] = {
        "external/cuda/lib/libcudart.so.12",
        "external/cuda/lib/libcublasLt.so.12",
        "external/cuda/lib/libcublas.so.12",
        "external/cuda/lib/libcudnn.so.9",
        "lib/libNVCVImage.so",
        "lib/libnvngxruntime.so",
        "lib/libVideoFXLocal.so",
        "lib/libVideoFX.so",
        "features/nvvfxtruehdr/lib/libnvVFXTrueHDR.so",
    };
    void *cudart = nullptr;
    void *vfx = nullptr;
    void *cvimg = nullptr;
    for (const char *lib : libs) {
        void *h = dlopen((root + QLatin1String(lib)).toLocal8Bit().constData(), RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            return QStringLiteral("cannot load %1: %2").arg(QLatin1String(lib), QLatin1String(dlerror()));
        }
        const QByteArray name(lib);
        if (name.endsWith("libcudart.so.12")) {
            cudart = h;
        } else if (name.endsWith("libVideoFX.so")) {
            vfx = h;
        } else if (name.endsWith("libNVCVImage.so")) {
            cvimg = h;
        }
    }
    bool ok = true;
    auto get = [&ok](void *h, const char *sym) {
        void *p = dlsym(h, sym);
        if (!p) {
            qWarning("truehdr: missing symbol %s", sym);
            ok = false;
        }
        return p;
    };
#define CU(n) api.n = reinterpret_cast<decltype(api.n)>(get(cudart, #n))
    CU(cudaSetDevice);
    CU(cudaGLGetDevices);
    CU(cudaStreamCreate);
    CU(cudaStreamDestroy);
    CU(cudaStreamSynchronize);
    CU(cudaDeviceReset);
    CU(cudaGraphicsGLRegisterImage);
    CU(cudaGraphicsUnregisterResource);
    CU(cudaGraphicsMapResources);
    CU(cudaGraphicsUnmapResources);
    CU(cudaGraphicsSubResourceGetMappedArray);
    CU(cudaMemcpy2DFromArrayAsync);
    CU(cudaMemcpy2DToArrayAsync);
    CU(cudaGetErrorString);
#undef CU
#define VF(f, n) api.f = reinterpret_cast<decltype(api.f)>(get(vfx, #n))
    VF(CreateEffect, NvVFX_CreateEffect);
    VF(DestroyEffect, NvVFX_DestroyEffect);
    VF(SetImage, NvVFX_SetImage);
    VF(SetU32, NvVFX_SetU32);
    VF(SetString, NvVFX_SetString);
    VF(SetCudaStream, NvVFX_SetCudaStream);
    VF(Load, NvVFX_Load);
    VF(Run, NvVFX_Run);
#undef VF
#define CV(f, n) api.f = reinterpret_cast<decltype(api.f)>(get(cvimg, #n))
    CV(ImageCreate, NvCVImage_Create);
    CV(ImageDestroy, NvCVImage_Destroy);
    CV(ImageTransfer, NvCVImage_Transfer);
    CV(StatusString, NvCV_GetErrorStringFromCode);
#undef CV
    if (!ok) {
        return QStringLiteral("SDK is missing expected functions");
    }
    // optional: the upscaler. Without it TrueHDR still works, just no upscaling.
    const QString vsrLib = root + QLatin1String("features/nvvfxvideosuperres/lib/libnvVFXVideoSuperRes.so");
    if (dlopen(vsrLib.toLocal8Bit().constData(), RTLD_NOW | RTLD_GLOBAL)) {
        vsrAvailable = true;
    } else {
        qWarning("truehdr: upscaling unavailable: %s", dlerror());
    }
    apiLoaded = true;
    return {};
}

// e.g. "1080p (2x)"; source height depends on the screen, so work it out from it
QByteArray heightLabel(double factor, int screenHeight)
{
    if (factor < 1.01) {
        return QByteArrayLiteral("native");
    }
    return QByteArray::number(qRound(screenHeight / factor)) + "p (" + QByteArray::number(factor, 'g', 2) + "x)";
}

// TrueHDR writes absolute luminance in PQ; describe it so KWin maps nits 1:1.
std::shared_ptr<ColorDescription> pqDescription(double peak)
{
    return std::make_shared<ColorDescription>(Colorimetry::BT2020, TransferFunction(TransferFunction::PerceptualQuantizer),
                                              203.0, 0.0, peak, peak);
}
} // namespace

// ---------------------------------------------------------------------------

TrueHdrEffect::Vsr::~Vsr()
{
    if (fx) {
        api.DestroyEffect(fx);
    }
    for (NvCVImage *im : {in, out}) {
        if (im) {
            api.ImageDestroy(im);
        }
    }
}

// Loaded VSR effects are kept for the life of KWin and reused: NVIDIA's VFX SDK
// does not return most of an effect's GPU memory when it is destroyed (measured
// ~650 MB leaked per 4K load), so rebuilding on every fullscreen/size switch
// grew KWin to 7+ GB. Keyed by input size, output size and quality level.
namespace
{
struct VsrKey
{
    int lw, lh, fw, fh;
    uint q;
    bool operator==(const VsrKey &) const = default;
};
std::vector<std::pair<VsrKey, std::shared_ptr<TrueHdrEffect::Vsr>>> g_vsrCache;
}

void TrueHdrEffect::reapOrphans(bool wait)
{
    if (m_orphanLoads.empty()) {
        return;
    }
    if (m_stream) {
        api.cudaStreamSynchronize(m_stream);
    }
    std::erase_if(m_orphanLoads, [wait](auto &o) {
        if (!wait && o.first.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return false;
        }
        o.first.wait();
        return true; // ~Vsr frees it (its load may have queued work: callers sync first)
    });
}

bool TrueHdrEffect::supported()
{
    return effects->isOpenGLCompositing();
}

TrueHdrEffect::TrueHdrEffect()
{
    QAction *a = new QAction(this);
    a->setObjectName(QStringLiteral("ToggleTrueHDR"));
    a->setText(QStringLiteral("Toggle TrueHDR Desktop"));
    KGlobalAccel::self()->setDefaultShortcut(a, {Qt::META | Qt::CTRL | Qt::Key_H});
    KGlobalAccel::self()->setShortcut(a, {Qt::META | Qt::CTRL | Qt::Key_H});
    connect(a, &QAction::triggered, this, &TrueHdrEffect::toggle);

    reconfigure(ReconfigureAll);
}

TrueHdrEffect::~TrueHdrEffect()
{
    effects->makeOpenGLContextCurrent();
    for (auto &[output, t] : m_targets) {
        releaseTarget(t);
    }
    m_targets.clear();
    m_bffPass.reset();
    reapOrphans(true);
    if (m_stream) {
        api.cudaStreamDestroy(m_stream);
    }
}

void TrueHdrEffect::reconfigure(ReconfigureFlags)
{
    KSharedConfig::Ptr cfg = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    cfg->reparseConfiguration();
    const KConfigGroup g = cfg->group(QStringLiteral("Effect-truehdr"));
    m_luminance = std::clamp(g.readEntry("Luminance", 1000u), 400u, 2000u);
    m_contrast = std::clamp(g.readEntry("Contrast", 100u), 0u, 200u);
    m_saturation = std::clamp(g.readEntry("Saturation", 100u), 0u, 200u);
    m_middleGray = std::clamp(g.readEntry("MiddleGray", 50u), 10u, 100u);
    m_debandingOff = g.readEntry("DebandingOff", false);
    m_onlyHdrOutputs = g.readEntry("OnlyHdrOutputs", true);
    m_upscale = g.readEntry("Upscale", true);
    m_upQuality = g.readEntry("UpscaleQuality", 17u);
    {
        const QString cl = g.readEntry("Cleanup", QStringLiteral("off"));
        m_cleanup = cl == QLatin1String("off") ? 0 : cl == QLatin1String("always") ? 2 : 1;
    }
    m_cleanQuality = g.readEntry("CleanupQuality", 2u);
    m_upForce = g.readEntry("UpscaleForce", 0u);
    if (m_upForce && m_upForce < 360) {
        m_upForce = 0;
    }
    m_detectThreshold = g.readEntry("DetectThreshold", 0.4);
    m_detectVotes = std::clamp(g.readEntry("DetectVotes", 7), 5, 8);
    m_detect2xThreshold = g.readEntry("Detect2xThreshold", 0.55);
    m_detectTopNative = g.readEntry("DetectTopNative", resdetect::kTopDetailNative);
    m_bff.read(g);
    qInfo("truehdr: blackfloor g22=%d(%.0f nits%s) gamma=%d(%.3f, %.0f nits) lower=%d(mode %d, %.4f -> %.4f nits, rolloff %.1f)", int(m_bff.g22), m_bff.g22Cutoff, m_bff.g22OnlyLower ? ", only lower" : "", int(m_bff.gamma), m_bff.gammaValue, m_bff.gammaCutoff, int(m_bff.lower), m_bff.lowerMode, m_bff.oldBlack, m_bff.newBlack, m_bff.rollOff);
    m_logDetect = g.readEntry("LogDetect", false);
    const bool startEnabled = g.readEntry("EnabledAtStart", true);
    static bool first = true;
    if (first) {
        m_enabled = startEnabled;
        first = false;
    }
    effects->makeOpenGLContextCurrent();
    for (auto &[output, t] : m_targets) {
        t.paramsDirty = true;
        releaseVsr(t); // quality level may have changed
        t.factor = t.candidate = 1.0;
        t.streak = 0;
        t.recent.clear();
    }
    m_upscaleBroken = false;
    qInfo("truehdr: luminance=%u contrast=%u saturation=%u middleGray=%u debandingOff=%d upscale=%d quality=%u force=%u cleanup=%d(quality %u)",
          m_luminance, m_contrast, m_saturation, m_middleGray, int(m_debandingOff), int(m_upscale), m_upQuality, m_upForce, m_cleanup, m_cleanQuality);
    effects->addRepaintFull();
}

void TrueHdrEffect::toggle()
{
    m_enabled = !m_enabled;
    if (!m_enabled) {
        // give the VRAM back while off
        effects->makeOpenGLContextCurrent();
        for (auto &[output, t] : m_targets) {
            releaseTarget(t);
        }
        m_targets.clear();
    }
    m_failed = false;
    m_upscaleBroken = false;
    qInfo("truehdr: %s", m_enabled ? "on" : "off");
    effects->addRepaintFull();
}

bool TrueHdrEffect::isActive() const
{
    return m_enabled && !m_failed;
}

bool TrueHdrEffect::blocksDirectScanout() const
{
    // fullscreen games would otherwise bypass composition (and this effect)
    return isActive();
}

void TrueHdrEffect::failCuda(const QString &why)
{
    m_needReset = true; // CUDA errors can be sticky: rebuild the whole CUDA context on retry
    fail(why);
}

void TrueHdrEffect::fail(const QString &why)
{
    qWarning("truehdr: disabled: %s", qPrintable(why));
    m_failed = true; // resources are released at the start of the next paint
    effects->addRepaintFull();
}

bool TrueHdrEffect::resetCuda()
{
    // Only reachable with every target released (see paintScreen/toggle). KWin's
    // GL never uses CUDA, so resetting this process's CUDA context is safe.
    reapOrphans(true);
    if (m_stream) {
        api.cudaStreamDestroy(m_stream);
        m_stream = nullptr;
    }
    m_cudaReady = false;
    g_vsrCache.clear();
    const cudaError_t e = api.cudaDeviceReset();
    qWarning("truehdr: reset CUDA context after an error: %s", api.cudaGetErrorString(e));
    m_needReset = false;
    return e == cudaSuccess;
}

bool TrueHdrEffect::initCuda()
{
    if (m_cudaReady && !m_needReset) {
        return true;
    }
    if (apiLoaded && (m_needReset || m_cudaReady)) {
        if (!m_targets.empty()) {
            return false; // resources still held; paintScreen releases them first
        }
        resetCuda();
    }
    if (const QString err = loadApi(); !err.isEmpty()) {
        fail(err);
        return false;
    }
    // use whichever GPU is driving KWin's GL context (matters on multi-GPU machines)
    unsigned count = 0;
    int devs[4] = {};
    cudaError_t e = api.cudaGLGetDevices(&count, devs, 4, kGLDeviceListAll);
    if (e != cudaSuccess || count == 0) {
        fail(QStringLiteral("cudaGLGetDevices: %1").arg(QLatin1String(api.cudaGetErrorString(e))));
        return false;
    }
    e = api.cudaSetDevice(devs[0]);
    if (e == cudaSuccess) {
        e = api.cudaStreamCreate(&m_stream);
    }
    if (e != cudaSuccess && resetCuda()) {
        // a sticky error left over from earlier: one fresh attempt
        e = api.cudaSetDevice(devs[0]);
        if (e == cudaSuccess) {
            e = api.cudaStreamCreate(&m_stream);
        }
    }
    if (e != cudaSuccess) {
        m_stream = nullptr;
        failCuda(QStringLiteral("CUDA init: %1").arg(QLatin1String(api.cudaGetErrorString(e))));
        return false;
    }
    m_device = devs[0];
    qInfo("truehdr: using CUDA device %d", devs[0]);
    m_cudaReady = true;
    return true;
}

void TrueHdrEffect::releaseVsr(Target &t)
{
    if (t.vsrLoad.valid()) {
        // still loading: let it finish on its own, free it later (never block the compositor)
        m_orphanLoads.emplace_back(std::move(t.vsrLoad), t.vsr);
        t.vsrLoad = {};
    }
    if (t.vsr && t.vsrReady && m_stream) {
        // the last frame's VSR pass may still be running on the GPU; freeing its
        // buffers under it caused an MMU fault (Xid 31) that killed CUDA in KWin
        api.cudaStreamSynchronize(m_stream);
    }
    if (t.vsr && t.vsrReady && t.lowTex) {
        g_vsrCache.emplace_back(VsrKey{t.lowTex->size().width(), t.lowTex->size().height(), t.size.width(), t.size.height(), t.vsrQuality}, t.vsr);
    }
    t.vsr.reset();
    t.vsrReady = false;
    if (t.lowRes) {
        api.cudaGraphicsUnregisterResource(t.lowRes);
    }
    t.lowRes = nullptr;
    t.lowFbo.reset();
    t.lowTex.reset();
    t.vsrFactor = 0;
}

void TrueHdrEffect::releaseTarget(Target &t)
{
    if (t.job.valid()) {
        t.job.wait(); // the analysis thread only touches its own copy of the pixels
    }
    if (t.pbo) {
        glDeleteBuffers(1, &t.pbo);
    }
    if (m_stream && m_cudaReady) {
        api.cudaStreamSynchronize(m_stream); // nothing may still be using these buffers
    }
    releaseVsr(t);
    if (t.sdrRes) {
        api.cudaGraphicsUnregisterResource(t.sdrRes);
    }
    if (t.hdrRes) {
        api.cudaGraphicsUnregisterResource(t.hdrRes);
    }
    if (t.fx) {
        api.DestroyEffect(t.fx);
    }
    for (NvCVImage *im : {t.in, t.out, t.half}) {
        if (im) {
            api.ImageDestroy(im);
        }
    }
    t = Target{};
}

TrueHdrEffect::Target *TrueHdrEffect::ensureTarget(LogicalOutput *screen, const QSize &size)
{
    Target &t = m_targets[screen];
    if (t.size == size && t.fx) {
        return &t;
    }
    releaseTarget(t);
    t.size = size;
    const unsigned w = size.width();
    const unsigned h = size.height();

    auto makeTex = [&](GLenum fmt) {
        auto tex = GLTexture::allocate(fmt, size);
        if (tex) {
            tex->setFilter(GL_NEAREST);
            tex->setWrapMode(GL_CLAMP_TO_EDGE);
        }
        return tex;
    };
    t.sceneTex = makeTex(GL_RGBA16F);
    t.sdrTex = makeTex(GL_RGBA8);
    t.hdrTex = makeTex(GL_RGBA16F);
    if (!t.sceneTex || !t.sdrTex || !t.hdrTex) {
        fail(QStringLiteral("could not allocate GL textures"));
        return nullptr;
    }
    t.sceneFbo = std::make_unique<GLFramebuffer>(t.sceneTex.get());
    t.sdrFbo = std::make_unique<GLFramebuffer>(t.sdrTex.get());

    cudaError_t e = api.cudaGraphicsGLRegisterImage(&t.sdrRes, t.sdrTex->texture(), GL_TEXTURE_2D, kRegReadOnly);
    if (e == cudaSuccess) {
        e = api.cudaGraphicsGLRegisterImage(&t.hdrRes, t.hdrTex->texture(), GL_TEXTURE_2D, kRegWriteDiscard);
    }
    if (e != cudaSuccess) {
        failCuda(QStringLiteral("GL interop: %1").arg(QLatin1String(api.cudaGetErrorString(e))));
        return nullptr;
    }

    NvCV_Status s = api.ImageCreate(w, h, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32, &t.in);
    if (!s) {
        s = api.ImageCreate(w, h, NVCV_RGB10A2, NVCV_P32, NVCV_CHUNKY, NVCV_GPU, 1, &t.out);
    }
    if (!s) {
        s = api.ImageCreate(w, h, NVCV_RGBA, NVCV_F16, NVCV_CHUNKY, NVCV_GPU, 1, &t.half);
    }
    if (!s) {
        s = api.CreateEffect("TrueHDR", &t.fx);
    }
    if (!s) {
        s = api.SetImage(t.fx, NVVFX_INPUT_IMAGE, t.in);
    }
    if (!s) {
        s = api.SetImage(t.fx, NVVFX_OUTPUT_IMAGE, t.out);
    }
    if (!s) {
        s = api.SetCudaStream(t.fx, NVVFX_CUDA_STREAM, m_stream);
    }
    if (s) {
        fail(QStringLiteral("TrueHDR setup: %1").arg(QLatin1String(api.StatusString(s))));
        return nullptr;
    }
    t.paramsDirty = true;
    qInfo("truehdr: ready for %s at %ux%u", qPrintable(screen->name()), w, h);
    return &t;
}

bool TrueHdrEffect::applyParams(Target &t)
{
    if (!t.paramsDirty) {
        return true;
    }
    NvCV_Status s = api.SetU32(t.fx, "Luminance", m_luminance);
    if (!s) {
        s = api.SetU32(t.fx, "Contrast", m_contrast);
    }
    if (!s) {
        s = api.SetU32(t.fx, "Saturation", m_saturation);
    }
    if (!s) {
        s = api.SetU32(t.fx, "MiddleGray", m_middleGray);
    }
    if (!s) {
        s = api.SetU32(t.fx, "DebandingOff", m_debandingOff ? 1 : 0);
    }
    if (!s) {
        s = api.Load(t.fx);
    }
    if (s) {
        fail(QStringLiteral("TrueHDR params: %1").arg(QLatin1String(api.StatusString(s))));
        return false;
    }
    t.paramsDirty = false;
    return true;
}

bool TrueHdrEffect::runTrueHdr(Target &t, bool upscale)
{
    cudaGraphicsResource *res[3] = {t.sdrRes, t.hdrRes, t.lowRes};
    const int nres = upscale ? 3 : 2;
    cudaError_t e = api.cudaGraphicsMapResources(nres, res, m_stream); // waits for the GL passes above
    if (e != cudaSuccess) {
        failCuda(QStringLiteral("map: %1").arg(QLatin1String(api.cudaGetErrorString(e))));
        return false;
    }
    cudaArray_t sdrArr = nullptr;
    cudaArray_t hdrArr = nullptr;
    api.cudaGraphicsSubResourceGetMappedArray(&sdrArr, t.sdrRes, 0, 0);
    api.cudaGraphicsSubResourceGetMappedArray(&hdrArr, t.hdrRes, 0, 0);

    const size_t w = t.size.width();
    const size_t h = t.size.height();
    NvCV_Status s = NVCV_SUCCESS;
    QString vsrError;
    bool upscaled = false;
    if (upscale) {
        // lowTex (content at its own resolution) -> VSR -> t.in at full size
        cudaArray_t lowArr = nullptr;
        api.cudaGraphicsSubResourceGetMappedArray(&lowArr, t.lowRes, 0, 0);
        NvCVImage *vin = t.vsr->in;
        e = api.cudaMemcpy2DFromArrayAsync(vin->pixels, vin->pitch, lowArr, 0, 0, size_t(vin->width) * 4, vin->height,
                                           cudaMemcpyDeviceToDevice, m_stream);
        if (e == cudaSuccess) {
            const NvCV_Status vs = api.Run(t.vsr->fx, 1);
            if (vs) {
                vsrError = QLatin1String(api.StatusString(vs));
            } else {
                s = api.ImageTransfer(t.vsr->out, t.in, 1.0f, m_stream, nullptr); // into TrueHDR's input
                upscaled = !s;
            }
        }
    }
    if (e == cudaSuccess && !upscaled) {
        e = api.cudaMemcpy2DFromArrayAsync(t.in->pixels, t.in->pitch, sdrArr, 0, 0, w * 4, h, cudaMemcpyDeviceToDevice, m_stream);
    }
    if (e == cudaSuccess) {
        s = api.Run(t.fx, 1);
    }
    if (e == cudaSuccess && !s) {
        s = api.ImageTransfer(t.out, t.half, 1.0f, m_stream, nullptr);
    }
    if (e == cudaSuccess && !s) {
        e = api.cudaMemcpy2DToArrayAsync(hdrArr, 0, 0, t.half->pixels, t.half->pitch, w * 8, h, cudaMemcpyDeviceToDevice, m_stream);
    }
    api.cudaGraphicsUnmapResources(nres, res, m_stream); // GL drawing below waits for this
    if (!vsrError.isEmpty()) {
        // keep TrueHDR running, just stop upscaling (until the next toggle/reconfigure)
        qWarning("truehdr: upscaling disabled: VSR run: %s", qPrintable(vsrError));
        m_upscaleBroken = true;
        releaseVsr(t);
    }
    if (e != cudaSuccess || s) {
        failCuda(e != cudaSuccess ? QStringLiteral("CUDA: %1").arg(QLatin1String(api.cudaGetErrorString(e)))
                              : QStringLiteral("TrueHDR run: %1").arg(QLatin1String(api.StatusString(s))));
        return false;
    }
    return true;
}

bool TrueHdrEffect::fullscreenOn(LogicalOutput *screen) const
{
    const EffectWindow *w = effects->activeWindow();
    return w && w->isFullScreen() && !w->isMinimized() && w->screen() == screen;
}

void TrueHdrEffect::detect(Target &t, LogicalOutput *screen)
{
    constexpr int n = resdetect::kPatch;
    // 1. pick up a finished analysis and update the decision
    if (t.job.valid() && t.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const resdetect::Result r = t.job.get();
        if (m_logDetect) {
            qInfo("truehdr: detect textured=%d p2=%.3f p3=%.3f top=%.4f sharp=%d -> %s", int(r.textured), r.period2, r.period3, r.topDetail,
                  int(r.sharpPhase), heightLabel(r.factor, t.size.height()).constData());
        }
        if (r.textured) { // flat frames (black, fades) say nothing; keep the last decision
            // majority vote over the last 8 readings (~3 s): change only when DetectVotes (default 7) agree,
            // so captions, menus or a borderline scene can't make it flip back and forth
            t.recent.push_back(r.factor);
            if (t.recent.size() > 8) {
                t.recent.pop_front();
            }
            std::map<double, int> votes;
            for (double f : t.recent) {
                ++votes[f];
            }
            for (const auto &[f, count] : votes) {
                if (count >= m_detectVotes && f != t.factor) {
                    t.factor = f;
                    qInfo("truehdr: %s content is %s%s", qPrintable(screen->name()), heightLabel(t.factor, t.size.height()).constData(),
                          t.factor > 1.0 ? ", upscaling" : "");
                }
            }
        }
    }
    // 2. every ~1/6 s (whatever the refresh rate): either start an async read-back
    //    of the centre patch, or hand the finished read-back to a worker thread
    const auto now = std::chrono::steady_clock::now();
    if (now - t.lastStep < std::chrono::milliseconds(166) || t.job.valid() || t.size.width() < n || t.size.height() < n) {
        return;
    }
    t.lastStep = now;
    if (!t.pbo) {
        glGenBuffers(1, &t.pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, t.pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, n * n * 4, nullptr, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, t.pbo);
    if (t.pboPending) {
        const auto *p = static_cast<const uint8_t *>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, n * n * 4, GL_MAP_READ_BIT));
        if (p) {
            std::vector<uint8_t> pixels(p, p + n * n * 4);
            const double th = m_detectThreshold;
            const double th2 = m_detect2xThreshold;
            const double top = m_detectTopNative;
            t.job = std::async(std::launch::async, [pixels = std::move(pixels), th, th2, top] {
                return resdetect::analyse(pixels.data(), th, th2, top);
            });
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        t.pboPending = false;
    } else {
        GLFramebuffer::pushFramebuffer(t.sdrFbo.get());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels((t.size.width() - n) / 2, (t.size.height() - n) / 2, n, n, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        GLFramebuffer::popFramebuffer();
        t.pboPending = true;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

bool TrueHdrEffect::ensureVsr(Target &t, double factor, uint quality)
{
    auto give_up = [&](const QString &why) {
        qWarning("truehdr: upscaling disabled: %s", qPrintable(why));
        m_upscaleBroken = true;
        releaseVsr(t);
        return false;
    };
    // a background load finished?
    if (t.vsrLoad.valid()) {
        if (t.vsrLoad.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return false; // still loading: plain TrueHDR meanwhile
        }
        const int s = t.vsrLoad.get();
        if (s) {
            return give_up(QStringLiteral("VSR setup: %1").arg(QLatin1String(api.StatusString(NvCV_Status(s)))));
        }
        t.vsrReady = true;
        qInfo("truehdr: upscaler ready %dx%d -> %dx%d (quality %u, load %lld ms)", t.lowTex->size().width(),
              t.lowTex->size().height(), t.size.width(), t.size.height(), t.vsrQuality,
              (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t.vsrLoadStart).count());
    }
    if (t.vsrFactor == factor && t.vsrQuality == quality && (t.vsrReady || t.vsrLoad.valid())) {
        return t.vsrReady;
    }
    if (false) {
        return true;
    }
    releaseVsr(t);
    if (!vsrAvailable) {
        return give_up(QStringLiteral("Video Super Resolution feature not installed"));
    }
    // GL side on this thread
    const QSize low(qRound(t.size.width() / factor) & ~1, qRound(t.size.height() / factor) & ~1);
    t.lowTex = GLTexture::allocate(GL_RGBA8, low);
    if (!t.lowTex) {
        return give_up(QStringLiteral("could not allocate low-res texture"));
    }
    t.lowTex->setFilter(GL_NEAREST);
    t.lowTex->setWrapMode(GL_CLAMP_TO_EDGE);
    t.lowTex->setContentTransform(t.sdrTex->contentTransform());
    t.lowFbo = std::make_unique<GLFramebuffer>(t.lowTex.get());
    const cudaError_t e = api.cudaGraphicsGLRegisterImage(&t.lowRes, t.lowTex->texture(), GL_TEXTURE_2D, kRegReadOnly);
    if (e != cudaSuccess) {
        t.lowRes = nullptr;
        return give_up(QStringLiteral("GL interop: %1").arg(QLatin1String(api.cudaGetErrorString(e))));
    }
    // CUDA/VSR side on a worker thread. The built engine is cached on disk, so
    // the slow first build happens once per resolution, not every login.
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/truehdr-vsr");
    QDir().mkpath(cacheDir);
    t.vsrFactor = factor;
    t.vsrQuality = quality;
    {
        const VsrKey key{low.width(), low.height(), t.size.width(), t.size.height(), quality};
        auto it = std::find_if(g_vsrCache.begin(), g_vsrCache.end(), [&](const auto &p) { return p.first == key; });
        if (it != g_vsrCache.end()) {
            t.vsr = it->second;
            g_vsrCache.erase(it);
            if (!api.SetCudaStream(t.vsr->fx, NVVFX_CUDA_STREAM, m_stream)) {
                t.vsrReady = true;
                qInfo("truehdr: reusing upscaler %dx%d -> %dx%d (quality %u)", low.width(), low.height(), t.size.width(), t.size.height(), quality);
                return true;
            }
            t.vsr.reset(); // could not rebind: build a new one
        }
    }
    t.vsrLoadStart = std::chrono::steady_clock::now();
    qInfo("truehdr: loading upscaler %dx%d -> %dx%d", low.width(), low.height(), t.size.width(), t.size.height());
    t.vsr = std::make_shared<Vsr>();
    t.vsrLoad = std::async(std::launch::async, [self = QPointer<TrueHdrEffect>(this), v = t.vsr, low, full = t.size,
                                                dev = m_device, stream = m_stream, quality,
                                                cache = cacheDir.toLocal8Bit()]() -> int {
        // wake the compositor when done, even if nothing on screen is changing
        auto done = qScopeGuard([self] {
            QMetaObject::invokeMethod(qApp, [self] {
                if (self) {
                    effects->addRepaintFull();
                }
            }, Qt::QueuedConnection);
        });
        api.cudaSetDevice(dev);
        NvCV_Status s = api.ImageCreate(low.width(), low.height(), NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32, &v->in);
        if (!s) {
            s = api.ImageCreate(full.width(), full.height(), NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32, &v->out);
        }
        if (!s) {
            s = api.CreateEffect("VideoSuperRes", &v->fx);
        }
        if (!s) {
            s = api.SetU32(v->fx, "QualityLevel", quality);
        }
        if (!s) {
            api.SetString(v->fx, NVVFX_MODEL_CACHE_DIRECTORY, cache.constData()); // optional
            s = api.SetImage(v->fx, NVVFX_INPUT_IMAGE, v->in);
        }
        if (!s) {
            s = api.SetImage(v->fx, NVVFX_OUTPUT_IMAGE, v->out);
        }
        if (!s) {
            s = api.SetCudaStream(v->fx, NVVFX_CUDA_STREAM, stream);
        }
        if (!s) {
            s = api.Load(v->fx);
        }
        return int(s);
    });
    return false;
}

void TrueHdrEffect::drawTexture(GLTexture *tex, const RenderViewport &viewport, const std::shared_ptr<ColorDescription> &src,
                                const std::shared_ptr<ColorDescription> &dst, RenderingIntent intent)
{
    glDisable(GL_BLEND);
    GLShader *shader = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
    const double scale = viewport.scale();
    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(viewport.renderRect().x() * scale, viewport.renderRect().y() * scale);
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
    shader->setColorspaceUniforms(src, dst, intent);
    tex->render(viewport.renderRect().size() * scale);
    ShaderManager::instance()->popShader();
}

void TrueHdrEffect::prePaintScreen(ScreenPrePaintData &data)
{
    if (isActive()) {
        // TrueHDR looks at the whole frame, so always repaint all of it
        data.mask |= PAINT_SCREEN_TRANSFORMED;
    }
    effects->prePaintScreen(data);
}

void TrueHdrEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &region, LogicalOutput *screen)
{
    reapOrphans(false);
    if (m_failed && !m_targets.empty()) {
        for (auto &[output, t] : m_targets) {
            releaseTarget(t);
        }
        m_targets.clear();
    }
    const bool wanted = isActive() && screen && (!m_onlyHdrOutputs || screen->backendOutput()->highDynamicRange());
    if (!wanted || !initCuda()) {
        effects->paintScreen(renderTarget, viewport, mask, region, screen);
        return;
    }
    Target *t = ensureTarget(screen, renderTarget.size());
    if (!t || !applyParams(*t)) {
        effects->paintScreen(renderTarget, viewport, mask, region, screen);
        return;
    }
    const OutputTransform transform = renderTarget.transform();
    t->sceneTex->setContentTransform(transform);
    t->sdrTex->setContentTransform(transform);
    t->hdrTex->setContentTransform(transform);
    const auto &outColor = renderTarget.colorDescription();

    // 1. scene -> sceneTex
    RenderTarget sceneRT(t->sceneFbo.get(), outColor);
    RenderViewport sceneVP(viewport.renderRect(), viewport.scale(), sceneRT, QPoint());
    GLFramebuffer::pushFramebuffer(t->sceneFbo.get());
    effects->paintScreen(sceneRT, sceneVP, mask, region, screen);
    GLFramebuffer::popFramebuffer();

    // 2. sceneTex -> 8-bit SDR (desktop SDR white -> 1.0)
    RenderTarget sdrRT(t->sdrFbo.get(), ColorDescription::sRGB);
    RenderViewport sdrVP(viewport.renderRect(), viewport.scale(), sdrRT, QPoint());
    GLFramebuffer::pushFramebuffer(t->sdrFbo.get());
    drawTexture(t->sceneTex.get(), sdrVP, outColor, ColorDescription::sRGB, RenderingIntent::Perceptual);
    GLFramebuffer::popFramebuffer();

    // 2b. find stretched fullscreen content and prepare it for upscaling
    bool upscale = false;
    if (m_upscale && !m_upscaleBroken) {
        if (m_upForce) {
            t->factor = double(t->size.height()) / m_upForce;
        } else if (fullscreenOn(screen)) {
            detect(*t, screen);
        } else if (t->factor != 1.0 || t->candidate != 1.0) {
            t->factor = t->candidate = 1.0;
            t->streak = 0;
            t->recent.clear();
        }
        // stretched content -> upscale; otherwise optional same-size cleanup (removes compression blocks)
        double vf = 0;
        uint vq = 0;
        if (t->factor > 1.01) {
            vf = t->factor;
            vq = m_upQuality;
        } else if (m_cleanup == 2 || (m_cleanup == 1 && fullscreenOn(screen))) {
            vf = 1.0;
            vq = m_cleanQuality;
        } else if (t->vsr && t->vsrFactor == 1.0 && t->vsrReady) {
            releaseVsr(*t); // left fullscreen: free the cleanup pass
        }
        if (vf > 0 && ensureVsr(*t, vf, vq)) {
            GLFramebuffer::pushFramebuffer(t->sdrFbo.get());
            t->lowFbo->blitFromFramebuffer(QRect(), QRect(), GL_LINEAR);
            GLFramebuffer::popFramebuffer();
            upscale = true;
        }
    }

    // 3. (upscaling +) TrueHDR on the GPU
    if (!runTrueHdr(*t, upscale)) {
        // show the untouched frame this time; the effect is now off
        drawTexture(t->sceneTex.get(), viewport, outColor, outColor, RenderingIntent::Perceptual);
        return;
    }

    // 3b. optional black floor fix on the HDR10 result (hdrTex -> fixTex)
    GLTexture *result = t->hdrTex.get();
    if (m_bff.any()) {
        if (!t->fixTex || t->fixTex->size() != t->size) {
            t->fixFbo.reset();
            t->fixTex = GLTexture::allocate(GL_RGBA16F, t->size);
            if (t->fixTex) {
                t->fixTex->setFilter(GL_NEAREST);
                t->fixTex->setWrapMode(GL_CLAMP_TO_EDGE);
                t->fixFbo = std::make_unique<GLFramebuffer>(t->fixTex.get());
            }
        }
        if (!m_bffPass) {
            m_bffPass = std::make_unique<BlackFloorPass>();
        }
        if (t->fixFbo) {
            t->fixTex->setContentTransform(transform);
            RenderTarget fixRT(t->fixFbo.get(), outColor);
            RenderViewport fixVP(viewport.renderRect(), viewport.scale(), fixRT, QPoint());
            if (GLShader *sh = m_bffPass->bind(m_bff)) {
                GLFramebuffer::pushFramebuffer(t->fixFbo.get());
                glDisable(GL_BLEND);
                const double scale = fixVP.scale();
                QMatrix4x4 mvp = fixVP.projectionMatrix();
                mvp.translate(fixVP.renderRect().x() * scale, fixVP.renderRect().y() * scale);
                sh->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
                t->hdrTex->render(fixVP.renderRect().size() * scale);
                GLFramebuffer::popFramebuffer();
                m_bffPass->unbind();
                result = t->fixTex.get();
            }
        }
    } else if (t->fixTex) {
        t->fixFbo.reset();
        t->fixTex.reset();
    }

    // 4. HDR10 result -> screen, nits preserved
    drawTexture(result, viewport, pqDescription(m_luminance), outColor, RenderingIntent::AbsoluteColorimetricNoAdaptation);
}

} // namespace KWin
