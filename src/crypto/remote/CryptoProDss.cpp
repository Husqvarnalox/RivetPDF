// SPDX-License-Identifier: MPL-2.0
#include "crypto/remote/CryptoProDss.hpp"

#include "crypto/remote/JsonLite.hpp"
#include "net/Error.hpp"
#include "net/Url.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <thread>

namespace rivet::crypto {

using remote::JsonObjectWriter;
using remote::JsonValue;

// =============================================================================
// ASSUMED DSS WIRE FORMAT. Everything below is an assumption about a REST-style
// CryptoPro DSS "sign hash" API, NOT verified against vendor documentation or a
// real service. It is the single place to correct once real documentation or a
// test tenant is available (ADR-0047, "Assumptions").
// =============================================================================
namespace dss {

// Paths relative to CryptoProDssOptions::serviceUrl.
constexpr std::string_view kTokenPath = "/oauth/token";          // ASSUMPTION: OAuth2 password grant, form-encoded
constexpr std::string_view kIdentitiesPath = "/api/v1/identities";
constexpr std::string_view kSignPath = "/api/v1/sign";
constexpr std::string_view kConfirmPath = "/api/v1/sign/confirm";
constexpr std::string_view kStatusPath = "/api/v1/sign/status";
constexpr std::string_view kCancelPath = "/api/v1/sign/cancel";

// ASSUMPTION: the net layer forbids a caller-set Authorization header (no
// credential forwarding), so the access token travels in the JSON body of the
// https POST. A real DSS most likely wants "Authorization: Bearer"; that needs
// a deliberate net-layer extension and is recorded as follow-up in the ADR.
constexpr std::string_view kTokenField = "access_token";

// Reply "status" words of sign / status / confirm calls.
constexpr std::string_view kSigned = "signed";
constexpr std::string_view kPending = "pending";                       // waiting for confirmation on the user's device
constexpr std::string_view kConfirmationRequired = "confirmation_required";
constexpr std::string_view kConfirmationWrong = "confirmation_wrong";
constexpr std::string_view kRejected = "rejected";                     // the user declined on the device

// Error codes in {"error": "..."} bodies of non-2xx replies.
constexpr std::string_view kErrNotQualified = "certificate_not_qualified";
constexpr std::string_view kErrPinLocked = "confirmation_locked";
constexpr std::string_view kErrExpired = "transaction_expired";

std::string_view algorithmName(SignatureAlgorithm a) { return toString(a); } // ASSUMPTION: Rivet's names

} // namespace dss

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

// Sleeps in short slices; false when cancelled.
bool waitFor(std::chrono::milliseconds duration, const CancelToken& cancel) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (!cancel.cancelled()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= end) return true;
        std::this_thread::sleep_for(std::min(std::chrono::duration_cast<std::chrono::milliseconds>(end - now) +
                                                 std::chrono::milliseconds(1),
                                             std::chrono::milliseconds(10)));
    }
    return false;
}

std::string lowerMediaType(std::string_view value) {
    if (const auto semi = value.find(';'); semi != std::string_view::npos) value = value.substr(0, semi);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Wipes a std::string that held a secret.
void wipe(std::string& s) {
    if (!s.empty()) secureZero(s.data(), s.size());
    s.clear();
}

std::string hexOf(ByteView bytes) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (auto b : bytes) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out;
}

struct Reply {
    int status = 0;
    JsonValue json; // Kind::Null when the body was empty / not JSON
};

// What a sign / confirm / status reply means.
struct SignState {
    std::string status;
    std::string transactionId;
    std::string kind; // "pin" / "otp"
    std::optional<long long> retriesLeft;
    Bytes signature;
};

} // namespace

// ---- construction -------------------------------------------------------------

CryptoResult<std::shared_ptr<CryptoProDssProvider>>
CryptoProDssProvider::create(std::shared_ptr<const net::NetworkClient> network, std::shared_ptr<CryptoProvider> crypto,
                             CryptoProDssOptions options) {
    if (!network || !crypto) return fail(CryptoErrorCode::InvalidArgument, "DSS provider needs a network client and a crypto provider");
    auto url = net::parseUrl(options.serviceUrl);
    if (!url) return fail(CryptoErrorCode::InvalidArgument, "invalid DSS service URL");
    if (!url->isHttps())
        return fail(CryptoErrorCode::InvalidArgument, "the DSS service URL must use https (credentials and tokens are never sent in clear text)");
    if (url->target.find('?') != std::string::npos)
        return fail(CryptoErrorCode::InvalidArgument, "the DSS service URL must not contain a query");
    if (options.privacy == RemotePrivacyMode::WholeDocument && options.wholeDocumentConsent != kDssWholeDocumentConsentText)
        return fail(CryptoErrorCode::InvalidArgument,
                    "whole-document mode uploads the document and needs the confirmed consent text");
    options.maxRetries = std::clamp(options.maxRetries, 0, 5);
    options.maxPinAttempts = std::clamp(options.maxPinAttempts, 1, 5);

    std::shared_ptr<CryptoProDssProvider> p(new CryptoProDssProvider());
    p->network_ = std::move(network);
    p->crypto_ = std::move(crypto);
    p->base_ = url->toString();
    while (p->base_.size() > 1 && p->base_.back() == '/') p->base_.pop_back();
    p->options_ = std::move(options);
    return p;
}

void CryptoProDssProvider::setAccessToken(SecretBuffer token) {
    std::lock_guard lock(mutex_);
    token_ = std::move(token);
}
bool CryptoProDssProvider::hasAccessToken() const {
    std::lock_guard lock(mutex_);
    return !token_.empty();
}
void CryptoProDssProvider::forgetAccessToken() {
    std::lock_guard lock(mutex_);
    token_.clear();
}

// ---- transport helpers --------------------------------------------------------

namespace {

struct Exchange {
    const net::NetworkClient& network;
    const CryptoProDssOptions& options;
    const std::string& base;

    // POST with bounded retry on HTTP 5xx. The same body (hence the same
    // request id) is re-sent so a retry is the same logical request. Returns the
    // final reply for any non-5xx status; 5xx after the retries -> ServerError.
    // `body` is wiped by the caller. Redirects are refused (maxRedirects 0): a
    // redirect could re-send the token to another host.
    CryptoResult<Reply> post(std::string_view path, std::string contentType, const std::string& body,
                             const CancelToken& cancel) const {
        auto url = net::parseUrl(base + std::string(path));
        if (!url) return fail(CryptoErrorCode::InvalidArgument, "invalid DSS endpoint URL");
        for (int attempt = 0;; ++attempt) {
            if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
            net::HttpRequest http;
            http.method = net::HttpMethod::Post;
            http.url = *url;
            http.headers = {{"Content-Type", contentType}, {"Accept", "application/json"}};
            http.body = body;
            http.maxRedirects = 0;
            CancelBridge bridge(cancel);
            auto response = network.fetch(std::move(http), bridge.token());
            if (!response) {
                const net::NetFailure failure = net::failureOf(response.error());
                if (failure == net::NetFailure::Cancelled || cancel.cancelled())
                    return fail(CryptoErrorCode::Cancelled, "operation cancelled");
                if (failure == net::NetFailure::Timeout) return fail(CryptoErrorCode::Timeout, "the service did not answer in time");
                return fail(CryptoErrorCode::Network, std::string(net::toString(failure)) + ": " + response.error().message);
            }
            if (response->status >= 500 && response->status <= 599) {
                if (attempt >= options.maxRetries)
                    return fail(CryptoErrorCode::ServerError,
                                "the service failed (HTTP " + std::to_string(response->status) + ") after " +
                                    std::to_string(attempt + 1) + " attempt(s)");
                if (!waitFor(options.retryBackoff * (attempt + 1), cancel))
                    return fail(CryptoErrorCode::Cancelled, "operation cancelled");
                continue;
            }
            Reply reply;
            reply.status = response->status;
            const auto contentTypeHeader = net::findHeader(response->headers, "Content-Type");
            const bool json = contentTypeHeader && lowerMediaType(*contentTypeHeader) == "application/json";
            if (json && !response->body.empty()) {
                auto parsed = remote::parseJson(response->body);
                if (parsed) reply.json = std::move(*parsed);
                else if (reply.status == 200) return std::unexpected(parsed.error());
            }
            return reply;
        }
    }
};

// Maps a non-200 reply to the error taxonomy. Server-supplied text is never
// copied into messages (it could echo secrets); only the numeric status is.
CryptoError errorForStatus(const Reply& reply, bool signing) {
    const std::string code = reply.json.stringAt("error").value_or("");
    const std::string http = "HTTP " + std::to_string(reply.status);
    if (code == dss::kErrNotQualified) return {CryptoErrorCode::NotQualified, "the service refused: certificate is not qualified (" + http + ")"};
    if (code == dss::kErrPinLocked) return {CryptoErrorCode::PinLocked, "the service locked the PIN / OTP (" + http + ")"};
    if (code == dss::kErrExpired) return {CryptoErrorCode::SessionExpired, "the signing transaction expired (" + http + ")"};
    switch (reply.status) {
    case 401:
    case 403: return {CryptoErrorCode::AuthFailed, "the service rejected the credentials or token (" + http + ")"};
    case 429: return {CryptoErrorCode::RateLimited, "the service rate limit / quota was exceeded (" + http + ")"};
    default: break;
    }
    if (!signing && reply.status == 400 && (code == "invalid_grant" || code == "invalid_client"))
        return {CryptoErrorCode::AuthFailed, "the service rejected the credentials (" + http + ")"};
    return {CryptoErrorCode::BadResponse, "unexpected reply from the service (" + http + ")"};
}

CryptoResult<SignState> readSignState(const Reply& reply) {
    if (reply.status != 200) return std::unexpected(errorForStatus(reply, true));
    if (!reply.json.isObject()) return fail(CryptoErrorCode::BadResponse, "the service reply is not a JSON object");
    SignState s;
    s.status = reply.json.stringAt("status").value_or("");
    s.transactionId = reply.json.stringAt("transaction_id").value_or("");
    s.kind = reply.json.stringAt("confirmation_kind").value_or("");
    s.retriesLeft = reply.json.intAt("retries_left");
    if (s.status == dss::kSigned) {
        auto encoded = reply.json.stringAt("signature");
        auto decoded = encoded ? remote::base64Decode(*encoded) : std::nullopt;
        if (!decoded || decoded->empty()) return fail(CryptoErrorCode::BadResponse, "the service returned no valid signature value");
        s.signature = std::move(*decoded);
    } else if (s.status == dss::kPending || s.status == dss::kConfirmationRequired || s.status == dss::kConfirmationWrong) {
        if (s.transactionId.empty() || s.transactionId.size() > 256)
            return fail(CryptoErrorCode::BadResponse, "the service reply has no usable transaction id");
    } else if (s.status != dss::kRejected) {
        return fail(CryptoErrorCode::BadResponse, "the service reply has an unknown status");
    }
    return s;
}

} // namespace

// ---- operations ----------------------------------------------------------------

CryptoOk CryptoProDssProvider::authenticate(const RemoteCredentials& credentials, const CancelToken& cancel) {
    if (credentials.userId.empty() || credentials.secret.empty())
        return fail(CryptoErrorCode::InvalidArgument, "user id and password are required");
    std::string body = "grant_type=password&client_id=" + remote::formEncode(options_.clientId) +
                       "&username=" + remote::formEncode(credentials.userId) + "&password=" +
                       remote::formEncode(std::string_view(credentials.secret.chars(), credentials.secret.size()));
    const Exchange ex{*network_, options_, base_};
    auto reply = ex.post(dss::kTokenPath, "application/x-www-form-urlencoded", body, cancel);
    wipe(body);
    if (!reply) return std::unexpected(reply.error());
    if (reply->status != 200) return std::unexpected(errorForStatus(*reply, false));
    auto token = reply->json.stringAt("access_token");
    if (!token || token->empty() || token->size() > 8192)
        return fail(CryptoErrorCode::BadResponse, "the service returned no access token");
    setAccessToken(SecretBuffer(std::string_view(*token)));
    wipe(*token);
    return {};
}

CryptoResult<std::vector<RemoteIdentityInfo>> CryptoProDssProvider::listIdentities(const CancelToken& cancel) {
    std::string token;
    {
        std::lock_guard lock(mutex_);
        token.assign(token_.chars(), token_.size());
    }
    if (token.empty()) return fail(CryptoErrorCode::AuthFailed, "not authenticated");
    std::string body = JsonObjectWriter().add(dss::kTokenField, token).str();
    wipe(token);
    const Exchange ex{*network_, options_, base_};
    auto reply = ex.post(dss::kIdentitiesPath, "application/json", body, cancel);
    wipe(body);
    if (!reply) return std::unexpected(reply.error());
    if (reply->status != 200) return std::unexpected(errorForStatus(*reply, false));
    const JsonValue* list = reply->json.find("identities");
    if (!list || !list->isArray() || list->items.size() > 64)
        return fail(CryptoErrorCode::BadResponse, "the identity list is missing or too long");
    std::vector<RemoteIdentityInfo> out;
    for (const JsonValue& item : list->items) {
        RemoteIdentityInfo info;
        info.id = item.stringAt("id").value_or("");
        info.displayName = item.stringAt("display_name").value_or("");
        const JsonValue* chain = item.find("certificate_chain");
        if (info.id.empty() || info.id.size() > 256 || !chain || !chain->isArray() || chain->items.empty() ||
            chain->items.size() > 8)
            return fail(CryptoErrorCode::BadResponse, "malformed identity entry");
        for (const JsonValue& c : chain->items) {
            auto der = c.kind == JsonValue::Kind::String ? remote::base64Decode(c.text) : std::nullopt;
            if (!der || der->empty() || der->size() > (1u << 20))
                return fail(CryptoErrorCode::BadResponse, "malformed certificate in identity entry");
            info.chainDer.push_back(std::move(*der));
        }
        out.push_back(std::move(info));
    }
    return out;
}

CryptoResult<Bytes> CryptoProDssProvider::signHash(const RemoteSignRequest& request, PinPrompt* prompt,
                                                   const CancelToken& cancel) {
    if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
    if (request.identityId.empty() || request.digest.empty())
        return fail(CryptoErrorCode::InvalidArgument, "identity and digest are required");
    const bool whole = options_.privacy == RemotePrivacyMode::WholeDocument;
    if (whole && (!request.document || request.document->empty()))
        return fail(CryptoErrorCode::InvalidArgument, "whole-document mode needs the document bytes");
    auto random = crypto_->randomBytes(16);
    if (!random) return std::unexpected(random.error());

    auto tokenCopy = [&]() {
        std::lock_guard lock(mutex_);
        return std::string(token_.chars(), token_.size());
    };
    std::string token = tokenCopy();
    if (token.empty()) return fail(CryptoErrorCode::AuthFailed, "not authenticated");

    const Exchange ex{*network_, options_, base_};

    // Best-effort notice to the service that the user gave up; never blocks long
    // and never carries the document.
    auto abortTransaction = [&](const std::string& transactionId) {
        if (transactionId.empty()) return;
        std::string t = tokenCopy();
        std::string body = JsonObjectWriter().add(dss::kTokenField, t).add("transaction_id", transactionId).str();
        wipe(t);
        if (auto url = net::parseUrl(base_ + std::string(dss::kCancelPath))) {
            net::HttpRequest http;
            http.method = net::HttpMethod::Post;
            http.url = std::move(*url);
            http.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
            http.body = body;
            http.maxRedirects = 0;
            http.timeouts.total = std::chrono::milliseconds(5000);
            (void)network_->fetch(std::move(http), net::CancelToken{});
        }
        wipe(body);
    };

    JsonObjectWriter w;
    w.add(dss::kTokenField, token)
        .add("request_id", hexOf(*random)) // idempotency key, stable across 5xx retries
        .add("identity_id", request.identityId)
        .add("algorithm", dss::algorithmName(request.algorithm))
        .add("digest", remote::base64Encode(request.digest))
        .add("privacy", whole ? "whole_document" : "hash_only");
    // HashOnly: the document is never serialised, whatever the caller supplied.
    if (whole) w.add("document", remote::base64Encode(*request.document));
    wipe(token);
    std::string body = w.str();
    auto first = ex.post(dss::kSignPath, "application/json", body, cancel);
    wipe(body);
    if (!first) return std::unexpected(first.error());
    auto state = readSignState(*first);
    if (!state) return std::unexpected(state.error());

    std::string transactionId = state->transactionId;
    auto deadline = std::chrono::steady_clock::now() + options_.confirmationTimeout;
    int pinAttempts = 0;
    for (;;) {
        if (cancel.cancelled()) {
            abortTransaction(transactionId);
            return fail(CryptoErrorCode::Cancelled, "operation cancelled");
        }
        if (!state->transactionId.empty()) transactionId = state->transactionId;

        if (state->status == dss::kSigned) return std::move(state->signature);
        if (state->status == dss::kRejected)
            return fail(CryptoErrorCode::ConfirmationRejected, "the confirmation was declined");

        std::string next; // endpoint of the next call
        std::string nextBody;
        if (state->status == dss::kPending) {
            if (std::chrono::steady_clock::now() >= deadline) {
                abortTransaction(transactionId);
                return fail(CryptoErrorCode::Timeout, "timed out waiting for the signing confirmation");
            }
            if (!waitFor(options_.pollInterval, cancel)) continue; // loop top reports Cancelled
            std::string t = tokenCopy();
            nextBody = JsonObjectWriter().add(dss::kTokenField, t).add("transaction_id", transactionId).str();
            wipe(t);
            next = std::string(dss::kStatusPath);
        } else { // confirmation_required / confirmation_wrong
            const bool wrong = state->status == dss::kConfirmationWrong;
            if (state->retriesLeft && *state->retriesLeft <= 0) {
                abortTransaction(transactionId);
                return fail(CryptoErrorCode::PinLocked, "the service locked the PIN / OTP");
            }
            if (!prompt) {
                abortTransaction(transactionId);
                return fail(CryptoErrorCode::ConfirmationRequired, "the service needs a PIN / OTP and no prompt is available");
            }
            if (++pinAttempts > options_.maxPinAttempts) {
                abortTransaction(transactionId);
                return fail(CryptoErrorCode::ConfirmationRejected, "the PIN / OTP was rejected");
            }
            PinRequest pr;
            pr.providerName = std::string(name());
            pr.label = state->kind == "otp" ? "DSS one-time code" : "DSS PIN";
            pr.attempt = pinAttempts;
            pr.retriesLeft = state->retriesLeft ? std::optional<int>(static_cast<int>(*state->retriesLeft)) : std::nullopt;
            pr.reason = !wrong ? PinPromptReason::Required
                               : (state->retriesLeft && *state->retriesLeft == 1 ? PinPromptReason::LastAttempt
                                                                                 : PinPromptReason::Incorrect);
            auto code = prompt->requestPin(pr, cancel); // no lock held
            if (!code || cancel.cancelled()) {
                abortTransaction(transactionId);
                return fail(CryptoErrorCode::Cancelled, "operation cancelled");
            }
            std::string t = tokenCopy();
            std::string codeText(code->chars(), code->size());
            nextBody = JsonObjectWriter()
                           .add(dss::kTokenField, t)
                           .add("transaction_id", transactionId)
                           .add("confirmation_kind", state->kind.empty() ? "pin" : state->kind)
                           .add("confirmation_code", codeText)
                           .str();
            wipe(t);
            wipe(codeText);
            code->clear();
            next = std::string(dss::kConfirmPath);
            deadline = std::chrono::steady_clock::now() + options_.confirmationTimeout;
        }
        auto reply = ex.post(next, "application/json", nextBody, cancel);
        wipe(nextBody);
        if (!reply) {
            if (reply.error().code == CryptoErrorCode::Cancelled) abortTransaction(transactionId);
            return std::unexpected(reply.error());
        }
        state = readSignState(*reply);
        if (!state) return std::unexpected(state.error());
    }
}

} // namespace rivet::crypto
