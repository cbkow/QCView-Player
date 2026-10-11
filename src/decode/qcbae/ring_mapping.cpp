// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT — see its
// src/common/LICENSE) at 5849977, src/common/surface/ring_mapping.cpp. The wire contract between the
// QCBridgeAE Transmit device and QCView, and of QCView's own output bus; keep
// in sync by re-copying, never by editing here. A version mismatch is refused
// at open, never misread.

// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2026 cbkow. See src/common/LICENSE: the ring and sidecar
// files are MIT in addition to the repository's GPL so that consumers under
// any licence can carry the wire contract.
#include "decode/qcbae/ring_mapping.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <chrono>
#  include <thread>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace qcbae {
namespace {

#if defined(_WIN32)
std::string win_err(const char* what) {
    const DWORD e = ::GetLastError();
    char buf[256] {};
    DWORD n = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, e, 0, buf, sizeof buf, nullptr);
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    return std::string(what) + ": " + (n > 0 ? buf : "error") + " (" + std::to_string(e) + ")";
}

// "/qcbae-ae" -> L"Local\qcbae-ae". Session-local: producer and consumer
// share a logon session, and Global\ would need a privilege the host may not
// have.
std::wstring object_name(const std::string& name) {
    std::wstring w = L"Local\\";
    for (size_t i = 1; i < name.size(); ++i) w.push_back(static_cast<wchar_t>(name[i]));
    return w;
}
#else
std::string errno_str(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}
#endif

}  // namespace

uint64_t SharedMapping::page_size() {
#if defined(_WIN32)
    static const uint64_t ps = [] {
        SYSTEM_INFO si {};
        ::GetSystemInfo(&si);
        return static_cast<uint64_t>(si.dwPageSize);
    }();
#else
    static const uint64_t ps = static_cast<uint64_t>(::getpagesize());
#endif
    return ps;
}

uint32_t SharedMapping::self_pid() {
#if defined(_WIN32)
    return static_cast<uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<uint32_t>(::getpid());
#endif
}

bool SharedMapping::name_ok(const std::string& name) {
    return !name.empty() && name[0] == '/' && name.size() <= kMaxShmName;
}

SharedMapping::~SharedMapping() { close(); }

SharedMapping::SharedMapping(SharedMapping&& o) noexcept { *this = std::move(o); }

SharedMapping& SharedMapping::operator=(SharedMapping&& o) noexcept {
    if (this != &o) {
        close();
        base_ = o.base_; size_ = o.size_;
#if defined(_WIN32)
        mapping_ = o.mapping_; o.mapping_ = nullptr;
#else
        fd_ = o.fd_; o.fd_ = -1;
#endif
        owner_ = o.owner_; name_ = std::move(o.name_); error_ = std::move(o.error_);
        o.base_ = nullptr; o.owner_ = false; o.size_ = 0;
    }
    return *this;
}

void SharedMapping::close() {
#if defined(_WIN32)
    // No unlink on Windows: the section lives while any handle to it is open
    // and goes with the last one. A consumer still holding the old mapping
    // keeps reading it, exactly as it would after shm_unlink.
    if (base_ != nullptr)    { ::UnmapViewOfFile(base_); base_ = nullptr; }
    if (mapping_ != nullptr) { ::CloseHandle(static_cast<HANDLE>(mapping_)); mapping_ = nullptr; }
#else
    if (base_ != nullptr) { ::munmap(base_, size_); base_ = nullptr; }
    if (fd_ >= 0)         { ::close(fd_); fd_ = -1; }
    if (owner_ && !name_.empty()) { ::shm_unlink(name_.c_str()); }
#endif
    size_ = 0; owner_ = false;
}

bool SharedMapping::create(const std::string& name, uint64_t total, uint64_t header_bytes,
                           uint32_t magic, RetireOld retire_old) {
    close();
    if (!name_ok(name)) {
        error_ = "shm name must start with '/' and be <= 31 bytes (macOS PSHMNAMLEN)";
        return false;
    }
#if defined(_WIN32)
    const std::wstring wname = object_name(name);
    HANDLE h = nullptr;
    for (int attempt = 0; ; ++attempt) {
        h = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                 static_cast<DWORD>(total >> 32),
                                 static_cast<DWORD>(total & 0xFFFFFFFFu),
                                 wname.c_str());
        if (h == nullptr) { error_ = win_err("CreateFileMapping"); return false; }
        if (::GetLastError() != ERROR_ALREADY_EXISTS) break;
        if (attempt == 0 && retire_old != nullptr) {
            void* old = ::MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                        static_cast<SIZE_T>(header_bytes));
            if (old != nullptr) {
                if (*static_cast<const uint32_t*>(old) == magic) retire_old(old);
                ::UnmapViewOfFile(old);
            }
        }
        ::CloseHandle(h); h = nullptr;
        if (attempt >= 60) {   // ~1.5 s: longer than a consumer's 250 ms wait poll
            error_ = "ring name is still held by another process (a consumer that has not closed it)";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    mapping_ = h;
    base_ = ::MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(total));
    if (base_ == nullptr) {
        error_ = win_err("MapViewOfFile");
        ::CloseHandle(h); mapping_ = nullptr; return false;
    }
#else
    (void)header_bytes; (void)magic; (void)retire_old;
    // A stale mapping from a crashed producer would otherwise be inherited
    // with someone else's geometry.
    ::shm_unlink(name.c_str());

    fd_ = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd_ < 0) { error_ = errno_str("shm_open"); return false; }
    if (::ftruncate(fd_, static_cast<off_t>(total)) != 0) {
        error_ = errno_str("ftruncate"); ::close(fd_); fd_ = -1;
        ::shm_unlink(name.c_str()); return false;
    }
    base_ = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) {
        base_ = nullptr; error_ = errno_str("mmap"); ::close(fd_); fd_ = -1;
        ::shm_unlink(name.c_str()); return false;
    }
#endif
    size_  = total;
    name_  = name;
    owner_ = true;
    std::memset(base_, 0, total);
    return true;
}

bool SharedMapping::open(const std::string& name, uint64_t header_bytes,
                         uint32_t magic, uint32_t version, TotalOf total_of, const char* what) {
    close();
    if (!name_ok(name)) {
        error_ = "shm name must start with '/' and be <= 31 bytes"; return false;
    }
    const std::string ring = what != nullptr ? what : "ring";
    const uint64_t probe = round_up(header_bytes, page_size());

#if defined(_WIN32)
    const std::wstring wname = object_name(name);
    HANDLE h = ::OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, wname.c_str());
    if (h == nullptr) { error_ = win_err("OpenFileMapping"); return false; }

    // Same shape as POSIX: header first, then the whole thing at the size
    // the header claims. A view larger than the section is refused by
    // MapViewOfFile itself, which is the fstat check in different clothes.
    void* head = ::MapViewOfFile(h, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(probe));
    if (head == nullptr) { error_ = win_err("MapViewOfFile header"); ::CloseHandle(h); return false; }
    const uint32_t got_magic   = static_cast<const uint32_t*>(head)[0];
    const uint32_t got_version = static_cast<const uint32_t*>(head)[1];
    const uint64_t total       = total_of(head);
    ::UnmapViewOfFile(head);

    if (got_magic != magic) { error_ = "not a QCBridgeAE " + ring + " (bad magic)"; ::CloseHandle(h); return false; }
    if (got_version != version) {
        error_ = ring + " version " + std::to_string(got_version) + " != expected "
               + std::to_string(version);
        ::CloseHandle(h); return false;
    }
    if (total < probe) { error_ = ring + " smaller than its header claims"; ::CloseHandle(h); return false; }

    base_ = ::MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(total));
    if (base_ == nullptr) {
        error_ = ::GetLastError() == ERROR_ACCESS_DENIED
               ? ring + " smaller than its header claims"
               : win_err("MapViewOfFile");
        ::CloseHandle(h); return false;
    }
    mapping_ = h;
#else
    fd_ = ::shm_open(name.c_str(), O_RDWR, 0);
    if (fd_ < 0) { error_ = errno_str("shm_open"); return false; }

    // Map the header first to learn the real geometry, then remap the whole
    // thing. Trusting a size from the producer without checking the magic
    // would mean mapping whatever a stale or foreign segment claims.
    void* head = ::mmap(nullptr, probe, PROT_READ, MAP_SHARED, fd_, 0);
    if (head == MAP_FAILED) { error_ = errno_str("mmap header"); ::close(fd_); fd_ = -1; return false; }
    const uint32_t got_magic   = static_cast<const uint32_t*>(head)[0];
    const uint32_t got_version = static_cast<const uint32_t*>(head)[1];
    const uint64_t total       = total_of(head);
    ::munmap(head, probe);

    if (got_magic != magic) { error_ = "not a QCBridgeAE " + ring + " (bad magic)"; ::close(fd_); fd_ = -1; return false; }
    if (got_version != version) {
        error_ = ring + " version " + std::to_string(got_version) + " != expected "
               + std::to_string(version);
        ::close(fd_); fd_ = -1; return false;
    }
    struct ::stat st {};
    if (::fstat(fd_, &st) != 0 || static_cast<uint64_t>(st.st_size) < total || total < probe) {
        error_ = ring + " smaller than its header claims"; ::close(fd_); fd_ = -1; return false;
    }

    base_ = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) { base_ = nullptr; error_ = errno_str("mmap"); ::close(fd_); fd_ = -1; return false; }
#endif
    size_  = total;
    name_  = name;
    owner_ = false;
    return true;
}

bool process_alive(uint32_t pid) {
    if (pid == 0u) return false;
#if defined(_WIN32)
    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        // Not ours to open (another user, or elevated): assume alive, the way
        // EPERM does on POSIX. Anything else means no such process.
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }
    const DWORD w = ::WaitForSingleObject(h, 0);
    ::CloseHandle(h);
    return w == WAIT_TIMEOUT;   // still running; WAIT_OBJECT_0 = it has exited
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

}  // namespace qcbae
