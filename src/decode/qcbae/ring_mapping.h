// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT — see its
// src/common/LICENSE) at 5849977, src/common/surface/ring_mapping.h. The wire contract between the
// QCBridgeAE Transmit device and QCView, and of QCView's own output bus; keep
// in sync by re-copying, never by editing here. A version mismatch is refused
// at open, never misread.

// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2026 cbkow. See src/common/LICENSE: the ring and sidecar
// files are MIT in addition to the repository's GPL so that consumers under
// any licence can carry the wire contract.
// QCBridgeAE — the one shared-memory mapping every ring is built on.
//
// The frame ring, the audio ring and the output ring differ in what they
// put in their slots; they do not differ in how a named, page-file-backed
// mapping is created, replaced, opened, checked and closed. Until 2026-10-11
// each ring carried its own copy of that code, including the Windows
// name-replace retry; this is the single copy.
//
// Platform rules, unchanged from the rings that grew them:
//   * POSIX: shm_open under a name that starts with '/' and is <= 31 bytes
//     (macOS PSHMNAMLEN). create() unlinks a stale name first, so a crashed
//     producer's geometry is never inherited; close() unlinks when it owns.
//   * Windows: `Local\` + the name without its slash, a session-local
//     page-file section. A section cannot be unlinked: a name still held by a
//     consumer hands the OLD object back with its old geometry. create() then
//     marks that old mapping Retired through `retire_old` (its consumers close
//     and re-open the name) and retries for ~1.5 s while they let go.
//   * open() maps the header first, checks magic and version, learns the
//     total size from the header through `total_of`, verifies the object is at
//     least that large, and only then maps the whole thing. A size claimed by
//     a stale or foreign segment is never trusted blind.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace qcbae {

// macOS caps shm_open names at 31 bytes including the leading slash. The
// same rule applies on Windows so a name that works on one platform works
// on the other.
inline constexpr size_t kMaxShmName = 31u;

class SharedMapping {
public:
    SharedMapping() = default;
    ~SharedMapping();
    SharedMapping(SharedMapping&&) noexcept;
    SharedMapping& operator=(SharedMapping&&) noexcept;
    SharedMapping(const SharedMapping&) = delete;
    SharedMapping& operator=(const SharedMapping&) = delete;

    // Reads the ring's header fields a mapping needs to validate it: the
    // magic at offset 0, the version at offset 4 are fixed for every ring;
    // where the total size lives is the ring's business.
    using TotalOf   = uint64_t (*)(const void* header);
    // Windows only: flip the old mapping's state word to Retired before the
    // retry. Receives the old header, already checked to carry `magic`.
    using RetireOld = void (*)(void* header);

    // Producer side: a zeroed mapping of `total` bytes under `name`,
    // replacing whatever was there. `header_bytes` is how much of the start
    // the retire step may touch (one page is plenty).
    bool create(const std::string& name, uint64_t total, uint64_t header_bytes,
                uint32_t magic, RetireOld retire_old);

    // Consumer side. `what` names the ring in error messages ("audio ring").
    bool open(const std::string& name, uint64_t header_bytes,
              uint32_t magic, uint32_t version, TotalOf total_of, const char* what);

    void close();

    bool   valid() const { return base_ != nullptr; }
    void*  base()  const { return base_; }
    size_t size()  const { return size_; }
    bool   owner() const { return owner_; }
    const std::string& name()  const { return name_; }
    const std::string& error() const { return error_; }

    static uint64_t page_size();
    static uint64_t round_up(uint64_t v, uint64_t to) { return (v + to - 1u) / to * to; }
    static uint32_t self_pid();
    static bool     name_ok(const std::string& name);

private:
    void*       base_    = nullptr;
    size_t      size_    = 0;
#if defined(_WIN32)
    void*       mapping_ = nullptr;   // HANDLE from CreateFileMapping / OpenFileMapping
#else
    int         fd_      = -1;
#endif
    bool        owner_   = false;
    std::string name_;
    std::string error_;
};

// Is the process that wrote a producer pid still running? A dead producer's
// ring stays readable (neither Adobe host unloads the Transmit device on
// quit), so the pid in the header is the consumer's only truth. POSIX:
// kill(pid, 0), with EPERM counting as alive. Windows: OpenProcess(SYNCHRONIZE)
// and a zero-timeout wait; a process we may not open counts as alive, the way
// EPERM does. Windows recycles pids far faster than macOS, so a consumer
// polling every few hundred ms can in principle mistake an unrelated new
// process for the producer; the ring then reads as alive-but-silent until
// that process exits. Accepted for v1 and noted in lab/results.
bool process_alive(uint32_t pid);

}  // namespace qcbae
