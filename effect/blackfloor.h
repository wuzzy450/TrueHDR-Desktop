// Port of Lilium's "HDR black floor fix" ReShade shader (GPL-3.0,
// github.com/EndlesslyFlowering/ReShade_HDR_shaders, lilium__hdr_black_floor_fix.fx)
// as a final pass on TrueHDR's HDR10 output (BT.2020 + PQ).
// Three independent parts, all off by default:
//   gamma 2.2 emulation, gamma adjustment, black floor lowering.
#pragma once

#include <KConfigGroup>
#include <memory>

namespace KWin
{
class GLShader;
class GLTexture;

struct BlackFloorSettings
{
    bool g22 = false; // re-decode the dark range as gamma 2.2 instead of the sRGB curve (darker shadows)
    double g22Cutoff = 80; // nits: only values below this are touched
    bool g22OnlyLower = false; // only darken, never brighten
    int g22Csp = 0; // processing primaries: 0 BT.709, 1 DCI-P3, 2 BT.2020

    bool gamma = false;
    double gammaValue = 1.0; // >1 darker shadows, <1 lighter
    double gammaCutoff = 80;
    int gammaCsp = 0;

    bool lower = false; // black floor lowering
    int lowerMode = 0; // 0 YRGB, 1 RGB in PQ, 2 RGB
    double oldBlack = 0.0; // nits: the current (raised) black level
    double rollOff = 10.0; // nits: where the remap blends back into the original
    double newBlack = 0.0; // nits: where old black ends up

    bool any() const { return g22 || gamma || lower; }
    void read(const KConfigGroup &g);
};

class BlackFloorPass
{
public:
    BlackFloorPass();
    ~BlackFloorPass();
    // Binds the shader and uploads settings. Returns nullptr if the shader failed
    // to build (logged once); the caller then skips the pass.
    GLShader *bind(const BlackFloorSettings &s);
    void unbind();

private:
    std::unique_ptr<GLShader> m_shader;
    bool m_broken = false;
};

} // namespace KWin
