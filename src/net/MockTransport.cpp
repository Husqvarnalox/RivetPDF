// SPDX-License-Identifier: MPL-2.0
#include "net/MockTransport.hpp"

#include <algorithm>
#include <thread>

namespace rivet::net {

namespace {

std::string keyFor(const std::string& url) {
    auto parsed = parseUrl(url);
    return parsed ? parsed->toString() : url;
}

} // namespace

void MockTransport::script(const std::string& url, MockReply reply) {
    std::lock_guard lock(mutex_);
    script_[keyFor(url)] = std::move(reply);
}

void MockTransport::scriptRedirect(const std::string& url, const std::string& location, int status) {
    MockReply r;
    r.status = status;
    r.headers.emplace_back("Location", location);
    script(url, std::move(r));
}

std::vector<HttpRequest> MockTransport::requests() const {
    std::lock_guard lock(mutex_);
    return requests_;
}

std::size_t MockTransport::callCount() const {
    std::lock_guard lock(mutex_);
    return requests_.size();
}

core::Result<HttpResponse> MockTransport::perform(const HttpRequest& request, const NetworkPolicy& policy,
                                                  CancelToken cancel) {
    MockReply reply;
    {
        std::lock_guard lock(mutex_);
        requests_.push_back(request);
        const auto it = script_.find(request.url.toString());
        if (it == script_.end())
            return netFail<HttpResponse>(NetFailure::Transport, "mock: no scripted reply for " + request.url.toString());
        reply = it->second;
    }

    // Same transport-level duties as the real thing.
    if (auto s = checkScheme(request.url, policy); !s) return std::unexpected(s.error());
    if (auto s = checkAllowList(request.url, policy); !s) return std::unexpected(s.error());
    if (cancel.cancelled()) return netFail<HttpResponse>(NetFailure::Cancelled, "operation cancelled");

    std::optional<IpAddress> peer = reply.connectAddress ? reply.connectAddress : request.url.ip;
    if (!policy.allowPrivateNetworks && peer && isForbiddenAddress(*peer))
        return netFail<HttpResponse>(NetFailure::ForbiddenAddress,
                                     "refusing to connect to forbidden address " + formatIp(*peer));

    const EffectiveLimits limits = effectiveLimits(request, policy);
    using Clock = std::chrono::steady_clock;
    if (reply.hang || reply.latency.count() > 0) {
        if (!reply.hang && reply.latency > limits.totalTimeout)
            return netFail<HttpResponse>(NetFailure::Timeout, "mock: total timeout exceeded");
        const auto start = Clock::now();
        const auto wait = reply.hang ? limits.totalTimeout : reply.latency;
        while (Clock::now() - start < wait) {
            if (cancel.cancelled()) return netFail<HttpResponse>(NetFailure::Cancelled, "operation cancelled");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (reply.hang) return netFail<HttpResponse>(NetFailure::Timeout, "mock: total timeout exceeded");
    }

    if (reply.body.size() > limits.maxResponseBytes)
        return netFail<HttpResponse>(NetFailure::ResponseTooLarge, "response exceeds the size limit");

    HttpResponse response;
    response.status = reply.status;
    response.headers = std::move(reply.headers);
    response.body = std::move(reply.body);
    response.finalUrl = request.url;
    return response;
}

} // namespace rivet::net
