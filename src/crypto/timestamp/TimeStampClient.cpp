// SPDX-License-Identifier: MPL-2.0
#include "crypto/timestamp/TimeStampClient.hpp"

#include "crypto/Async.hpp"
#include "net/Error.hpp"
#include "net/Url.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace rivet::crypto {

namespace {

// Forwards cancellation of the crypto token to the net token while a blocking
// fetch is in flight (the two layers have distinct token types).
class CancelBridge {
public:
    explicit CancelBridge(const CancelToken& source) : watcher_([this, source] { run(source); }) {}
    ~CancelBridge() {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        watcher_.join();
    }
    CancelBridge(const CancelBridge&) = delete;
    CancelBridge& operator=(const CancelBridge&) = delete;
    const net::CancelToken& token() const { return net_; }

private:
    void run(const CancelToken& source) {
        std::unique_lock lock(mutex_);
        while (!done_) {
            if (source.cancelled()) {
                net_.cancel();
                return;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(10));
        }
    }

    net::CancelToken net_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread watcher_; // last: starts after the other members exist
};

std::string lowerMediaType(std::string_view value) {
    const std::size_t semi = value.find(';');
    if (semi != std::string_view::npos) value = value.substr(0, semi);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

TimeStampClient::TimeStampClient(std::shared_ptr<const net::NetworkClient> network,
                                 std::shared_ptr<CryptoProvider> provider)
    : network_(std::move(network)), provider_(std::move(provider)) {}

CryptoResult<TimeStampResult> TimeStampClient::request(std::string_view url, DigestAlgorithm algorithm,
                                                       ByteView digest, const TimeStampClientOptions& options,
                                                       const CancelToken& cancel) const {
    if (!network_ || !provider_) return fail(CryptoErrorCode::InvalidArgument, "time-stamp client is not configured");
    if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
    if (!provider_->isAvailable()) return fail(CryptoErrorCode::BackendUnavailable, "no crypto backend available");

    auto built = buildTimeStampRequest(*provider_, algorithm, digest, options.request);
    if (!built) return std::unexpected(built.error());

    auto parsedUrl = net::parseUrl(url);
    if (!parsedUrl) return fail(CryptoErrorCode::InvalidArgument, "invalid TSA URL: " + parsedUrl.error().message);

    net::HttpRequest http;
    http.method = net::HttpMethod::Post;
    http.url = std::move(*parsedUrl);
    http.headers = {{"Content-Type", std::string(kTimeStampQueryContentType)},
                    {"Accept", std::string(kTimeStampReplyContentType)}};
    http.body.assign(reinterpret_cast<const char*>(built->der.data()), built->der.size());
    // Size cap and timeouts: left unset, so the NetworkPolicy values apply.

    CancelBridge bridge(cancel);
    auto response = network_->fetch(std::move(http), bridge.token());
    if (!response) {
        const net::NetFailure failure = net::failureOf(response.error());
        if (failure == net::NetFailure::Cancelled || cancel.cancelled())
            return fail(CryptoErrorCode::Cancelled, "operation cancelled");
        return fail(CryptoErrorCode::Network,
                    std::string(net::toString(failure)) + ": " + response.error().message);
    }
    if (response->status != 200)
        return fail(CryptoErrorCode::BadResponse, "the TSA answered with HTTP status " + std::to_string(response->status));
    const auto contentType = net::findHeader(response->headers, "Content-Type");
    if (!contentType || lowerMediaType(*contentType) != kTimeStampReplyContentType)
        return fail(CryptoErrorCode::BadResponse, "the TSA reply has no application/timestamp-reply Content-Type");
    if (response->body.empty()) return fail(CryptoErrorCode::BadResponse, "the TSA reply is empty");

    TimeStampResult result;
    result.request = std::move(*built);
    result.responseDer.assign(response->body.begin(), response->body.end());
    if (auto parsed = parseTimeStampResponse(result.responseDer)) result.response = std::move(*parsed);

    auto verification = verifyTimeStampResponse(result.responseDer, *provider_, result.request, [&] {
        TimeStampVerifyOptions vo = options.verify;
        vo.cancel = cancel;
        return vo;
    }());
    if (!verification) return std::unexpected(verification.error());
    result.verification = std::move(*verification);
    return result;
}

bool TimeStampClient::requestAsync(core::TaskScheduler& scheduler, core::IMainThreadDispatcher& dispatcher,
                                   core::AsyncScope& scope, CancelToken cancel, std::string url,
                                   DigestAlgorithm algorithm, Bytes digest, TimeStampClientOptions options,
                                   std::function<void(CryptoResult<TimeStampResult>)> done) const {
    TimeStampClient self = *this; // two shared_ptrs: the worker needs no reference to the caller's object
    return runCryptoAsync<TimeStampResult>(
        scheduler, dispatcher, scope, cancel,
        [self = std::move(self), url = std::move(url), algorithm, digest = std::move(digest),
         options = std::move(options)](const CancelToken& token) {
            return self.request(url, algorithm, digest, options, token);
        },
        std::move(done));
}

} // namespace rivet::crypto
