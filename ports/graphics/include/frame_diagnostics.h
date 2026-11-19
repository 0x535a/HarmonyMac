/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include <unistd.h>

static inline bool aurora_display_trace_enabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("AURORA_DIRECT_BUFFER_TRACE");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

static inline unsigned long long aurora_display_time_ns()
{
    timespec stamp{};
    clock_gettime(CLOCK_MONOTONIC, &stamp);
    return static_cast<unsigned long long>(stamp.tv_sec) * 1000000000ULL + stamp.tv_nsec;
}

static inline void aurora_display_trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
static inline void aurora_display_trace(const char *format, ...)
{
    const int saved_errno = errno;
    char line[512];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (length > 0 && static_cast<size_t>(length) < sizeof(line) - 1) {
        line[length] = '\n';
        (void)write(STDERR_FILENO, line, length + 1);
    }
    errno = saved_errno;
}

// Explicit, bounded development capture. A root-created token file enables
// at most sixteen images per process; ordinary rendering never writes images.
// RGB is normalized to top-left origin so stages can be compared byte-for-byte.
struct AuroraFrameCapture {
    unsigned last_token = 0;
    static unsigned token()
    {
        auto file = std::fopen("/data/local/tmp/aurora-display-capture", "r");
        unsigned value = 0;
        if (file) { if (std::fscanf(file, "%u", &value) != 1) value = 0; std::fclose(file); }
        return value;
    }
    void save(unsigned token, const char *stage, const void *identity,
              const uint8_t *pixels, int width, int height, int stride, bool rgba, bool bottom_up)
    {
        static unsigned total = 0;
        if (!token || token == last_token || total >= 16) return;
        last_token = token;
        ++total;
        char path[256];
        std::snprintf(path, sizeof(path), "/data/local/tmp/aurora-%s-%d-%p-%u-%u.ppm", stage, getpid(), identity, token,total);
        auto file = std::fopen(path, "wb");
        if (!file) return;
        std::fprintf(file, "P6\n%d %d\n255\n", width, height);
        uint64_t hash = 14695981039346656037ULL;
        uint8_t rgb_row[16384*3];
        for (int y = 0; y < height; ++y) {
            auto row = pixels + size_t(bottom_up ? height-1-y : y)*stride;
            for (int x = 0; x < width; ++x) {
                uint8_t rgb[] = {row[x*4+(rgba ? 0 : 2)], row[x*4+1], row[x*4+(rgba ? 2 : 0)]};
                for (auto byte : rgb) hash = (hash ^ byte)*1099511628211ULL;
                for (int c=0;c<3;++c) rgb_row[x*3+c]=rgb[c];
            }
            std::fwrite(rgb_row, 3, width, file);
        }
        bool ok = !std::ferror(file);
        if (std::fclose(file)) ok = false;
        std::fprintf(stderr, "AURORA_FRAME_CAPTURE stage=%s token=%u id=%p size=%dx%d rgb_fnv=%llx ok=%d path=%s\n",
            stage, token, identity, width, height, (unsigned long long)hash, ok, path);
    }
};
