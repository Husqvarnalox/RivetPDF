// SPDX-License-Identifier: MPL-2.0
#include "crypto/revocation/Internal.hpp"

#include "crypto/X509Parse.hpp"
#include "net/Error.hpp"
#include "net/Url.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace rivet::crypto::revocation_detail {

namespace dr = rivet::crypto::der;

std::string hexOf(ByteView data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (std::uint8_t b : data) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0x0F];
    }
    return out;
}

CryptoResult<std::vector<RawExtension>> readExtensions(const dr::Element& seq) {
    if (seq.tag != dr::kSequence) return fail(CryptoErrorCode::CorruptData, "extensions are not a SEQUENCE");
    auto items = dr::children(seq);
    if (!items) return std::unexpected(items.error());
    std::vector<RawExtension> out;
    for (const dr::Element& ext : *items) {
        if (ext.tag != dr::kSequence) return fail(CryptoErrorCode::CorruptData, "extension is not a SEQUENCE");
        auto parts = dr::children(ext);
        if (!parts || parts->size() < 2 || parts->size() > 3) return fail(CryptoErrorCode::CorruptData, "malformed extension");
        if ((*parts)[0].tag != dr::kOid) return fail(CryptoErrorCode::CorruptData, "malformed extension OID");
        auto oidText = dr::decodeOid((*parts)[0].content);
        if (!oidText) return std::unexpected(oidText.error());
        RawExtension e;
        e.oid = *oidText;
        std::size_t i = 1;
        if ((*parts)[i].tag == dr::kBoolean) {
            if ((*parts)[i].content.size() != 1) return fail(CryptoErrorCode::CorruptData, "malformed critical flag");
            e.critical = (*parts)[i].content[0] != 0;
            ++i;
        }
        if (i >= parts->size() || (*parts)[i].tag != dr::kOctetString)
            return fail(CryptoErrorCode::CorruptData, "malformed extension value");
        e.value = (*parts)[i].content;
        out.push_back(std::move(e));
    }
    return out;
}

namespace {

struct AlgId {
    std::string oid;
    std::optional<dr::Element> params;
};

CryptoResult<AlgId> readAlgId(const dr::Element& e) {
    if (e.tag != dr::kSequence) return fail(CryptoErrorCode::CorruptData, "AlgorithmIdentifier is not a SEQUENCE");
    auto k = dr::children(e);
    if (!k || k->empty() || (*k)[0].tag != dr::kOid) return fail(CryptoErrorCode::CorruptData, "malformed AlgorithmIdentifier");
    auto o = dr::decodeOid((*k)[0].content);
    if (!o) return std::unexpected(o.error());
    AlgId a;
    a.oid = *o;
    if (k->size() > 1) a.params = (*k)[1];
    return a;
}

} // namespace

CryptoResult<SignatureScheme> resolveSignatureScheme(ByteView algDer, PublicKeyType keyType) {
    auto el = dr::readSingle(algDer);
    if (!el) return std::unexpected(el.error());
    auto alg = readAlgId(*el);
    if (!alg) return std::unexpected(alg.error());
    const std::string& o = alg->oid;
    SignatureScheme s;
    s.keyType = keyType;
    auto unsupported = [&]() -> std::unexpected<CryptoError> {
        return fail(CryptoErrorCode::UnsupportedAlgorithm, "unsupported signature algorithm " + o);
    };
    if (o == oid::kSha1WithRsa || o == oid::kSha256WithRsa || o == oid::kSha384WithRsa || o == oid::kSha512WithRsa) {
        s.keyType = PublicKeyType::Rsa;
        s.padding = SignaturePadding::Pkcs1;
        s.digest = o == oid::kSha1WithRsa     ? DigestAlgorithm::Sha1
                   : o == oid::kSha256WithRsa ? DigestAlgorithm::Sha256
                   : o == oid::kSha384WithRsa ? DigestAlgorithm::Sha384
                                              : DigestAlgorithm::Sha512;
    } else if (o == oid::kEcdsaSha1 || o == oid::kEcdsaSha256 || o == oid::kEcdsaSha384 || o == oid::kEcdsaSha512) {
        s.keyType = PublicKeyType::Ec;
        s.padding = SignaturePadding::Ecdsa;
        s.digest = o == oid::kEcdsaSha1     ? DigestAlgorithm::Sha1
                   : o == oid::kEcdsaSha256 ? DigestAlgorithm::Sha256
                   : o == oid::kEcdsaSha384 ? DigestAlgorithm::Sha384
                                            : DigestAlgorithm::Sha512;
    } else if (o == oid::kRsaPss) {
        s.keyType = PublicKeyType::Rsa;
        s.padding = SignaturePadding::Pss;
        DigestAlgorithm hash = DigestAlgorithm::Sha1;
        DigestAlgorithm mgfHash = DigestAlgorithm::Sha1;
        std::int64_t salt = 20;
        if (alg->params && alg->params->tag == dr::kSequence) {
            auto fields = dr::children(*alg->params);
            if (!fields) return std::unexpected(fields.error());
            for (const dr::Element& f : *fields) {
                auto inner = dr::children(f);
                if (!inner || inner->empty()) return fail(CryptoErrorCode::CorruptData, "malformed RSASSA-PSS parameters");
                if (f.tag == dr::contextTag(0, true)) {
                    auto h = readAlgId((*inner)[0]);
                    if (!h) return std::unexpected(h.error());
                    auto d = digestFromOid(h->oid);
                    if (!d) return unsupported();
                    hash = *d;
                } else if (f.tag == dr::contextTag(1, true)) {
                    auto m = readAlgId((*inner)[0]);
                    if (!m || m->oid != oid::kMgf1 || !m->params) return unsupported();
                    auto h = readAlgId(*m->params);
                    if (!h) return std::unexpected(h.error());
                    auto d = digestFromOid(h->oid);
                    if (!d) return unsupported();
                    mgfHash = *d;
                } else if (f.tag == dr::contextTag(2, true)) {
                    const dr::Element& n = (*inner)[0];
                    if (n.tag != dr::kInteger || n.content.size() > 4) return fail(CryptoErrorCode::CorruptData, "malformed PSS salt length");
                    salt = 0;
                    for (std::uint8_t b : n.content) salt = (salt << 8) | b;
                }
            }
        }
        if (mgfHash != hash) return unsupported();
        s.digest = hash;
        s.pssSaltLength = salt == static_cast<std::int64_t>(digestSize(hash)) ? -1 : static_cast<int>(salt);
    } else {
        return unsupported();
    }
    return s;
}

CryptoOk verifyWithCertificate(CryptoProvider& provider, const CertificateInfo& signer, ByteView algDer,
                               ByteView signedBytes, ByteView signatureBitString) {
    if (signatureBitString.empty() || signatureBitString[0] != 0)
        return fail(CryptoErrorCode::CorruptData, "malformed signature BIT STRING");
    auto scheme = resolveSignatureScheme(algDer, signer.publicKeyType);
    if (!scheme) return std::unexpected(scheme.error());
    if (signer.publicKeyType != PublicKeyType::Unknown && scheme->keyType != signer.publicKeyType)
        return fail(CryptoErrorCode::KeyMismatch, "signature algorithm does not match the signer's key type");
    if (scheme->padding == SignaturePadding::Gost || !provider.supports(scheme->digest))
        return fail(CryptoErrorCode::UnsupportedAlgorithm, "unsupported signature scheme");
    return provider.verifySignature(signer, *scheme, signedBytes, signatureBitString.subspan(1));
}

CryptoResult<Bytes> publicKeyBitsOf(const CertificateInfo& cert) {
    X509Extras extras;
    auto parsed = parseX509(cert.der, {}, &extras);
    if (!parsed) return std::unexpected(parsed.error());
    return std::move(extras.publicKeyBits);
}

CryptoResult<Bytes> hashOf(CryptoProvider& provider, DigestAlgorithm algorithm, ByteView data) {
    return provider.digest(algorithm, data);
}

// ---- HTTP ------------------------------------------------------------------------------

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

} // namespace

std::string lowerMediaType(std::string_view value) {
    const std::size_t semi = value.find(';');
    if (semi != std::string_view::npos) value = value.substr(0, semi);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool isGateRefusal(const CryptoError& error) {
    return error.code == CryptoErrorCode::Network &&
           (error.message.rfind("OnlineValidationOff", 0) == 0 || error.message.rfind("ConsentDenied", 0) == 0);
}

CryptoResult<net::HttpResponse> httpExchange(const net::NetworkClient& network, net::HttpMethod method,
                                             std::string_view url, std::string body, net::HttpHeaders headers,
                                             std::size_t maxResponseBytes, const CancelToken& cancel) {
    if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
    auto parsed = net::parseUrl(url);
    if (!parsed) return fail(CryptoErrorCode::InvalidArgument, "unusable URL: " + parsed.error().message);
    net::HttpRequest http;
    http.method = method;
    http.url = std::move(*parsed);
    http.headers = std::move(headers);
    http.body = std::move(body);
    http.maxResponseBytes = maxResponseBytes; // tightens the NetworkPolicy cap, never loosens it

    CancelBridge bridge(cancel);
    auto response = network.fetch(std::move(http), bridge.token());
    if (!response) {
        const net::NetFailure failure = net::failureOf(response.error());
        if (failure == net::NetFailure::Cancelled || cancel.cancelled())
            return fail(CryptoErrorCode::Cancelled, "operation cancelled");
        return fail(CryptoErrorCode::Network, std::string(net::toString(failure)) + ": " + response.error().message);
    }
    return std::move(*response);
}

CryptoResult<Bytes> decodePemCrl(ByteView text) {
    const std::string_view s(reinterpret_cast<const char*>(text.data()), text.size());
    constexpr std::string_view kBegin = "-----BEGIN X509 CRL-----";
    constexpr std::string_view kEnd = "-----END X509 CRL-----";
    const std::size_t b = s.find(kBegin);
    if (b == std::string_view::npos) return fail(CryptoErrorCode::CorruptData, "no PEM CRL block");
    const std::size_t start = b + kBegin.size();
    const std::size_t e = s.find(kEnd, start);
    if (e == std::string_view::npos) return fail(CryptoErrorCode::CorruptData, "unterminated PEM CRL block");
    Bytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    int pad = 0;
    for (std::size_t i = start; i < e; ++i) {
        const char c = s[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') { ++pad; continue; }
        else if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        else return fail(CryptoErrorCode::CorruptData, "invalid character in PEM CRL");
        if (pad) return fail(CryptoErrorCode::CorruptData, "data after PEM padding");
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
        if (out.size() > (std::size_t{64} << 20)) return fail(CryptoErrorCode::LimitExceeded, "PEM CRL too large");
    }
    if (out.empty()) return fail(CryptoErrorCode::CorruptData, "empty PEM CRL");
    return out;
}

} // namespace rivet::crypto::revocation_detail
