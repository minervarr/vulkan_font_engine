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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#endif

struct AssetReader {
    virtual bool read(const char* path, std::vector<uint8_t>& out) = 0;
    virtual ~AssetReader() = default;
};

// Trivial filesystem implementation for desktop hosts and offline tools:
// resolves paths relative to the process working directory (or reads an
// absolute path directly). `path` is always UTF-8.
struct FileByteReader : AssetReader {
    bool read(const char* path, std::vector<uint8_t>& out) override {
#ifdef _WIN32
        // Plain fopen()/CreateFileA take the process's ANSI codepage, not
        // UTF-8 — a non-ASCII path (accented/non-Latin filenames, common in
        // real-world file libraries) would silently fail or open the wrong
        // file. Convert to UTF-16 and use the wide API instead.
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        if (wlen <= 0) return false;
        std::wstring wpath(static_cast<size_t>(wlen), 0);
        MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], wlen);

        HANDLE f = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        BOOL ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
        CloseHandle(f);
        return ok && got == out.size();
#else
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
#endif
    }
};
