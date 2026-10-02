#pragma once

#include <coreinit/debug.h>
#include <whb/log.h>

#define ALBUM_LOG(fmt, ...) do { \
    OSReport("[ALBUM] " fmt "\n", ##__VA_ARGS__); \
    WHBLogPrintf("[ALBUM] " fmt, ##__VA_ARGS__); \
} while (0)

#define ALBUM_LOG_QUIET(fmt, ...) \
    WHBLogPrintf("[ALBUM] " fmt, ##__VA_ARGS__)

#define ALBUM_ERROR(fmt, ...) OSReport("[ALBUM] ** " fmt "\n", ##__VA_ARGS__)