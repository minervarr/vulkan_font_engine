#pragma once
#include <vector>

void emitFilledRect(std::vector<float>& out,
                    float cx, float cy, float halfW, float halfH,
                    float r, float g, float b, float a);

void emitLineSegment(std::vector<float>& out,
                     float x0, float y0, float x1, float y1,
                     float lineWidth,
                     float r, float g, float b, float a);

// Emits glyph strokes for one character. Supports 0-9, A-Z (and a-z mapped to
// uppercase), space, and common punctuation. Unknown chars are skipped.
void emitGlyph(std::vector<float>& out,
               char c, float x, float y, float scale, float lineWidth,
               float r, float g, float b, float a);

float glyphAdvance(char c, float scale);

// Convenience: emit a whole string left-to-right starting at (x, y).
void emitString(std::vector<float>& out,
                const char* str, float x, float y, float scale, float lineWidth,
                float r, float g, float b, float a);

float stringWidth(const char* str, float scale);
