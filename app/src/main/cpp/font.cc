#include "font.hh"
#include "renderer.hh"
#include <algorithm>
#include <cmath>
#include <cstring>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

// ── Line record emit ────────────────────────────────────────────────────────

static void emitLineRecord(std::vector<float>& out,
                           float x0, float y0,
                           float x1, float y1,
                           float r, float g, float b, float a) {
    float minX = std::min(x0, x1);
    float minY = std::min(y0, y1);
    float maxX = std::max(x0, x1);
    float maxY = std::max(y0, y1);

    size_t n = out.size();
    out.resize(n + Renderer::CURVE_FLOATS, 0.0f);
    out[n +  0] = 6.0f;          // type = line segment
    out[n +  1] = x0;
    out[n +  2] = y0;
    out[n +  3] = x1;
    out[n +  4] = y1;
    // [5..8] unused
    out[n +  9] = r;
    out[n + 10] = g;
    out[n + 11] = b;
    out[n + 12] = a;
    out[n + 13] = 0.0f;
    out[n + 14] = minX;
    out[n + 15] = minY;
    out[n + 16] = maxX;
    out[n + 17] = maxY;
}

// ── Quadratic record emit ───────────────────────────────────────────────────

static inline void tightenQuad(float p0, float p1, float p2,
                               float& lo, float& hi) {
    lo = std::min(p0, p2);
    hi = std::max(p0, p2);
    float denom = p0 - 2.0f * p1 + p2;
    if (std::abs(denom) > 1e-6f) {
        float t = (p0 - p1) / denom;
        if (t > 0.0f && t < 1.0f) {
            float u = 1.0f - t;
            float v = u*u*p0 + 2.0f*u*t*p1 + t*t*p2;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
}

static void emitQuadraticRecord(std::vector<float>& out,
                                float x0, float y0,
                                float x1, float y1,
                                float x2, float y2,
                                float r, float g, float b, float a) {
    float minX, maxX, minY, maxY;
    tightenQuad(x0, x1, x2, minX, maxX);
    tightenQuad(y0, y1, y2, minY, maxY);

    size_t n = out.size();
    out.resize(n + Renderer::CURVE_FLOATS, 0.0f);
    out[n +  0] = 5.0f;          // type = quadratic Bézier
    out[n +  1] = x0;
    out[n +  2] = y0;
    out[n +  3] = x1;            // control point
    out[n +  4] = y1;
    out[n +  5] = x2;            // end point
    out[n +  6] = y2;
    // [7..8] unused
    out[n +  9] = r;
    out[n + 10] = g;
    out[n + 11] = b;
    out[n + 12] = a;
    out[n + 13] = 0.0f;
    out[n + 14] = minX;
    out[n + 15] = minY;
    out[n + 16] = maxX;
    out[n + 17] = maxY;
}

// ── Cubic record emit ───────────────────────────────────────────────────────

static inline float evalCubic(float p0, float p1, float p2, float p3, float t) {
    float u = 1.0f - t;
    return u*u*u*p0 + 3.0f*u*u*t*p1 + 3.0f*u*t*t*p2 + t*t*t*p3;
}

static inline void tightenCubic(float p0, float p1, float p2, float p3,
                                float& lo, float& hi) {
    lo = std::min(p0, p3);
    hi = std::max(p0, p3);
    // derivative roots: a t^2 + b t + c = 0
    float a = -p0 + 3.0f*p1 - 3.0f*p2 + p3;
    float b = 2.0f*(p0 - 2.0f*p1 + p2);
    float c = -p0 + p1;
    float roots[2]; int n = 0;
    if (std::abs(a) < 1e-6f) {
        if (std::abs(b) > 1e-6f) roots[n++] = -c / b;
    } else {
        float disc = b*b - 4.0f*a*c;
        if (disc >= 0.0f) {
            float sq = std::sqrt(disc);
            roots[n++] = (-b + sq) / (2.0f*a);
            roots[n++] = (-b - sq) / (2.0f*a);
        }
    }
    for (int i = 0; i < n; i++) {
        float t = roots[i];
        if (t > 0.0f && t < 1.0f) {
            float v = evalCubic(p0, p1, p2, p3, t);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
}

static void emitCubicRecord(std::vector<float>& out,
                             float x0, float y0,
                             float x1, float y1,
                             float x2, float y2,
                             float x3, float y3,
                             float r, float g, float b, float a) {
    float minX, maxX, minY, maxY;
    tightenCubic(x0, x1, x2, x3, minX, maxX);
    tightenCubic(y0, y1, y2, y3, minY, maxY);

    size_t n = out.size();
    out.resize(n + Renderer::CURVE_FLOATS, 0.0f);
    out[n +  0] = 4.0f;          // type = cubic Bézier
    out[n +  1] = x0;            // P0
    out[n +  2] = y0;
    out[n +  3] = x1;            // P1
    out[n +  4] = y1;
    out[n +  5] = x2;            // P2
    out[n +  6] = y2;
    out[n +  7] = x3;            // P3
    out[n +  8] = y3;
    out[n +  9] = r;             // color
    out[n + 10] = g;
    out[n + 11] = b;
    out[n + 12] = a;
    out[n + 13] = 0.0f;          // lineWidth (fill, no stroke)
    out[n + 14] = minX;          // bounding box
    out[n + 15] = minY;
    out[n + 16] = maxX;
    out[n + 17] = maxY;
}

// ── FreeType outline decompose callbacks ────────────────────────────────────

struct DecomposeCtx {
    std::vector<float>& out;
    float r, g, b, a;
    float curX = 0.0f, curY = 0.0f;
    float ftScale;      // pixels per font unit
    float originX;      // baseline x in screen pixels
    float originY;      // baseline y in screen pixels (y increases downward)

    float toSX(FT_Pos v) const { return originX + (float)v * ftScale; }
    // Font y-up → screen y-down: flip around baseline
    float toSY(FT_Pos v) const { return originY - (float)v * ftScale; }
};

static int cbMoveTo(const FT_Vector* to, void* user) {
    auto* c = (DecomposeCtx*)user;
    c->curX = c->toSX(to->x);
    c->curY = c->toSY(to->y);
    return 0;
}

static int cbLineTo(const FT_Vector* to, void* user) {
    auto* c = (DecomposeCtx*)user;
    float x1 = c->toSX(to->x), y1 = c->toSY(to->y);
    emitLineRecord(c->out, c->curX, c->curY, x1, y1, c->r, c->g, c->b, c->a);
    c->curX = x1; c->curY = y1;
    return 0;
}

static int cbConicTo(const FT_Vector* ctrl, const FT_Vector* to, void* user) {
    auto* c = (DecomposeCtx*)user;
    float cx = c->toSX(ctrl->x), cy = c->toSY(ctrl->y);
    float x1 = c->toSX(to->x),   y1 = c->toSY(to->y);
    emitQuadraticRecord(c->out,
        c->curX, c->curY, cx, cy, x1, y1,
        c->r, c->g, c->b, c->a);
    c->curX = x1; c->curY = y1;
    return 0;
}

static int cbCubicTo(const FT_Vector* c1, const FT_Vector* c2,
                     const FT_Vector* to, void* user) {
    auto* c = (DecomposeCtx*)user;
    float x1 = c->toSX(c1->x), y1 = c->toSY(c1->y);
    float x2 = c->toSX(c2->x), y2 = c->toSY(c2->y);
    float x3 = c->toSX(to->x), y3 = c->toSY(to->y);
    emitCubicRecord(c->out,
        c->curX, c->curY, x1, y1, x2, y2, x3, y3,
        c->r, c->g, c->b, c->a);
    c->curX = x3; c->curY = y3;
    return 0;
}

// ── Font public API ─────────────────────────────────────────────────────────

bool Font::load(const char* path) {
    FT_Library lib;
    if (FT_Init_FreeType(&lib) != 0) return false;
    FT_Face face;
    if (FT_New_Face(lib, path, 0, &face) != 0) {
        FT_Done_FreeType(lib);
        return false;
    }
    ftLibrary = lib;
    ftFace    = face;
    return true;
}

bool Font::loadFromMemory(const uint8_t* data, size_t size) {
    fontBuffer.assign(data, data + size);
    FT_Library lib;
    if (FT_Init_FreeType(&lib) != 0) return false;
    FT_Open_Args args{};
    args.flags       = FT_OPEN_MEMORY;
    args.memory_base = fontBuffer.data();
    args.memory_size = (FT_Long)fontBuffer.size();
    FT_Face face;
    if (FT_Open_Face(lib, &args, 0, &face) != 0) {
        FT_Done_FreeType(lib);
        return false;
    }
    ftLibrary = lib;
    ftFace    = face;
    return true;
}

void Font::destroy() {
    if (ftFace)    FT_Done_Face((FT_Face)ftFace);
    if (ftLibrary) FT_Done_FreeType((FT_Library)ftLibrary);
    ftFace = ftLibrary = nullptr;
}

int Font::emitGlyph(std::vector<float>& out,
                    char c, float x, float y, float scale,
                    float r, float g, float b, float a) const {
    FT_Face face = (FT_Face)ftFace;
    FT_UInt gi   = FT_Get_Char_Index(face, (FT_ULong)(unsigned char)c);
    if (FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0)
        return -1;

    float ftScale = scale / (float)face->units_per_EM;
    int before    = (int)(out.size() / Renderer::CURVE_FLOATS);

    FT_Outline_Funcs funcs = { cbMoveTo, cbLineTo, cbConicTo, cbCubicTo, 0, 0 };
    DecomposeCtx ctx{ out, r, g, b, a, 0.0f, 0.0f, ftScale, x, y };
    FT_Outline_Decompose(&face->glyph->outline, &funcs, &ctx);

    return (int)(out.size() / Renderer::CURVE_FLOATS) - before;
}

float Font::glyphAdvance(char c, float scale) const {
    FT_Face face = (FT_Face)ftFace;
    if (c == ' ') {
        FT_UInt gi = FT_Get_Char_Index(face, (FT_ULong)' ');
        if (gi && FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) == 0)
            return (float)face->glyph->metrics.horiAdvance
                   / (float)face->units_per_EM * scale;
        return 0.5f * scale;
    }
    FT_UInt gi = FT_Get_Char_Index(face, (FT_ULong)(unsigned char)c);
    if (!gi) return 0.6f * scale;
    if (FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0)
        return 0.6f * scale;
    return (float)face->glyph->metrics.horiAdvance
           / (float)face->units_per_EM * scale;
}

float Font::stringWidth(std::string_view str, float scale) const {
    float w = 0.0f;
    for (char p : str) w += glyphAdvance(p, scale);
    return w;
}

void Font::emitString(std::vector<float>& out,
                      std::string_view str, float x, float y, float scale,
                      float r, float g, float b, float a) const {
    for (char p : str) {
        if (p != ' ') emitGlyph(out, p, x, y, scale, r, g, b, a);
        x += glyphAdvance(p, scale);
    }
}
