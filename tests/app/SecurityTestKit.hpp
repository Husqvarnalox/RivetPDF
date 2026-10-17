// SPDX-License-Identifier: MPL-2.0
#pragma once

// Fakes shared by the Phase 8 security controller tests (ADR-0048).

#include "app/DocumentSecurityHost.hpp"
#include "core/async/IMainThreadDispatcher.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rivet::app::sectest {

// A main thread you drive by hand: posted tasks queue until pump() runs them.
class QueueDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_all();
    }
    // Runs queued tasks until `done()` is true or the timeout expires.
    bool pumpUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                if (!cv_.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(5)),
                                    [&] { return !queue_.empty(); })) {
                    if (std::chrono::steady_clock::now() >= deadline) return done();
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
        return true;
    }
    void pumpFor(std::chrono::milliseconds duration) {
        const auto end = std::chrono::steady_clock::now() + duration;
        pumpUntil([&] { return std::chrono::steady_clock::now() >= end; }, duration + std::chrono::milliseconds(50));
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& tag) {
        path = std::filesystem::temp_directory_path() /
               ("rivet-sectest-" + tag + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

inline bool fileExists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

inline std::vector<std::uint8_t> readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<std::uint8_t> out(size);
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return out;
}

// Scriptable host: document info, destination and captured bytes.
class FakeHost final : public IDocumentSecurityHost {
public:
    SecurityDocumentInfo info;
    std::optional<std::filesystem::path> destination;
    std::vector<std::uint8_t> bytes;
    bool captureFails = false;
    int captureCalls = 0;
    int destinationCalls = 0;
    std::function<bool()> producerGate; // when set, the producer waits while it returns false (until cancelled)

    SecurityDocumentInfo activeDocument() override { return info; }
    std::optional<std::filesystem::path> chooseDestination(const std::string&, const std::string&) override {
        ++destinationCalls;
        return destination;
    }
    core::Result<PlainPdfProducer> captureSave(TabId) override {
        ++captureCalls;
        if (captureFails) return std::unexpected(core::makeError(core::ErrorCode::Io, "capture failed", "test"));
        auto data = bytes;
        auto gate = producerGate;
        PlainPdfProducer producer = [data, gate](const std::function<bool()>& cancelled) -> core::Result<PdfBytes> {
            if (gate) {
                while (!gate()) {
                    if (cancelled()) return std::unexpected(core::makeError(core::ErrorCode::Cancelled, "cancelled", "test"));
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            return data;
        };
        return producer;
    }
    void commitPendingEdits() override { ++commits; }
    int commits = 0;
};

inline SecurityDocumentInfo readyDocument(const std::filesystem::path& path, bool signedDoc = false) {
    SecurityDocumentInfo i;
    i.available = true;
    i.tab = TabId{1};
    i.title = "Doc";
    i.path = path;
    i.pageCount = 2;
    i.signatures = signedDoc ? SecurityDocumentInfo::Signatures::Present : SecurityDocumentInfo::Signatures::None;
    return i;
}

} // namespace rivet::app::sectest
