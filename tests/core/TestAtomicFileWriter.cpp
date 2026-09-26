// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/io/AtomicFileWriter.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using rivet::core::ErrorCode;
using rivet::core::Status;
using rivet::core::io::AtomicFileWriter;
using rivet::core::io::AtomicWriteFault;
using rivet::core::io::AtomicWriteOptions;
using rivet::core::io::IByteSink;
using rivet::core::io::writeFileAtomically;
namespace fs = std::filesystem;

#if defined(_WIN32)

RIVET_TEST(atomicWriterUnsupportedOnWindows) {
    auto writer = AtomicFileWriter::begin(fs::temp_directory_path() / "rivet-atomic.pdf");
    CHECK(!writer.has_value());
    CHECK_EQ(writer.error().code, ErrorCode::Unsupported);
}

#else

#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/xattr.h>
#endif

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "rivet-atomic-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char* made = ::mkdtemp(buffer.data());
        if (made != nullptr) path = made;
    }
    ~TempDir() {
        if (path.empty()) return;
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void writeFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::size_t entryCount(const fs::path& dir) {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir)) ++count;
    return count;
}

unsigned modeOf(const fs::path& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0xFFFFU;
    return static_cast<unsigned>(st.st_mode) & 07777U;
}

unsigned currentUmask() {
    // Test-only (single-threaded here): read the umask by setting it back.
    const mode_t mask = ::umask(022);
    ::umask(mask);
    return static_cast<unsigned>(mask);
}

Status writeString(IByteSink& sink, const std::string& text) {
    return sink.write(text.data(), text.size());
}

} // namespace

RIVET_TEST(atomicWriterCreatesNewFileWithDefaultMode) {
    TempDir dir;
    CHECK(!dir.path.empty());
    const fs::path dest = dir.path / "new.pdf";

    auto writer = AtomicFileWriter::begin(dest);
    CHECK(writer.has_value());
    // The temp sits next to the destination, private while being written.
    CHECK_EQ((*writer)->tempPath().parent_path(), dir.path);
    CHECK_EQ(modeOf((*writer)->tempPath()), 0600U);
    CHECK(!fs::exists(dest));
    CHECK(writeString(**writer, "%PDF-1.7 hello").has_value());
    CHECK(writeString(**writer, " world").has_value());
    CHECK_EQ((*writer)->bytesWritten(), std::uint64_t{20});
    CHECK((*writer)->commit().has_value());

    CHECK_EQ(readFile(dest), std::string("%PDF-1.7 hello world"));
    CHECK_EQ(modeOf(dest), 0644U & ~currentUmask());
    CHECK_EQ(entryCount(dir.path), std::size_t{1}); // no temp left behind
}

RIVET_TEST(atomicWriterOverwritesAndKeepsMode) {
    TempDir dir;
    const fs::path dest = dir.path / "doc.pdf";
    writeFile(dest, "old content that is longer than the new one");
    ::chmod(dest.c_str(), 0640);

    CHECK(writeFileAtomically(dest, [](IByteSink& sink) { return writeString(sink, "new"); })
              .has_value());
    CHECK_EQ(readFile(dest), std::string("new"));
    CHECK_EQ(modeOf(dest), 0640U);
    CHECK_EQ(entryCount(dir.path), std::size_t{1});
}

RIVET_TEST(atomicWriterRefusesExistingWithoutOverwrite) {
    TempDir dir;
    const fs::path dest = dir.path / "doc.pdf";
    writeFile(dest, "old");
    AtomicWriteOptions options;
    options.overwriteExisting = false;
    auto writer = AtomicFileWriter::begin(dest, options);
    CHECK(!writer.has_value());
    CHECK_EQ(writer.error().code, ErrorCode::AlreadyExists);
    CHECK_EQ(readFile(dest), std::string("old"));
}

RIVET_TEST(atomicWriterNoOverwriteDetectsRaceAtCommit) {
    TempDir dir;
    const fs::path dest = dir.path / "doc.pdf";
    AtomicWriteOptions options;
    options.overwriteExisting = false;
    auto writer = AtomicFileWriter::begin(dest, options);
    CHECK(writer.has_value());
    CHECK(writeString(**writer, "mine").has_value());
    writeFile(dest, "theirs"); // appears between begin() and commit()
    const Status committed = (*writer)->commit();
    CHECK(!committed.has_value());
    CHECK_EQ(committed.error().code, ErrorCode::AlreadyExists);
    CHECK_EQ(readFile(dest), std::string("theirs"));
    CHECK_EQ(entryCount(dir.path), std::size_t{1});
}

RIVET_TEST(atomicWriterFailureMidWriteLeavesOriginal) {
    TempDir dir;
    const fs::path dest = dir.path / "doc.pdf";
    writeFile(dest, "original");

    int writes = 0;
    AtomicWriteOptions options;
    options.faultInjector = [&writes](AtomicWriteFault point) {
        if (point == AtomicWriteFault::Write && ++writes == 3) return EIO;
        return 0;
    };
    auto writer = AtomicFileWriter::begin(dest, options);
    CHECK(writer.has_value());
    const fs::path temp = (*writer)->tempPath();
    CHECK(writeString(**writer, "aaa").has_value());
    CHECK(writeString(**writer, "bbb").has_value());
    const Status third = writeString(**writer, "ccc");
    CHECK(!third.has_value());
    CHECK_EQ(third.error().code, ErrorCode::Io);
    CHECK(!fs::exists(temp)); // rolled back immediately
    // Sticky failure.
    CHECK_EQ(writeString(**writer, "ddd").error().code, ErrorCode::Io);
    CHECK_EQ((*writer)->commit().error().code, ErrorCode::Io);
    CHECK_EQ(readFile(dest), std::string("original"));
    CHECK_EQ(entryCount(dir.path), std::size_t{1});
}

RIVET_TEST(atomicWriterInjectedDiskFullAndSyncFailuresRollBack) {
    struct Case {
        AtomicWriteFault point;
        int err;
        ErrorCode expected;
    };
    const Case cases[] = {
        {AtomicWriteFault::Write, ENOSPC, ErrorCode::DiskFull},
        {AtomicWriteFault::Write, EDQUOT, ErrorCode::DiskFull},
        {AtomicWriteFault::Sync, EIO, ErrorCode::Io},
        {AtomicWriteFault::Sync, ENOSPC, ErrorCode::DiskFull},
        {AtomicWriteFault::Rename, EACCES, ErrorCode::PermissionDenied},
        {AtomicWriteFault::Rename, EROFS, ErrorCode::PermissionDenied},
    };
    for (const Case& c : cases) {
        TempDir dir;
        const fs::path dest = dir.path / "doc.pdf";
        writeFile(dest, "original");
        AtomicWriteOptions options;
        options.faultInjector = [c](AtomicWriteFault point) { return point == c.point ? c.err : 0; };
        const Status result = writeFileAtomically(
            dest, [](IByteSink& sink) { return writeString(sink, "replacement"); }, options);
        CHECK(!result.has_value());
        CHECK_EQ(result.error().code, c.expected);
        CHECK(!result.error().message.empty());
        CHECK_EQ(readFile(dest), std::string("original"));
        CHECK_EQ(entryCount(dir.path), std::size_t{1});
    }
}

RIVET_TEST(atomicWriterDirSyncFailureAfterRenameStillSucceeds) {
    TempDir dir;
    const fs::path dest = dir.path / "doc.pdf";
    writeFile(dest, "original");
    AtomicWriteOptions options;
    options.faultInjector = [](AtomicWriteFault point) {
        return point == AtomicWriteFault::DirSync ? EIO : 0;
    };
    CHECK(writeFileAtomically(dest, [](IByteSink& sink) { return writeString(sink, "new"); },
                              options)
              .has_value());
    CHECK_EQ(readFile(dest), std::string("new"));
}

RIVET_TEST(atomicWriterProducerFailureRollsBack) {
    TempDir dir;
    const fs::path dest = dir.path / "doc.pdf";
    writeFile(dest, "original");
    const Status result = writeFileAtomically(dest, [](IByteSink& sink) -> Status {
        if (Status s = writeString(sink, "partial"); !s) return s;
        return std::unexpected(rivet::core::makeError(ErrorCode::InvalidDocument, "producer failed"));
    });
    CHECK(!result.has_value());
    CHECK_EQ(result.error().code, ErrorCode::InvalidDocument);
    CHECK_EQ(readFile(dest), std::string("original"));
    CHECK_EQ(entryCount(dir.path), std::size_t{1});
}

RIVET_TEST(atomicWriterDestructorWithoutCommitRollsBack) {
    TempDir dir;
    const fs::path dest = dir.path / "doc.pdf";
    writeFile(dest, "original");
    {
        auto writer = AtomicFileWriter::begin(dest);
        CHECK(writer.has_value());
        CHECK(writeString(**writer, "never committed").has_value());
    }
    CHECK_EQ(readFile(dest), std::string("original"));
    CHECK_EQ(entryCount(dir.path), std::size_t{1});
}

RIVET_TEST(atomicWriterPermissionDeniedDirectory) {
    if (::geteuid() == 0) return; // root bypasses permission bits
    TempDir dir;
    const fs::path locked = dir.path / "locked";
    fs::create_directory(locked);
    ::chmod(locked.c_str(), 0500);
    auto writer = AtomicFileWriter::begin(locked / "doc.pdf");
    CHECK(!writer.has_value());
    CHECK_EQ(writer.error().code, ErrorCode::PermissionDenied);
    ::chmod(locked.c_str(), 0700);
    CHECK_EQ(entryCount(locked), std::size_t{0});
}

RIVET_TEST(atomicWriterReadOnlyDestinationIsRefused) {
    if (::geteuid() == 0) return;
    TempDir dir;
    const fs::path dest = dir.path / "readonly.pdf";
    writeFile(dest, "original");
    ::chmod(dest.c_str(), 0444);
    auto writer = AtomicFileWriter::begin(dest);
    CHECK(!writer.has_value());
    CHECK_EQ(writer.error().code, ErrorCode::PermissionDenied);
    CHECK_EQ(readFile(dest), std::string("original"));
    CHECK_EQ(modeOf(dest), 0444U);
    CHECK_EQ(entryCount(dir.path), std::size_t{1});
}

RIVET_TEST(atomicWriterNonexistentDirectory) {
    TempDir dir;
    auto writer = AtomicFileWriter::begin(dir.path / "missing" / "doc.pdf");
    CHECK(!writer.has_value());
    CHECK_EQ(writer.error().code, ErrorCode::NotFound);

    // A regular file used as a directory component.
    writeFile(dir.path / "file", "x");
    auto underFile = AtomicFileWriter::begin(dir.path / "file" / "doc.pdf");
    CHECK(!underFile.has_value());
    CHECK_EQ(underFile.error().code, ErrorCode::NotFound);
}

RIVET_TEST(atomicWriterDestinationIsDirectory) {
    TempDir dir;
    fs::create_directory(dir.path / "folder.pdf");
    auto writer = AtomicFileWriter::begin(dir.path / "folder.pdf");
    CHECK(!writer.has_value());
    CHECK_EQ(writer.error().code, ErrorCode::InvalidArgument);
    CHECK(fs::is_directory(dir.path / "folder.pdf"));
    CHECK_EQ(entryCount(dir.path), std::size_t{1});
}

RIVET_TEST(atomicWriterNameTooLong) {
    TempDir dir;
    auto writer = AtomicFileWriter::begin(dir.path / (std::string(300, 'n') + ".pdf"));
    CHECK(!writer.has_value());
    CHECK_EQ(writer.error().code, ErrorCode::InvalidArgument);
    CHECK_EQ(entryCount(dir.path), std::size_t{0});
}

RIVET_TEST(atomicWriterLongUtf8NameGetsValidTempName) {
    TempDir dir;
    std::string name;
    for (int i = 0; i < 40; ++i) name += "\xD0\x96"; // "Ж" x40 = 80 bytes
    name += ".pdf";
    CHECK(writeFileAtomically(dir.path / name, [](IByteSink& sink) { return writeString(sink, "x"); })
              .has_value());
    CHECK_EQ(readFile(dir.path / name), std::string("x"));
}

RIVET_TEST(atomicWriterCancellation) {
    TempDir dir;
    const fs::path dest = dir.path / "doc.pdf";
    writeFile(dest, "original");
    std::atomic<bool> cancel{false};
    AtomicWriteOptions options;
    options.cancelFlag = &cancel;
    auto writer = AtomicFileWriter::begin(dest, options);
    CHECK(writer.has_value());
    CHECK(writeString(**writer, "part one").has_value());
    cancel.store(true);
    const Status second = writeString(**writer, "part two");
    CHECK(!second.has_value());
    CHECK_EQ(second.error().code, ErrorCode::Cancelled);
    CHECK_EQ((*writer)->commit().error().code, ErrorCode::Cancelled);
    CHECK_EQ(readFile(dest), std::string("original"));
    CHECK_EQ(entryCount(dir.path), std::size_t{1});

    // Cancelled between the last write and commit.
    cancel.store(false);
    auto late = AtomicFileWriter::begin(dest, options);
    CHECK(late.has_value());
    CHECK(writeString(**late, "all").has_value());
    cancel.store(true);
    CHECK_EQ((*late)->commit().error().code, ErrorCode::Cancelled);
    CHECK_EQ(readFile(dest), std::string("original"));
    CHECK_EQ(entryCount(dir.path), std::size_t{1});
}

RIVET_TEST(atomicWriterLargeStreamedWrite) {
    TempDir dir;
    const fs::path dest = dir.path / "large.pdf";
    constexpr std::size_t kChunk = 1U << 20U;
    constexpr std::size_t kChunks = 64; // 64 MiB
    std::vector<unsigned char> chunk(kChunk);
    CHECK(writeFileAtomically(dest,
                              [&](IByteSink& sink) -> Status {
                                  for (std::size_t i = 0; i < kChunks; ++i) {
                                      for (std::size_t j = 0; j < kChunk; j += 4096) {
                                          chunk[j] = static_cast<unsigned char>(i);
                                      }
                                      if (Status s = sink.write(chunk.data(), chunk.size()); !s) {
                                          return s;
                                      }
                                  }
                                  return rivet::core::ok();
                              })
              .has_value());
    CHECK_EQ(fs::file_size(dest), std::uintmax_t{kChunk * kChunks});
    std::ifstream in(dest, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(kChunk * 63));
    CHECK_EQ(in.get(), 63);
    CHECK_EQ(entryCount(dir.path), std::size_t{1});
}

RIVET_TEST(atomicWriterWritesThroughSymlink) {
    TempDir dir;
    const fs::path target = dir.path / "real.pdf";
    const fs::path link = dir.path / "link.pdf";
    writeFile(target, "original");
    fs::create_symlink("real.pdf", link); // relative link
    auto writer = AtomicFileWriter::begin(link);
    CHECK(writer.has_value());
    CHECK_EQ((*writer)->resolvedDestination().filename(), fs::path("real.pdf"));
    CHECK(writeString(**writer, "updated").has_value());
    CHECK((*writer)->commit().has_value());
    CHECK(fs::is_symlink(link));
    CHECK_EQ(readFile(target), std::string("updated"));
    CHECK_EQ(readFile(link), std::string("updated"));
    CHECK_EQ(entryCount(dir.path), std::size_t{2});
}

RIVET_TEST(atomicWriterSourceReadThroughOpenFdSurvivesReplace) {
    TempDir dir;
    const fs::path dest = dir.path / "doc.pdf";
    writeFile(dest, "source bytes");
    std::ifstream source(dest, std::ios::binary); // retained handle
    CHECK(writeFileAtomically(dest, [&](IByteSink& sink) -> Status {
              // Stream from the retained source while writing its replacement.
              std::string buffer((std::istreambuf_iterator<char>(source)),
                                 std::istreambuf_iterator<char>());
              buffer += " + edits";
              return writeString(sink, buffer);
          }).has_value());
    CHECK_EQ(readFile(dest), std::string("source bytes + edits"));
}

#if defined(__APPLE__)
RIVET_TEST(atomicWriterPreservesExtendedAttributesOnApple) {
    TempDir dir;
    const fs::path dest = dir.path / "doc.pdf";
    writeFile(dest, "original");
    const char value[] = "tagged";
    CHECK(::setxattr(dest.c_str(), "org.rivet.test", value, sizeof(value), 0, 0) == 0);
    CHECK(writeFileAtomically(dest, [](IByteSink& sink) { return writeString(sink, "new"); })
              .has_value());
    char readBack[16] = {};
    const ssize_t size = ::getxattr(dest.c_str(), "org.rivet.test", readBack, sizeof(readBack), 0, 0);
    CHECK_EQ(size, static_cast<ssize_t>(sizeof(value)));
    CHECK_EQ(std::string(readBack), std::string("tagged"));
}
#endif

RIVET_TEST(atomicWriterUseAfterCommitIsRejected) {
    TempDir dir;
    auto writer = AtomicFileWriter::begin(dir.path / "doc.pdf");
    CHECK(writer.has_value());
    CHECK((*writer)->commit().has_value()); // empty file is valid
    CHECK_EQ(fs::file_size(dir.path / "doc.pdf"), std::uintmax_t{0});
    CHECK_EQ(writeString(**writer, "late").error().code, ErrorCode::InvalidArgument);
    CHECK_EQ((*writer)->commit().error().code, ErrorCode::InvalidArgument);
    (*writer)->abort(); // no-op after commit
    CHECK(fs::exists(dir.path / "doc.pdf"));
}

#endif // _WIN32
