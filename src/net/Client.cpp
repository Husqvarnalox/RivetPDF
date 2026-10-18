// SPDX-License-Identifier: MPL-2.0
#include "net/Client.hpp"

#include <algorithm>
#include <chrono>

namespace rivet::net {

namespace {

bool isRedirectStatus(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

core::Error cancelled() { return makeNetError(NetFailure::Cancelled, "operation cancelled"); }

} // namespace

NetworkClient::NetworkClient(NetworkPolicy policy, std::shared_ptr<INetworkTransport> transport,
                             std::shared_ptr<OnlineValidationGate> gate)
    : policy_(std::move(policy)),
      transport_(transport ? std::move(transport) : makeDefaultTransport()),
      gate_(gate ? std::move(gate) : std::make_shared<OnlineValidationGate>()) {}

core::Result<HttpResponse> NetworkClient::fetch(HttpRequest request, CancelToken cancel) const {
    using Clock = std::chrono::steady_clock;

    if (request.method == HttpMethod::Get && !request.body.empty())
        return netFail<HttpResponse>(NetFailure::InvalidRequest, "GET requests cannot have a body");
    if (auto s = validateHeaders(request.headers); !s) return std::unexpected(s.error());

    // The user setting wins over everything else, including URL diagnostics.
    if (gate_->mode() == OnlineValidationMode::Off)
        return std::unexpected(makeNetError(NetFailure::OnlineValidationOff, "online validation is turned off in the settings"));

    const EffectiveLimits limits = effectiveLimits(request, policy_);
    const int maxRedirects = std::clamp(request.maxRedirects.value_or(policy_.maxRedirects), 0, policy_.maxRedirects);
    const auto deadline = Clock::now() + limits.totalTimeout;

    int hops = 0;
    while (true) {
        if (cancel.cancelled()) return std::unexpected(cancelled());

        if (auto s = checkUrl(request.url, policy_); !s) return std::unexpected(s.error());
        if (auto s = gate_->authorize(request.url.host, "network request", cancel); !s) return std::unexpected(s.error());

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0)
            return netFail<HttpResponse>(NetFailure::Timeout, "total time limit exceeded");

        HttpRequest hop = request;
        hop.maxResponseBytes = limits.maxResponseBytes;
        hop.timeouts.total = remaining;
        hop.timeouts.connect = std::min(limits.connectTimeout, remaining);
        hop.maxRedirects = 0;

        core::Result<HttpResponse> result = transport_->perform(hop, policy_, cancel);
        if (cancel.cancelled()) return std::unexpected(cancelled());
        if (!result) return result;
        HttpResponse response = std::move(*result);

        if (response.body.size() > limits.maxResponseBytes)
            return netFail<HttpResponse>(NetFailure::ResponseTooLarge, "response exceeds the size limit");

        const auto location = findHeader(response.headers, "location");
        if (!isRedirectStatus(response.status) || !location) {
            response.finalUrl = request.url;
            return response;
        }

        if (hops >= maxRedirects) return netFail<HttpResponse>(NetFailure::RedirectLimit, "too many redirects");
        auto next = resolveReference(request.url, *location);
        if (!next) return netFail<HttpResponse>(NetFailure::BadRedirect, "unusable redirect target");
        if (request.url.isHttps() && !next->isHttps())
            return netFail<HttpResponse>(NetFailure::RedirectDowngrade, "refusing https to http redirect");

        if (response.status == 301 || response.status == 302 || response.status == 303) {
            if (request.method == HttpMethod::Post || response.status == 303) {
                request.method = HttpMethod::Get;
                request.body.clear();
            }
        }
        request.url = std::move(*next);
        ++hops;
    }
}

} // namespace rivet::net
