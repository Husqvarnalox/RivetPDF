// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "net/Client.hpp"
#include "net/MockTransport.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

using namespace rivet;
using namespace rivet::net;

namespace {

class ScriptedPrompt final : public IConsentPrompt {
public:
    explicit ScriptedPrompt(bool allow) : allow_(allow) {}
    void ask(const ConsentRequest& request, std::function<void(bool)> answer) override {
        ++asked_;
        lastHost = request.host;
        answer(allow_);
    }
    std::atomic<int> asked_{0};
    std::string lastHost;
    bool allow_;
};

// Answers from another thread after a delay (UI-style).
class DelayedPrompt final : public IConsentPrompt {
public:
    ~DelayedPrompt() override {
        for (auto& t : threads_) t.join();
    }
    void ask(const ConsentRequest&, std::function<void(bool)> answer) override {
        ++asked_;
        threads_.emplace_back([answer = std::move(answer)] {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            answer(true);
        });
    }
    std::atomic<int> asked_{0};
    std::vector<std::thread> threads_;
};

// Never answers.
class SilentPrompt final : public IConsentPrompt {
public:
    void ask(const ConsentRequest&, std::function<void(bool)>) override { ++asked_; }
    std::atomic<int> asked_{0};
};

HttpRequest get(const char* text) {
    HttpRequest r;
    r.url = *parseUrl(text);
    return r;
}

std::shared_ptr<MockTransport> scriptedMock() {
    auto mock = std::make_shared<MockTransport>();
    MockReply r;
    r.body = "ok";
    mock->script("https://a.example/", r);
    mock->script("https://b.example/", r);
    return mock;
}

} // namespace

RIVET_TEST(netGateDefaultsToAskAndDenies) {
    OnlineValidationGate gate;
    CHECK(gate.mode() == OnlineValidationMode::Ask);
    auto r = gate.authorize("a.example", "test", CancelToken{});
    CHECK(!r.has_value());
    CHECK(failureOf(r.error()) == NetFailure::ConsentDenied);

    // A client built without an explicit gate behaves the same: nothing is contacted.
    auto mock = scriptedMock();
    NetworkClient client(NetworkPolicy{}, mock);
    auto fetched = client.fetch(get("https://a.example/"));
    CHECK(!fetched.has_value());
    CHECK(failureOf(fetched.error()) == NetFailure::ConsentDenied);
    CHECK_EQ(mock->callCount(), static_cast<std::size_t>(0));
}

RIVET_TEST(netGateOffMakesZeroTransportCalls) {
    auto mock = scriptedMock();
    auto prompt = std::make_shared<ScriptedPrompt>(true);
    auto gate = std::make_shared<OnlineValidationGate>(OnlineValidationMode::Off, prompt);
    NetworkClient client(NetworkPolicy{}, mock, gate);
    auto r = client.fetch(get("https://a.example/"));
    CHECK(!r.has_value());
    CHECK(failureOf(r.error()) == NetFailure::OnlineValidationOff);
    CHECK(failureOf(r.error()) != NetFailure::ConsentDenied);
    CHECK_EQ(mock->callCount(), static_cast<std::size_t>(0));
    CHECK_EQ(prompt->asked_.load(), 0); // Off never even asks
    // Off also wins over an otherwise invalid URL (no information leak about policy).
    auto bad = client.fetch(get("https://127.0.0.1/"));
    CHECK(!bad.has_value() && failureOf(bad.error()) == NetFailure::OnlineValidationOff);
    CHECK_EQ(mock->callCount(), static_cast<std::size_t>(0));
}

RIVET_TEST(netGateOnProceedsWithoutAsking) {
    auto mock = scriptedMock();
    auto prompt = std::make_shared<ScriptedPrompt>(false);
    auto gate = std::make_shared<OnlineValidationGate>(OnlineValidationMode::On, prompt);
    NetworkClient client(NetworkPolicy{}, mock, gate);
    CHECK(client.fetch(get("https://a.example/")).has_value());
    CHECK_EQ(prompt->asked_.load(), 0);
    CHECK_EQ(mock->callCount(), static_cast<std::size_t>(1));
}

RIVET_TEST(netGateAskAllowedOncePerHost) {
    auto mock = scriptedMock();
    auto prompt = std::make_shared<ScriptedPrompt>(true);
    auto gate = std::make_shared<OnlineValidationGate>(OnlineValidationMode::Ask, prompt);
    NetworkClient client(NetworkPolicy{}, mock, gate);
    CHECK(client.fetch(get("https://a.example/")).has_value());
    CHECK(client.fetch(get("https://a.example/")).has_value());
    CHECK_EQ(prompt->asked_.load(), 1);
    CHECK_EQ(prompt->lastHost, std::string("a.example"));
    CHECK(client.fetch(get("https://b.example/")).has_value());
    CHECK_EQ(prompt->asked_.load(), 2);
    CHECK_EQ(mock->callCount(), static_cast<std::size_t>(3));

    // New session: asked again.
    gate->resetSession();
    CHECK(client.fetch(get("https://a.example/")).has_value());
    CHECK_EQ(prompt->asked_.load(), 3);
}

RIVET_TEST(netGateAskDeniedIsRememberedAndBlocksTransport) {
    auto mock = scriptedMock();
    auto prompt = std::make_shared<ScriptedPrompt>(false);
    auto gate = std::make_shared<OnlineValidationGate>(OnlineValidationMode::Ask, prompt);
    NetworkClient client(NetworkPolicy{}, mock, gate);
    for (int i = 0; i < 3; ++i) {
        auto r = client.fetch(get("https://a.example/"));
        CHECK(!r.has_value());
        CHECK(failureOf(r.error()) == NetFailure::ConsentDenied);
    }
    CHECK_EQ(prompt->asked_.load(), 1);
    CHECK_EQ(mock->callCount(), static_cast<std::size_t>(0));
}

RIVET_TEST(netGateModeChangesTakeEffect) {
    auto mock = scriptedMock();
    auto gate = std::make_shared<OnlineValidationGate>(OnlineValidationMode::Off);
    NetworkClient client(NetworkPolicy{}, mock, gate);
    CHECK(!client.fetch(get("https://a.example/")).has_value());
    gate->setMode(OnlineValidationMode::On);
    CHECK(client.fetch(get("https://a.example/")).has_value());
    gate->setMode(OnlineValidationMode::Off);
    CHECK(failureOf(client.fetch(get("https://a.example/")).error()) == NetFailure::OnlineValidationOff);
}

RIVET_TEST(netGateConsultedForRedirectHosts) {
    auto mock = std::make_shared<MockTransport>();
    mock->scriptRedirect("https://a.example/", "https://b.example/");
    MockReply r;
    mock->script("https://b.example/", r);
    auto prompt = std::make_shared<ScriptedPrompt>(true);
    auto gate = std::make_shared<OnlineValidationGate>(OnlineValidationMode::Ask, prompt);
    NetworkClient client(NetworkPolicy{}, mock, gate);
    CHECK(client.fetch(get("https://a.example/")).has_value());
    CHECK_EQ(prompt->asked_.load(), 2); // both hosts asked
}

RIVET_TEST(netGateAsyncPromptAndConcurrentRequests) {
    auto mock = scriptedMock();
    auto prompt = std::make_shared<DelayedPrompt>();
    auto gate = std::make_shared<OnlineValidationGate>(OnlineValidationMode::Ask, prompt);
    NetworkClient client(NetworkPolicy{}, mock, gate);
    std::atomic<int> okCount{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&] {
            if (client.fetch(get("https://a.example/")).has_value()) ++okCount;
        });
    for (auto& t : threads) t.join();
    CHECK_EQ(okCount.load(), 4);
    CHECK_EQ(prompt->asked_.load(), 1); // one prompt for four concurrent requests
}

RIVET_TEST(netGateCancelWhilePromptPending) {
    auto mock = scriptedMock();
    auto prompt = std::make_shared<SilentPrompt>();
    auto gate = std::make_shared<OnlineValidationGate>(OnlineValidationMode::Ask, prompt);
    NetworkClient client(NetworkPolicy{}, mock, gate);
    CancelToken cancel;
    std::thread canceller([cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        cancel.cancel();
    });
    auto r = client.fetch(get("https://a.example/"), cancel);
    canceller.join();
    CHECK(!r.has_value());
    CHECK(failureOf(r.error()) == NetFailure::Cancelled);
    CHECK_EQ(mock->callCount(), static_cast<std::size_t>(0));
}
