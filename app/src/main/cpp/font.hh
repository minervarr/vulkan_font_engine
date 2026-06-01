#pragma once
#include <cstdint>
#include <vector>
#include <string_view>

struct Font {
    void* ftLibrary = nullptr;
    void* ftFace    = nullptr;
    std::vector<uint8_t> fontBuffer;  // keeps memory-loaded data alive

    // Load from a file path (desktop/test use).
    bool load(const char* otfPath);

    // Load from a byte buffer — required for Android assets (no fs path).
    // The Font stores its own copy of the data.
    bool loadFromMemory(const uint8_t* data, size_t size);

    void destroy();

    // Emit type-4 cubic curve records for one character.
    // x, y: baseline origin in screen pixels (y increases downward).
    // scale: desired cap-height in pixels (used as em-square height).
    // Returns number of curve records emitted, or -1 on error.
    int emitGlyph(std::vector<float>& out,
                  char c, float x, float y, float scale,
                  float r, float g, float b, float a) const;

    float glyphAdvance(char c, float scale) const;
    float stringWidth(std::string_view str, float scale) const;

    void emitString(std::vector<float>& out,
                    std::string_view str, float x, float y, float scale,
                    float r, float g, float b, float a) const;
};
