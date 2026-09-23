// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT) at 5057e96,
// src/common/surface/shared_ring.cpp. The wire contract between the QCBridgeAE Transmit device
// and QCView. Keep in sync by re-copying, never by editing here:
// SharedRing::open() rejects a ring whose version != kFrameDescVersion, so
// a mismatch fails loudly instead of misreading frames.

#include "decode/qcbae/shared_ring.h"

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

uint64_t page_size() {
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

uint64_t round_up(uint64_t v, uint64_t to) {
    return (v + to - 1u) / to * to;
}

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

SharedRing::~SharedRing() { close(); }

SharedRing::SharedRing(SharedRing&& o) noexcept { *this = std::move(o); }

SharedRing& SharedRing::operator=(SharedRing&& o) noexcept {
    if (this != &o) {
        close();
        base_ = o.base_;   size_ = o.size_;     header_ = o.header_;
#if defined(_WIN32)
        mapping_ = o.mapping_; o.mapping_ = nullptr;
#else
        fd_ = o.fd_; o.fd_ = -1;
#endif
        owner_ = o.owner_;   name_ = std::move(o.name_);
        error_ = std::move(o.error_);
        writing_slot_ = o.writing_slot_; write_seq_ = o.write_seq_; held_ = o.held_;
        o.base_ = nullptr; o.header_ = nullptr; o.owner_ = false;
        o.size_ = 0;
    }
    return *this;
}

void SharedRing::close() {
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
    header_ = nullptr; size_ = 0; owner_ = false;
}

SlotHeader* SharedRing::slot_at(uint32_t index) const {
    auto* bytes = static_cast<uint8_t*>(base_);
    return reinterpret_cast<SlotHeader*>(bytes + header_->slots_offset
                                         + index * header_->slot_stride);
}

bool SharedRing::create(const std::string& name, uint64_t pixels_capacity,
                        uint32_t slot_count, uint64_t icc_capacity) {
    close();
    if (name.empty() || name[0] != '/' || name.size() > kMaxShmName) {
        error_ = "shm name must start with '/' and be <= 31 bytes (macOS PSHMNAMLEN)";
        return false;
    }
    if (slot_count < kMinSlots || slot_count > kMaxSlots) {
        error_ = "slot_count must be between 3 and 255 (see pack_latest)";
        return false;
    }

    const uint64_t ps         = page_size();
    icc_capacity              = round_up(icc_capacity, ps);
    const uint64_t pix_off    = round_up(sizeof(SlotHeader), ps);
    const uint64_t pix_cap    = round_up(pixels_capacity, ps);
    const uint64_t stride     = pix_off + pix_cap;
    const uint64_t head_bytes = round_up(sizeof(RingHeader), ps);
    const uint64_t total      = head_bytes + icc_capacity + stride * slot_count;

#if defined(_WIN32)
    // POSIX unlinks a stale name and creates a fresh one. A Windows section
    // cannot be unlinked: CreateFileMapping on a name that still exists hands
    // back the OLD object, with its old geometry, and ERROR_ALREADY_EXISTS.
    // The name only survives while someone holds a handle -- a consumer that
    // has not yet noticed the previous producer is gone, or the previous
    // producer itself (a module reset in the same host, whose own handle
    // close() above just dropped). So: tell that mapping it is Retired (its
    // consumers close and re-open the name), then retry for a moment while
    // they let go.
    const std::wstring wname = object_name(name);
    HANDLE h = nullptr;
    for (int attempt = 0; ; ++attempt) {
        h = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                 static_cast<DWORD>(total >> 32),
                                 static_cast<DWORD>(total & 0xFFFFFFFFu),
                                 wname.c_str());
        if (h == nullptr) { error_ = win_err("CreateFileMapping"); return false; }
        if (::GetLastError() != ERROR_ALREADY_EXISTS) break;
        if (attempt == 0) {
            void* old = ::MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                        static_cast<SIZE_T>(head_bytes));
            if (old != nullptr) {
                auto* oh = static_cast<RingHeader*>(old);
                if (oh->magic == kRingMagic)
                    oh->host_state.store(static_cast<uint32_t>(HostState::Retired),
                                         std::memory_order_release);
                ::UnmapViewOfFile(old);
            }
        }
        ::CloseHandle(h); h = nullptr;
        if (attempt >= 60) {   // ~1.5 s: longer than the consumer's 250 ms wait poll
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
    const uint32_t self_pid = static_cast<uint32_t>(::GetCurrentProcessId());
#else
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
    const uint32_t self_pid = static_cast<uint32_t>(::getpid());
#endif

    size_  = total;
    name_  = name;
    owner_ = true;

    std::memset(base_, 0, total);
    header_ = static_cast<RingHeader*>(base_);
    header_->magic           = kRingMagic;
    header_->version         = kFrameDescVersion;
    header_->slot_count      = slot_count;
    header_->producer_pid    = self_pid;
    header_->slot_stride     = stride;
    header_->pixels_offset   = pix_off;
    header_->pixels_capacity = pix_cap;
    header_->icc_offset      = head_bytes;
    header_->icc_capacity    = icc_capacity;
    header_->slots_offset    = head_bytes + icc_capacity;
    header_->total_size      = total;
    // header_ atomics are zeroed by the memset above, which is the intended
    // initial state (latest = 0 means "nothing published yet").
    return true;
}

bool SharedRing::open(const std::string& name) {
    close();
    if (name.empty() || name[0] != '/' || name.size() > kMaxShmName) {
        error_ = "shm name must start with '/' and be <= 31 bytes"; return false;
    }
    const uint64_t probe = round_up(sizeof(RingHeader), page_size());

#if defined(_WIN32)
    const std::wstring wname = object_name(name);
    HANDLE h = ::OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, wname.c_str());
    if (h == nullptr) { error_ = win_err("OpenFileMapping"); return false; }

    // Same shape as POSIX: header first, then the whole thing at the size
    // the header claims. A view larger than the section is refused by
    // MapViewOfFile itself, which is the fstat check in different clothes.
    void* head = ::MapViewOfFile(h, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(probe));
    if (head == nullptr) { error_ = win_err("MapViewOfFile header"); ::CloseHandle(h); return false; }
    const auto* hh = static_cast<const RingHeader*>(head);
    const uint32_t magic = hh->magic, version = hh->version;
    const uint64_t total = hh->total_size;
    ::UnmapViewOfFile(head);

    if (magic != kRingMagic) { error_ = "not a QCBridgeAE ring (bad magic)"; ::CloseHandle(h); return false; }
    if (version != kFrameDescVersion) {
        error_ = "ring version " + std::to_string(version) + " != expected "
               + std::to_string(kFrameDescVersion);
        ::CloseHandle(h); return false;
    }
    if (total < probe) { error_ = "ring smaller than its header claims"; ::CloseHandle(h); return false; }

    base_ = ::MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(total));
    if (base_ == nullptr) {
        error_ = ::GetLastError() == ERROR_ACCESS_DENIED
               ? std::string("ring smaller than its header claims")
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
    const auto* h = static_cast<const RingHeader*>(head);
    const uint32_t magic = h->magic, version = h->version;
    const uint64_t total = h->total_size;
    ::munmap(head, probe);

    if (magic != kRingMagic) { error_ = "not a QCBridgeAE ring (bad magic)"; ::close(fd_); fd_ = -1; return false; }
    if (version != kFrameDescVersion) {
        error_ = "ring version " + std::to_string(version) + " != expected "
               + std::to_string(kFrameDescVersion);
        ::close(fd_); fd_ = -1; return false;
    }
    struct ::stat st {};
    if (::fstat(fd_, &st) != 0 || static_cast<uint64_t>(st.st_size) < total) {
        error_ = "ring smaller than its header claims"; ::close(fd_); fd_ = -1; return false;
    }

    base_ = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) { base_ = nullptr; error_ = errno_str("mmap"); ::close(fd_); fd_ = -1; return false; }
#endif
    size_    = total;
    name_    = name;
    owner_   = false;
    header_  = static_cast<RingHeader*>(base_);
    return true;
}

void* SharedRing::begin_write(uint64_t bytes) {
    if (!valid() || writing_slot_ != UINT32_MAX) return nullptr;
    if (bytes > header_->pixels_capacity) {
        error_ = "frame larger than slot capacity"; return nullptr;
    }
    const uint64_t packed = header_->latest.load(std::memory_order_relaxed);
    const uint64_t next   = latest_seq(packed) + 1u;
    const uint32_t count  = header_->slot_count;
    const uint64_t claim  = header_->reader_claim.load(std::memory_order_acquire);

    // Advance from the slot we last wrote, stepping over whatever the consumer
    // is holding. With >= 3 slots the next one is guaranteed free.
    uint32_t index = (packed == 0u) ? 0u : (latest_slot(packed) + 1u) % count;
    if (claim != 0u && index == latest_slot(claim)) {
        index = (index + 1u) % count;
    }

    SlotHeader* slot = slot_at(index);
    const uint64_t s = slot->seq.load(std::memory_order_relaxed);
    slot->seq.store(s + 1u, std::memory_order_release);   // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);

    writing_slot_ = index;
    write_seq_    = next;
    return static_cast<uint8_t*>(base_) + header_->slots_offset
         + index * header_->slot_stride + header_->pixels_offset;
}

void SharedRing::commit(const FrameDesc& desc) {
    if (!valid() || writing_slot_ == UINT32_MAX) return;
    SlotHeader* slot = slot_at(writing_slot_);
    slot->desc = desc;
    const uint64_t s = slot->seq.load(std::memory_order_relaxed);
    slot->seq.store(s + 1u, std::memory_order_release);   // even: stable again
    header_->latest.store(pack_latest(write_seq_, writing_slot_), std::memory_order_release);
    writing_slot_ = UINT32_MAX;
}

void SharedRing::abandon() {
    if (!valid() || writing_slot_ == UINT32_MAX) return;
    SlotHeader* slot = slot_at(writing_slot_);
    const uint64_t s = slot->seq.load(std::memory_order_relaxed);
    slot->seq.store(s + 1u, std::memory_order_release);   // back to even; latest untouched
    writing_slot_ = UINT32_MAX;
}

bool SharedRing::acquire_latest(uint64_t* last_seen, FrameDesc* out_desc, const void** out_pixels) {
    if (!valid() || last_seen == nullptr) return false;
    const uint64_t packed = header_->latest.load(std::memory_order_acquire);
    if (packed == 0u) return false;
    const uint64_t seq   = latest_seq(packed);
    const uint32_t index = latest_slot(packed);
    if (seq == *last_seen) return false;
    if (index >= header_->slot_count) return false;   // corrupt or foreign ring
    SlotHeader* slot = slot_at(index);

    // Claim before reading so the producer stops reusing this slot, then
    // verify with the seqlock that nothing was mid-write when we claimed.
    header_->reader_claim.store(packed, std::memory_order_release);

    const uint64_t s1 = slot->seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0u) { header_->reader_claim.store(held_, std::memory_order_release); return false; }

    FrameDesc desc = slot->desc;
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t s2 = slot->seq.load(std::memory_order_acquire);
    if (s1 != s2) { header_->reader_claim.store(held_, std::memory_order_release); return false; }

    held_ = packed;
    *last_seen = seq;
    if (out_desc != nullptr) *out_desc = desc;
    if (out_pixels != nullptr) {
        *out_pixels = static_cast<const uint8_t*>(base_) + header_->slots_offset
                    + index * header_->slot_stride + header_->pixels_offset;
    }
    return true;
}

void SharedRing::release() {
    if (!valid()) return;
    held_ = 0;
    header_->reader_claim.store(0u, std::memory_order_release);
}

bool SharedRing::set_icc_profile(const void* data, uint32_t len) {
    if (!valid()) return false;
    if (len > header_->icc_capacity) { error_ = "ICC profile exceeds region"; return false; }
    auto* dst = static_cast<uint8_t*>(base_) + header_->icc_offset;
    const uint64_t s = header_->icc_seq.load(std::memory_order_relaxed);
    header_->icc_seq.store(s + 1u, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    if (len > 0u && data != nullptr) std::memcpy(dst, data, len);
    header_->icc_len.store(len, std::memory_order_release);
    header_->icc_generation.fetch_add(1u, std::memory_order_release);
    header_->icc_seq.store(s + 2u, std::memory_order_release);
    return true;
}

uint64_t SharedRing::read_icc_profile(uint64_t known_generation, std::string* out_blob) const {
    if (!valid() || out_blob == nullptr) return 0u;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t s1 = header_->icc_seq.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) continue;
        const uint64_t gen = header_->icc_generation.load(std::memory_order_acquire);
        if (gen == 0u || gen == known_generation) return 0u;
        const uint32_t len = header_->icc_len.load(std::memory_order_acquire);
        if (len > header_->icc_capacity) return 0u;
        const auto* src = static_cast<const uint8_t*>(base_) + header_->icc_offset;
        std::string blob(reinterpret_cast<const char*>(src), len);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (header_->icc_seq.load(std::memory_order_acquire) != s1) continue;
        *out_blob = std::move(blob);
        return gen;
    }
    return 0u;
}

void SharedRing::set_host_state(HostState st) {
    if (header_ != nullptr) header_->host_state.store(static_cast<uint32_t>(st), std::memory_order_release);
}

HostState SharedRing::host_state() const {
    return header_ != nullptr ? static_cast<HostState>(header_->host_state.load(std::memory_order_acquire))
                              : HostState::Retired;
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
