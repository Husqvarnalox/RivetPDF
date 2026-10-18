// SPDX-License-Identifier: MPL-2.0
#pragma once

// In-process MOCK of a CryptoPro-DSS-like signing service (ADR-0047). It
// speaks exactly the ASSUMED wire format of src/crypto/remote/CryptoProDss.cpp
// and nothing else: passing these tests proves the adapter against its own
// assumptions, never against a real DSS. The "service" signs with a key
// generated at test run time; nothing is committed.

#include "TestSupport.hpp"
#include "crypto/remote/JsonLite.hpp"
#include "net/Transport.hpp"

#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace rivet::crypto::testing {

struct DssScript {
    enum class Sign { Immediate, Pin, Pending, DeviceRejects, NotQualified, RateLimit, Unauthorized, BadSignature };
    Sign sign = Sign::Immediate;
    std::string password = "correct-horse-password";
    std::string token = "TOKEN-9f3a7c11d2e84b0aa6";
    std::string pin = "246810";
    int pinRetries = 3;
    int pendingPolls = 2;       // status polls answered "pending" before the result
    int fail5xxFirst = 0;       // sign calls answered with HTTP 503 first
    bool timeoutOnSign = false; // transport-level timeout on the sign call
    std::string redirectSignTo{}; // answer the sign call with a 307 to this URL
    std::function<void()> onStatusPoll{};
};

struct SeenRequest {
    std::string url;
    std::string path;
    std::string body;
    net::HttpHeaders headers;
};

class MockDssTransport final : public net::INetworkTransport {
public:
    MockDssTransport(TestKey leaf, std::vector<Bytes> chainDer, DssScript script)
        : leaf_(std::move(leaf)), chain_(std::move(chainDer)), script_(std::move(script)) {}

    DssScript& script() { return script_; }

    core::Result<net::HttpResponse> perform(const net::HttpRequest& request, const net::NetworkPolicy&,
                                            net::CancelToken) override {
        std::unique_lock lock(mutex_);
        const std::string path = request.url.target;
        seen_.push_back({request.url.toString(), path, request.body, request.headers});
        auto reply = [&](int status, std::string body) {
            net::HttpResponse r;
            r.status = status;
            r.headers = {{"Content-Type", "application/json"}};
            r.body = std::move(body);
            r.finalUrl = request.url;
            return core::Result<net::HttpResponse>(std::move(r));
        };
        auto error = [&](int status, const std::string& code) {
            return reply(status, remote::JsonObjectWriter().add("error", code).str());
        };

        if (path == "/dss/oauth/token") {
            const std::string needle = "&password=" + remote::formEncode(script_.password);
            if (request.body.find("grant_type=password") != std::string::npos &&
                request.body.find(needle) != std::string::npos)
                return reply(200, remote::JsonObjectWriter().add("access_token", script_.token).add("token_type", "bearer").addInt("expires_in", 3600).str());
            return error(400, "invalid_grant");
        }

        auto parsed = remote::parseJson(request.body);
        if (!parsed) return error(400, "bad_request");
        if (parsed->stringAt("access_token").value_or("") != script_.token) return error(401, "invalid_token");

        if (path == "/dss/api/v1/identities") {
            std::string chain = "[";
            for (std::size_t i = 0; i < chain_.size(); ++i) chain += (i ? "," : "") + remote::jsonQuote(remote::base64Encode(chain_[i]));
            chain += "]";
            const std::string one = remote::JsonObjectWriter().add("id", "id-1").add("display_name", "Mock Cloud Identity (test)").addRaw("certificate_chain", chain).str();
            return reply(200, "{\"identities\":[" + one + "]}");
        }
        if (path == "/dss/api/v1/sign") {
            ++signCalls_;
            if (script_.timeoutOnSign) return net::netFail<net::HttpResponse>(net::NetFailure::Timeout, "mock timeout");
            if (!script_.redirectSignTo.empty()) {
                net::HttpResponse r;
                r.status = 307;
                r.headers = {{"Location", script_.redirectSignTo}};
                r.finalUrl = request.url;
                return core::Result<net::HttpResponse>(std::move(r));
            }
            if (script_.fail5xxFirst > 0) {
                --script_.fail5xxFirst;
                return reply(503, "{}");
            }
            requestIds_.push_back(parsed->stringAt("request_id").value_or(""));
            algorithm_ = parsed->stringAt("algorithm").value_or("");
            auto digest = remote::base64Decode(parsed->stringAt("digest").value_or("")).value_or(Bytes{});
            digest_ = digest;
            polls_ = 0;
            pinTries_ = 0;
            retriesLeft_ = script_.pinRetries;
            switch (script_.sign) {
            case DssScript::Sign::NotQualified: return error(403, "certificate_not_qualified");
            case DssScript::Sign::RateLimit: return error(429, "quota_exceeded");
            case DssScript::Sign::Unauthorized: return error(401, "invalid_token");
            case DssScript::Sign::Pin: return confirmation(reply, "confirmation_required");
            case DssScript::Sign::Pending:
            case DssScript::Sign::DeviceRejects: return reply(200, transaction("pending"));
            case DssScript::Sign::BadSignature:
            case DssScript::Sign::Immediate: return signedReply(reply);
            }
        }
        if (path == "/dss/api/v1/sign/status") {
            ++statusCalls_;
            auto hook = script_.onStatusPoll;
            ++polls_;
            const bool done = polls_ > script_.pendingPolls;
            lock.unlock();
            if (hook) hook();
            lock.lock();
            if (!done) return reply(200, transaction("pending"));
            if (script_.sign == DssScript::Sign::DeviceRejects) return reply(200, transaction("rejected"));
            return signedReply(reply);
        }
        if (path == "/dss/api/v1/sign/confirm") {
            ++pinTries_;
            if (parsed->stringAt("confirmation_code").value_or("") == script_.pin) return signedReply(reply);
            if (--retriesLeft_ <= 0) return error(403, "confirmation_locked");
            return confirmation(reply, "confirmation_wrong");
        }
        if (path == "/dss/api/v1/sign/cancel") {
            ++cancelCalls_;
            return reply(200, "{}");
        }
        return error(404, "not_found");
    }

    std::vector<SeenRequest> seen() const {
        std::lock_guard lock(mutex_);
        return seen_;
    }
    std::size_t count(const std::string& path) const {
        std::lock_guard lock(mutex_);
        std::size_t n = 0;
        for (const auto& s : seen_) n += s.path == path;
        return n;
    }
    std::size_t signCalls() const { std::lock_guard lock(mutex_); return signCalls_; }
    std::size_t statusCalls() const { std::lock_guard lock(mutex_); return statusCalls_; }
    std::size_t cancelCalls() const { std::lock_guard lock(mutex_); return cancelCalls_; }
    int pinTries() const { std::lock_guard lock(mutex_); return pinTries_; }
    std::vector<std::string> requestIds() const { std::lock_guard lock(mutex_); return requestIds_; }

private:
    static std::string transaction(const std::string& status) {
        return remote::JsonObjectWriter().add("status", status).add("transaction_id", "tx-1").str();
    }
    template <typename Reply>
    core::Result<net::HttpResponse> confirmation(Reply& reply, const std::string& status) {
        return reply(200, remote::JsonObjectWriter().add("status", status).add("transaction_id", "tx-1").add("confirmation_kind", "pin").addInt("retries_left", retriesLeft_).str());
    }
    template <typename Reply>
    core::Result<net::HttpResponse> signedReply(Reply& reply) {
        Bytes sig = rawSign();
        if (script_.sign == DssScript::Sign::BadSignature && !sig.empty()) sig[sig.size() / 2] ^= 0x5A;
        return reply(200, remote::JsonObjectWriter().add("status", "signed").add("signature", remote::base64Encode(sig)).str());
    }
    // The service's private-key operation over the received DIGEST.
    Bytes rawSign() const {
        const char* md = algorithm_.find("Sha384") != std::string::npos ? "SHA384" : algorithm_.find("Sha512") != std::string::npos ? "SHA512" : "SHA256";
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(leaf_.pkey.get(), nullptr);
        Bytes out;
        if (ctx && EVP_PKEY_sign_init(ctx) == 1) {
            const bool rsa = algorithm_.rfind("Rsa", 0) == 0;
            bool ok = true;
            if (rsa) ok = EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) == 1;
            ok = ok && EVP_PKEY_CTX_set_signature_md(ctx, EVP_get_digestbyname(md)) == 1;
            std::size_t len = 0;
            if (ok && EVP_PKEY_sign(ctx, nullptr, &len, digest_.data(), digest_.size()) == 1) {
                out.resize(len);
                if (EVP_PKEY_sign(ctx, out.data(), &len, digest_.data(), digest_.size()) == 1) out.resize(len);
                else out.clear();
            }
        }
        EVP_PKEY_CTX_free(ctx);
        return out;
    }

    mutable std::mutex mutex_;
    TestKey leaf_;
    std::vector<Bytes> chain_;
    DssScript script_;
    std::vector<SeenRequest> seen_;
    std::vector<std::string> requestIds_;
    std::string algorithm_;
    Bytes digest_;
    int polls_ = 0, pinTries_ = 0, retriesLeft_ = 3;
    std::size_t signCalls_ = 0, statusCalls_ = 0, cancelCalls_ = 0;
};

} // namespace rivet::crypto::testing
