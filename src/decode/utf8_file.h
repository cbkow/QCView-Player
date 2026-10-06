// utf8_file — open a file whose path is UTF-8, on every platform.
//
// Paths leave Qt as UTF-8 (QString::toStdString). The C library takes
// that as-is on macOS and Linux, but Windows' narrow fopen reads bytes
// in the ANSI code page, so any name outside it (CJK, accents on a
// mismatched locale) never resolves. Convert to UTF-16 and use the
// wide entry points there. The EXR path already does this through
// MemoryMappedIStream; this is the same rule for the FILE* loaders and
// libtiff.
//
// std::filesystem is not a substitute. On MSVC a path built from a
// std::string is decoded in the ANSI code page: a UTF-8 CJK name is
// mangled when its bytes happen to form valid characters there, and the
// constructor THROWS when they do not (GitHub issue #6 — a Chinese
// folder either played nothing or took the app down, depending on the
// byte count of its name). Keep paths as UTF-8 strings and go through
// the wide entry points here.

#pragma once

#include <cstdio>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <sys/stat.h>
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

// Does a file or directory exist at this UTF-8 path? Never throws — the
// std::filesystem equivalent does on Windows for a name the ANSI code
// page cannot decode (see above).
inline bool exists(const std::string &utf8Path)
{
#ifdef _WIN32
    const std::wstring wpath = toWide(utf8Path);
    if (wpath.empty()) return false;
    return GetFileAttributesW(wpath.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st {};
    return ::stat(utf8Path.c_str(), &st) == 0;
#endif
}

// dir + '/' + file, as UTF-8. The directory may already end in a
// separator (either kind on Windows).
inline std::string join(const std::string &dir, const std::string &file)
{
    if (dir.empty()) return file;
    const char last = dir.back();
#ifdef _WIN32
    const bool sep = last == '/' || last == '\\';
#else
    const bool sep = last == '/';
#endif
    return sep ? dir + file : dir + '/' + file;
}

} // namespace qcv::utf8file
