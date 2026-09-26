// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace rivet::core::io {

// Ordered destination of a byte stream. Same shape as pdf::IPdfByteSink so a
// one-line adapter connects document assembly to an AtomicFileWriter without
// core depending on pdf. A non-ok Status aborts the producer.
class IByteSink {
public:
    virtual ~IByteSink() = default;
    virtual Status write(const void* data, std::size_t size) = 0;
};

// Points at which AtomicWriteOptions::faultInjector is consulted (tests only).
enum class AtomicWriteFault : std::uint8_t {
    Write,    // before each write(2) of the temp file
    Sync,     // before fsync of the temp file
    Rename,   // before rename(2) onto the destination
    DirSync,  // before fsync of the destination directory (after rename)
};

struct AtomicWriteOptions {
    // Replace an existing destination. When false and the destination exists,
    // begin() fails with ErrorCode::AlreadyExists (checked again at commit).
    bool overwriteExisting = true;

    // Optional cancellation flag, checked before every write() and before the
    // rename in commit(). Cancelling yields ErrorCode::Cancelled and rolls back.
    // Must outlive the writer.
    const std::atomic<bool>* cancelFlag = nullptr;

    // TEST HOOK. When set, it is called at each AtomicWriteFault point; a
    // non-zero return is treated as if the system call had failed with that
    // errno (e.g. ENOSPC, EIO). Production code leaves it empty.
    std::function<int(AtomicWriteFault)> faultInjector;
};

// Crash-safe "write new file, then atomically replace" writer.
//
//   auto writer = AtomicFileWriter::begin(dest, options);   // creates temp
//   writer->write(...) ...                                  // streams bytes
//   writer->commit();                                       // durable replace
//
// Guarantees (POSIX):
//   * The temp file lives in the destination's directory (same volume, so
//     rename(2) is atomic), has a unique hidden name, and is created 0600.
//   * commit(): flush + fsync (F_FULLFSYNC on Apple) of the temp file, size
//     verification, chmod to the final mode, rename over the destination,
//     then fsync of the directory. Readers observe either the complete old
//     or the complete new file, never a mix.
//   * On ANY failure before the rename (including cancellation and the
//     destructor running without commit()), the temp file is removed and the
//     destination is untouched. The writer is then failed; further write()/
//     commit() calls return the same error.
//   * A failing directory fsync AFTER the rename cannot be rolled back (the
//     new content is already in place); it is logged as a warning and
//     commit() still succeeds.
//
// Destination handling:
//   * Final mode: the existing destination's permission bits; for a new file
//     0644 masked by the process umask. Owner/group of an existing file are
//     restored best-effort (fchown; silently kept as ours if not permitted).
//   * A destination that exists but is not writable by us (e.g. mode 0444)
//     is refused with PermissionDenied rather than silently replaced.
//   * Symlink destination: the link is followed (up to 32 hops) and the
//     final target is replaced; the symlink itself is preserved. A dangling
//     link creates its target.
//   * Destination == the document's source file is fine as long as the
//     source is read through an already-open descriptor: rename(2) swaps the
//     directory entry, the old inode stays alive for open descriptors.
//   * Extended attributes: on Apple, xattrs of an existing destination
//     (Finder tags, etc.) are copied to the new file best-effort
//     (fcopyfile COPYFILE_XATTR). Elsewhere they are NOT preserved, nor are
//     ACLs, hard links (the name is re-pointed, other links keep the old
//     inode) or file creation dates. Documented limitation.
//   * Windows: not implemented; begin() returns ErrorCode::Unsupported. See
//     docs/adr/ADR-0010-atomic-save-replacement.md for the planned
//     ReplaceFileW / MoveFileExW design and its open-file limitation.
//
// Threading: an instance is not thread-safe; use it from one thread at a
// time (typically a background save worker). Distinct instances are
// independent.
class AtomicFileWriter final : public IByteSink {
public:
    static Result<std::unique_ptr<AtomicFileWriter>> begin(const std::filesystem::path& destination,
                                                           AtomicWriteOptions options = {});

    ~AtomicFileWriter() override;

    AtomicFileWriter(const AtomicFileWriter&) = delete;
    AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

    // Appends bytes to the temp file. Fails (and rolls back) on I/O error or
    // cancellation.
    Status write(const void* data, std::size_t size) override;

    // Makes the written content the destination, durably. Single use.
    Status commit();

    // Discards the temp file; the destination is untouched. Idempotent;
    // no-op after a successful commit().
    void abort();

    // The file that commit() replaces (symlinks resolved).
    const std::filesystem::path& resolvedDestination() const { return destination_; }
    const std::filesystem::path& tempPath() const { return tempPath_; }
    std::uint64_t bytesWritten() const { return bytesWritten_; }

private:
    AtomicFileWriter() = default;

    Status fail(Error error);

    enum class State : std::uint8_t { Open, Failed, Committed, Aborted };

    std::filesystem::path destination_;
    std::filesystem::path tempPath_;
    AtomicWriteOptions options_;
    int fd_ = -1;
    std::uint64_t bytesWritten_ = 0;
    State state_ = State::Open;
    Error error_;
    // Metadata to apply at commit.
    unsigned finalMode_ = 0644;
    bool destinationExisted_ = false;
    long long ownerUid_ = -1;
    long long ownerGid_ = -1;
};

// Convenience: begin(), run `produce` against the writer, commit(). Any
// failure (including one returned by `produce`) rolls back.
Status writeFileAtomically(const std::filesystem::path& destination,
                           const std::function<Status(IByteSink&)>& produce,
                           AtomicWriteOptions options = {});

} // namespace rivet::core::io
