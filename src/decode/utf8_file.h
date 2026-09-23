// utf8_file — open a file whose path is UTF-8, on every platform.
//
// Paths leave Qt as UTF-8 (QString::toStdString). The C library takes
// that as-is on macOS and Linux, but Windows' narrow fopen reads bytes
// in the ANSI code page, so any name outside it (CJK, accents on a
// mismatched locale) never resolves. Convert to UTF-16 and use the
// wide entry points there. The EXR path already does this through
// MemoryMappedIStream; this is the same rule for the FILE* loaders and
// libtiff.

#pragma once

#include <cstdio>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace qcv::utf8file {

#ifdef _WIN32
inline std::wstring toWide(const std::string &utf8)
{
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(n) - 1);   // drop the terminator
    return w;
}
#endif

// fopen for a UTF-8 path. `mode` is the narrow fopen mode ("rb", ...).
inline std::FILE *open(const std::string &utf8Path, const char *mode)
{
#ifdef _WIN32
    const std::wstring wpath = toWide(utf8Path);
    if (wpath.empty()) return nullptr;
    std::wstring wmode;
    for (const char *m = mode; *m; ++m) wmode.push_back(static_cast<wchar_t>(*m));
    return _wfopen(wpath.c_str(), wmode.c_str());
#else
    return std::fopen(utf8Path.c_str(), mode);
#endif
}

} // namespace qcv::utf8file
