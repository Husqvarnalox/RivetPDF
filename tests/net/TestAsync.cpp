// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "net/Async.hpp"
#include "net/MockTransport.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

using namespace rivet;
using namespace rivet::net;

namespace {

class QueueDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_all();
    }
    bool drain(std::size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::size_t ran = 0;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (ran < count) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                if (!cv_.wait_until(lock, deadline, [&] { return !queue_.empty(); })) return false;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
            ++ran;
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

std::shared_ptr<NetworkClient> makeClient(const std::shared_ptr<MockTransport>& mock) {
    return std::make_shared<NetworkClient>(NetworkPolicy{}, mock,
                                           std::make_shared<OnlineValidationGate>(OnlineValidationMode::On));
}

HttpRequest get(const char* text) {
    HttpRequest r;
    r.url = *parseUrl(text);
    return r;
}

} // namespace

RIVET_TEST(netAsyncDeliversOnMainThreadOnce) {
    core::TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    core::AsyncScope scope;
    auto mock = std::make_shared<MockTransport>();
    MockReply reply;
    reply.body = "async";
    mock->script("https://example.com/", reply);
    const auto mainThread = std::this_thread::get_id();
    std::thread::id doneThread;
    int calls = 0;
    std::string body;
    CHECK(runNetworkAsync(scheduler, dispatcher, scope, makeClient(mock), get("https://example.com/"), CancelToken{},
                          [&](core::Result<HttpResponse> r) {
                              doneThread = std::this_thread::get_id();
                              ++calls;
                              if (r) body = r->body;
                          }));
    CHECK(dispatcher.drain(1));
    CHECK_EQ(calls, 1);
    CHECK(doneThread == mainThread);
    CHECK_EQ(body, std::string("async"));
    scope.closeAndWait();
}

RIVET_TEST(netAsyncCancelAndClosedScope) {
    core::TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    core::AsyncScope scope;
    auto mock = std::make_shared<MockTransport>();
    MockReply hang;
    hang.hang = true;
    mock->script("https://hang.example/", hang);
    CancelToken cancel;
    NetFailure kind = NetFailure::Transport;
    bool delivered = false;
    CHECK(runNetworkAsync(scheduler, dispatcher, scope, makeClient(mock), get("https://hang.example/"), cancel,
                          [&](core::Result<HttpResponse> r) {
                              delivered = true;
                              if (!r) kind = failureOf(r.error());
                          }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cancel.cancel();
    CHECK(dispatcher.drain(1));
    CHECK(delivered);
    CHECK(kind == NetFailure::Cancelled);

    scope.closeAndWait();
    bool called = false;
    CHECK(!runNetworkAsync(scheduler, dispatcher, scope, makeClient(mock), get("https://hang.example/"), CancelToken{},
                           [&](core::Result<HttpResponse>) { called = true; }));
    CHECK(!called);
}
