#include "demo.hh"
#include "font.hh"
#include "glyphs.hh"
#include "renderer.hh"
#include <vector>

namespace {

static float uiAdvance(const Font* font, char c, float scale) {
    return font ? font->glyphAdvance(c, scale) : glyphAdvance(c, scale);
}
static float uiWidth(const Font* font, const char* s, float scale) {
    return font ? font->stringWidth(s, scale) : stringWidth(s, scale);
}
static void uiString(std::vector<float>& out, const Font* font,
                     const char* s, float x, float y, float scale,
                     float r, float g, float b, float a) {
    if (font)
        font->emitString(out, s, x, y, scale, r, g, b, a);
    else
        emitString(out, s, x, y, scale, scale * 0.07f, r, g, b, a);
}

} // namespace

void Demo::init(uint32_t w, uint32_t h) {
    screenW = w;
    screenH = h;
}

void Demo::setInsets(uint32_t top, uint32_t bottom, uint32_t left, uint32_t right) {
    if (top == insetTop && bottom == insetBottom &&
        left == insetLeft && right == insetRight) return;
    insetTop    = top;
    insetBottom = bottom;
    insetLeft   = left;
    insetRight  = right;
    dirty = true;
}

void Demo::rebuildCurves(std::vector<float>& out, const Font* font) const {
    out.clear();
    out.reserve(4000 * Renderer::CURVE_FLOATS);

    float sw  = float(screenW);
    float sh  = float(screenH);
    float pad = sw * 0.03f;
    // Safe area: system insets + rounded-corner margin (≈ corner radius).
    float corner = sw * 0.06f;
    float topSafe    = float(insetTop)    + corner;
    float bottomSafe = float(insetBottom) + corner;
    float leftSafe   = float(insetLeft)   + pad;
    float rightSafe  = float(insetRight)  + pad;
    float y   = topSafe;

    // Background
    emitFilledRect(out, sw * 0.5f, sh * 0.5f, sw * 0.5f, sh * 0.5f,
                   0.04f, 0.04f, 0.06f, 1.0f);

    float maxW = sw - leftSafe - rightSafe;
    auto centeredLine = [&](const char* text, float scale,
                            float r, float g, float b) {
        float w = uiWidth(font, text, scale);
        if (w > maxW) { scale *= maxW / w; w = maxW; }
        y += scale;                            // top → baseline (≈ ascender)
        uiString(out, font, text, (sw - w) * 0.5f, y, scale, r, g, b, 1.0f);
        y += scale * 0.45f;                    // descender + line gap
    };

    // Title
    centeredLine("VK FONT RENDERER", sh * 0.075f, 0.40f, 0.80f, 1.00f);
    y += pad * 0.4f;

    // Pangram
    centeredLine("THE QUICK BROWN FOX JUMPS", sh * 0.052f, 0.95f, 0.95f, 0.95f);
    centeredLine("OVER THE LAZY DOG",         sh * 0.052f, 0.95f, 0.95f, 0.95f);
    y += pad * 0.4f;

    // Alphabet rows
    centeredLine("ABCDEFGHIJKLM", sh * 0.048f, 0.55f, 0.90f, 0.55f);
    centeredLine("NOPQRSTUVWXYZ", sh * 0.048f, 0.55f, 0.90f, 0.55f);
    y += pad * 0.4f;

    // Digits
    centeredLine("0123456789", sh * 0.048f, 0.90f, 0.80f, 0.45f);
    y += pad * 0.4f;

    // Size stress-test: same string at shrinking scales
    const char* sample = "SAMPLE TEXT";
    for (float frac : {0.042f, 0.032f, 0.024f, 0.016f}) {
        float scale = sh * frac;
        float w = uiWidth(font, sample, scale);
        if (w > maxW) { scale *= maxW / w; w = maxW; }
        y += scale;
        uiString(out, font, sample, (sw - w) * 0.5f, y, scale,
                 0.70f, 0.70f, 0.70f, 1.0f);
        y += scale * 0.5f;
    }

    // Rendering mode indicator pinned to bottom
    {
        float scale = sh * 0.038f;
        const char* mode = font ? "OTF  (FREETYPE  FILLED)" : "STROKE  FALLBACK";
        float mr = font ? 0.35f : 0.90f;
        float mg = font ? 1.00f : 0.60f;
        float mb = font ? 0.35f : 0.20f;
        float mw = uiWidth(font, mode, scale);
        if (mw > maxW) { scale *= maxW / mw; mw = maxW; }
        uiString(out, font, mode, (sw - mw) * 0.5f, sh - bottomSafe,
                 scale, mr, mg, mb, 1.0f);
    }
}
