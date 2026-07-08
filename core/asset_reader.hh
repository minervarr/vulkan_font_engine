#pragma once
// The engine's only platform seam: reads a bundled asset (APK assets on
// Android, exe-relative dir or plain filesystem on desktop) by relative path,
// e.g. "shaders/tiling.spv". Core code must depend only on this interface,
// never on platform SDK headers. Byte-identical to the AssetReader consumers
// like vk_canvas declare in their own platform seam, so one implementation
// serves both libraries.

#include <cstdint>
#include <cstdio>
#include <vector>

struct AssetReader {
    virtual bool read(const char* path, std::vector<uint8_t>& out) = 0;
    virtual ~AssetReader() = default;
};

// Trivial filesystem implementation for desktop hosts and offline tools:
// resolves paths relative to the process working directory via fopen.
struct FileByteReader : AssetReader {
    bool read(const char* path, std::vector<uint8_t>& out) override {
        FILE* f = fopen(path, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size <= 0) { fclose(f); return false; }
        out.resize(static_cast<size_t>(size));
        size_t got = fread(out.data(), 1, out.size(), f);
        fclose(f);
        return got == out.size();
    }
};
