// See blackfloor.h. Maths follows lilium__hdr_black_floor_fix.fx / .fxh (HDR10 path).
#include "blackfloor.h"

#include "opengl/eglcontext.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"

#include <QDebug>
#include <algorithm>
#include <cmath>

namespace KWin
{

void BlackFloorSettings::read(const KConfigGroup &g)
{
    g22 = g.readEntry("BffG22", false);
    g22Cutoff = std::clamp(g.readEntry("BffG22Cutoff", 80.0), 10.0, 400.0);
    g22OnlyLower = g.readEntry("BffG22OnlyLower", false);
    g22Csp = std::clamp(g.readEntry("BffG22Csp", 0), 0, 2);
    gamma = g.readEntry("BffGamma", false);
    gammaValue = std::clamp(g.readEntry("BffGammaValue", 1.0), 0.5, 1.5);
    gammaCutoff = std::clamp(g.readEntry("BffGammaCutoff", 80.0), 10.0, 400.0);
    gammaCsp = std::clamp(g.readEntry("BffGammaCsp", 0), 0, 2);
    lower = g.readEntry("BffLower", false);
    lowerMode = std::clamp(g.readEntry("BffLowerMode", 0), 0, 2);
    oldBlack = std::clamp(g.readEntry("BffOldBlack", 0.0), 0.0, 0.5);
    rollOff = std::clamp(g.readEntry("BffRollOff", 10.0), 1.0, 50.0);
    newBlack = std::clamp(g.readEntry("BffNewBlack", 0.0), -0.1, 0.1);
}

namespace
{
const char *kBody = R"GLSL(
uniform sampler2D sampler;
uniform int g22On;
uniform int g22OnlyLower;
uniform int g22Csp;
uniform float g22Wp;
uniform int gaOn;
uniform int gaCsp;
uniform float gaWp;
uniform float gaGamma;
uniform int lowOn;
uniform int lowMode;
uniform float rollOff;
uniform float oldBlack;
uniform float rollMinusOld;
uniform float minLum;

const float m1 = 0.1593017578125;
const float m2 = 78.84375;
const float c1 = 0.8359375;
const float c2 = 18.8515625;
const float c3 = 18.6875;

// PQ <-> linear, linear normalised so 1.0 = 10000 nits
vec3 pqToLin(vec3 e)
{
    vec3 p = pow(max(e, vec3(0.0)), vec3(1.0 / m2));
    return pow(max(p - c1, vec3(0.0)) / (c2 - c3 * p), vec3(1.0 / m1));
}
float pqToLin(float e) { return pqToLin(vec3(e)).x; }
vec3 linToPq(vec3 l)
{
    vec3 p = pow(max(l, vec3(0.0)), vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}
float linToPq(float l) { return linToPq(vec3(l)).x; }

vec3 linToSrgb(vec3 x)
{
    vec3 lo = x * 12.92;
    vec3 hi = 1.055 * pow(max(x, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, vec3(lessThanEqual(x, vec3(0.0031308))));
}

vec3 rows(vec3 r0, vec3 r1, vec3 r2, vec3 c) { return vec3(dot(r0, c), dot(r1, c), dot(r2, c)); }

// linear BT.2020 <-> chosen primaries (0 BT.709, 1 Display P3/D65, 2 BT.2020)
vec3 toCsp(vec3 c, int csp)
{
    if (csp == 0) {
        return rows(vec3(1.6604910, -0.5876411, -0.0728499), vec3(-0.1245505, 1.1328999, -0.0083494), vec3(-0.0181508, -0.1005789, 1.1187297), c);
    }
    if (csp == 1) {
        return rows(vec3(1.3435783, -0.2821797, -0.0613986), vec3(-0.0652975, 1.0757879, -0.0104905), vec3(0.0028218, -0.0195985, 1.0167767), c);
    }
    return c;
}
vec3 fromCsp(vec3 c, int csp)
{
    if (csp == 0) {
        return rows(vec3(0.6274040, 0.3292820, 0.0433136), vec3(0.0690970, 0.9195400, 0.0113612), vec3(0.0163916, 0.0880132, 0.8955950), c);
    }
    if (csp == 1) {
        return rows(vec3(0.7538330, 0.1985974, 0.0475696), vec3(0.0457438, 0.9417772, 0.0124789), vec3(-0.0012103, 0.0176017, 0.9836086), c);
    }
    return c;
}

const vec3 kLum2020 = vec3(0.2627002, 0.6779981, 0.0593017);

float bpa(float x)
{
    float c = (x - oldBlack) / rollMinusOld;
    float e = 1.0 - c;
    e = e * e;
    e = e * e;
    return (minLum * e + c) * rollMinusOld + oldBlack;
}
vec3 bpa(vec3 x)
{
    vec3 c = (x - oldBlack) / rollMinusOld;
    vec3 e = vec3(1.0) - c;
    e = e * e;
    e = e * e;
    return (minLum * e + c) * rollMinusOld + oldBlack;
}

vec3 process(vec3 pq)
{
    vec3 lin = pqToLin(pq); // BT.2020 linear

    if (g22On != 0 && gaOn != 0 && g22OnlyLower == 0 && g22Wp == gaWp) {
        vec3 x = toCsp(lin, g22Csp);
        vec3 need = vec3(lessThan(x, vec3(g22Wp))) * vec3(greaterThan(x, vec3(0.0)));
        vec3 p = pow(max(linToSrgb(x / g22Wp), vec3(0.0)), vec3(2.2 * gaGamma)) * g22Wp;
        lin = fromCsp(mix(x, p, need), g22Csp);
    } else {
        if (g22On != 0) {
            vec3 x = toCsp(lin, g22Csp);
            float maxRange = g22OnlyLower != 0 ? 0.3892221149351 * g22Wp : g22Wp;
            vec3 need = vec3(lessThan(x, vec3(maxRange))) * vec3(greaterThan(x, vec3(0.0)));
            vec3 p = pow(max(linToSrgb(x / g22Wp), vec3(0.0)), vec3(2.2)) * g22Wp;
            lin = fromCsp(mix(x, p, need), g22Csp);
        }
        if (gaOn != 0) {
            vec3 x = toCsp(lin, gaCsp);
            vec3 need = vec3(lessThan(x, vec3(gaWp))) * vec3(greaterThan(x, vec3(0.0)));
            vec3 p = pow(max(x / gaWp, vec3(0.0)), vec3(gaGamma)) * gaWp;
            lin = fromCsp(mix(x, p, need), gaCsp);
        }
    }

    if (lowOn != 0) {
        float y = dot(kLum2020, lin);
        if (lowMode == 0) { // YRGB: scale RGB by the luminance remap (done in PQ)
            float ypq = linToPq(y);
            if (ypq <= rollOff && y > 0.0) {
                float y2 = pqToLin(bpa(ypq));
                lin *= y2 / y;
            }
        } else if (lowMode == 1) { // RGB in PQ
            if (linToPq(y) <= rollOff) {
                lin = pqToLin(max(bpa(linToPq(lin)), vec3(0.0)));
            }
        } else { // RGB (linear)
            if (y <= rollOff) {
                lin = max(bpa(lin), vec3(0.0));
            }
        }
    }
    return linToPq(max(lin, vec3(0.0)));
}
)GLSL";
} // namespace

BlackFloorPass::BlackFloorPass() = default;
BlackFloorPass::~BlackFloorPass() = default;

GLShader *BlackFloorPass::bind(const BlackFloorSettings &s)
{
    if (m_broken) {
        return nullptr;
    }
    if (!m_shader) {
        const auto ctx = EglContext::currentContext();
        QByteArray src;
        if (ctx->isOpenGLES()) {
            const bool es3 = ctx->glslVersion() >= Version(3, 0);
            src += es3 ? "#version 300 es\n" : "";
            src += "precision highp float;\n";
            src += es3 ? "in vec2 texcoord0;\nout vec4 fragColor;\n#define TEX texture\n"
                       : "varying vec2 texcoord0;\n#define fragColor gl_FragColor\n#define TEX texture2D\n";
        } else if (ctx->glslVersion() >= Version(1, 40)) {
            src += "#version 140\nin vec2 texcoord0;\nout vec4 fragColor;\n#define TEX texture\n";
        } else {
            src += "varying vec2 texcoord0;\n#define fragColor gl_FragColor\n#define TEX texture2D\n";
        }
        src += kBody;
        src += "void main()\n{\n    vec4 t = TEX(sampler, texcoord0);\n    fragColor = vec4(process(t.rgb), t.a);\n}\n";
        m_shader = ShaderManager::instance()->generateCustomShader(ShaderTrait::MapTexture, QByteArray(), src);
        if (!m_shader) {
            qWarning("truehdr: black floor fix shader failed to build, pass disabled");
            m_shader.reset();
            m_broken = true;
            return nullptr;
        }
    }

    // GetParams() from the .fx, HDR10 path (white points / 10000)
    auto pq = [](double l) {
        const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
        const double p = std::pow(std::max(l, 0.0), m1);
        return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
    };
    double roll = s.rollOff / 10000.0;
    double oldB = s.oldBlack / 10000.0;
    double newB = s.newBlack / 10000.0;
    if (s.lowerMode != 2) {
        roll = pq(roll);
        oldB = pq(oldB);
        newB = (newB < 0 ? -1.0 : 1.0) * pq(std::abs(newB));
    }
    const double rmo = roll - oldB;

    ShaderManager::instance()->pushShader(m_shader.get());
    GLShader *sh = m_shader.get();
    sh->setUniform("sampler", 0);
    sh->setUniform("g22On", int(s.g22));
    sh->setUniform("g22OnlyLower", int(s.g22OnlyLower));
    sh->setUniform("g22Csp", s.g22Csp);
    sh->setUniform("g22Wp", float(s.g22Cutoff / 10000.0));
    sh->setUniform("gaOn", int(s.gamma));
    sh->setUniform("gaCsp", s.gammaCsp);
    sh->setUniform("gaWp", float(s.gammaCutoff / 10000.0));
    sh->setUniform("gaGamma", float(s.gammaValue));
    sh->setUniform("lowOn", int(s.lower));
    sh->setUniform("lowMode", s.lowerMode);
    sh->setUniform("rollOff", float(roll));
    sh->setUniform("oldBlack", float(oldB));
    sh->setUniform("rollMinusOld", float(rmo));
    sh->setUniform("minLum", float(rmo != 0 ? (newB - oldB) / rmo : 0.0));
    return sh;
}

void BlackFloorPass::unbind()
{
    ShaderManager::instance()->popShader();
}

} // namespace KWin
