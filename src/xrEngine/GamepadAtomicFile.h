#pragma once
// FR.1 #2: atomic replacement for gamepad profile files (appdata\gamepad\profile.ltx).
// Writers serialize to "<path>.tmp", flush and close the handle, then call GpCommitTempFile:
// MoveFileExA(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH). On failure the temp file is
// removed and the previous valid target file remains untouched.
// Header-only so GamepadMath.cpp and GamepadBindings.cpp still build standalone in unit tests.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstdio>
#include <filesystem>
#endif
#include <string>

inline std::string GpTempPathFor(const char* path)
{
    return std::string(path ? path : "") + ".tmp";
}

inline bool GpCommitTempFile(const char* tmpPath, const char* path)
{
    if (!tmpPath || !tmpPath[0] || !path || !path[0])
        return false;
#ifdef _WIN32
    if (!MoveFileExA(tmpPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileA(tmpPath);
        return false;
    }
    return true;
#else
    std::error_code ec;
    std::filesystem::rename(tmpPath, path, ec);
    if (ec)
    {
        std::remove(tmpPath);
        return false;
    }
    return true;
#endif
}
