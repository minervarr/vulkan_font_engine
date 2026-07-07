#pragma once
// Logging shim so core code never includes <android/log.h> directly.
// Android routes to logcat; other platforms to stdout/stderr.

#if defined(__ANDROID__)
#include <android/log.h>
#define VFE_LOGI(tag, ...) __android_log_print(ANDROID_LOG_INFO,  tag, __VA_ARGS__)
#define VFE_LOGE(tag, ...) __android_log_print(ANDROID_LOG_ERROR, tag, __VA_ARGS__)
#else
#include <cstdio>
#define VFE_LOGI(tag, ...) \
    (std::fprintf(stdout, "I/%s: ", tag), std::fprintf(stdout, __VA_ARGS__), std::fputc('\n', stdout))
#define VFE_LOGE(tag, ...) \
    (std::fprintf(stderr, "E/%s: ", tag), std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#endif
