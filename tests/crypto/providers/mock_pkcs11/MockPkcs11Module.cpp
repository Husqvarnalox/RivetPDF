// SPDX-License-Identifier: MPL-2.0
//
// TEST-ONLY mock PKCS#11 module: a software token with one slot, built on
// OpenSSL, loaded by the PKCS#11 provider through dlopen exactly like a vendor
// module. Behaviour is selected at run time with rivet_mock_set_scenario():
//
//   ok                     RSA-2048 + EC P-256 identities, login required, PIN "1234"
//   wrong-pin              like ok; failures raise USER_PIN_COUNT_LOW then USER_PIN_FINAL_TRY (never locks)
//   locked                 C_Login returns CKR_PIN_LOCKED
//   locked-flag            token reports CKF_USER_PIN_LOCKED
//   token-removed          C_Sign returns CKR_DEVICE_REMOVED
//   session-expired-once   first C_Sign returns CKR_SESSION_HANDLE_INVALID (session and login are dropped)
//   session-expired-always every C_Sign returns CKR_SESSION_HANDLE_INVALID
//   key-missing            certificates only, no key objects
//   expired-cert           leaf certificates are expired
//   no-slots               C_GetSlotList reports no token
//   init-fails             C_Initialize returns CKR_GENERAL_ERROR
//   already-initialized    C_Initialize returns CKR_CRYPTOKI_ALREADY_INITIALIZED
//   raw-mechanisms         the token lists only CKM_RSA_PKCS, CKM_RSA_PKCS_PSS and CKM_ECDSA
//   no-mechlist            C_GetMechanismList is not supported
//   sparse                 C_GetSlotInfo / C_GetSessionInfo / C_GetMechanismList entries are NULL
//   open-token             no login required, private objects visible
//
// The mock is not a security product: the PIN is a fixed string.

#include "TestSupport.hpp"
#include "Pkcs11Abi.hpp"

#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#define MOCK_EXPORT extern "C" __declspec(dllexport)
#else
#define MOCK_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using namespace rivet::crypto::p11;
namespace testing = rivet::crypto::testing;

constexpr CK_SLOT_ID kSlot = 7;
constexpr const char* kPin = "1234";

struct Object {
    CK_OBJECT_HANDLE handle = 0;
    CK_OBJECT_CLASS cls = 0;
    std::vector<std::uint8_t> id;
    std::vector<std::uint8_t> value; // certificate DER
    std::string label;
    bool priv = false;               // hidden until login
    CK_KEY_TYPE keyType = 0;
    EVP_PKEY* key = nullptr;         // non-owning, private keys only
};

struct Session {
    CK_SLOT_ID slot = 0;
    std::vector<CK_OBJECT_HANDLE> found;
    std::size_t position = 0;
    bool signActive = false;
    CK_MECHANISM_TYPE mechanism = 0;
    CK_RSA_PKCS_PSS_PARAMS pss{};
    bool hasPss = false;
    CK_OBJECT_HANDLE key = 0;
};

struct Identities {
    testing::TestKey ca, rsa, ec;
};

struct State {
    std::mutex mutex;
    std::string scenario = "ok";
    bool initialized = false;
    bool loggedIn = false;
    int failures = 0;
    bool expiredDone = false;
    unsigned loginCalls = 0;
    unsigned signCalls = 0;
    CK_MECHANISM_TYPE lastMechanism = 0;
    unsigned long lastPssSaltLength = 0;
    CK_SESSION_HANDLE nextHandle = 100;
    std::map<CK_SESSION_HANDLE, Session> sessions;
    std::vector<Object> objects;
    std::unique_ptr<Identities> valid;
    std::unique_ptr<Identities> expired;
};

State& state() {
    static State s;
    return s;
}

bool is(const char* name) { return state().scenario == name; }

void padCopy(unsigned char* dst, std::size_t size, const char* text) {
    std::memset(dst, ' ', size);
    std::memcpy(dst, text, std::min(size, std::strlen(text)));
}

void rebuildLocked() {
    State& s = state();
    const bool wantExpired = is("expired-cert");
    std::unique_ptr<Identities>& slot = wantExpired ? s.expired : s.valid;
    if (!slot) {
        slot = std::make_unique<Identities>();
        testing::CertSpec ca;
        ca.cn = "Mock Root CA";
        ca.keyKind = "ec256";
        ca.ca = true;
        slot->ca = testing::makeCert(ca);
        testing::CertSpec rsa;
        rsa.cn = "Mock RSA Signer";
        rsa.keyKind = "rsa2048";
        rsa.issuer = &slot->ca;
        testing::CertSpec ec;
        ec.cn = "Mock EC Signer";
        ec.keyKind = "ec256";
        ec.issuer = &slot->ca;
        if (wantExpired) {
            rsa.notBeforeOffsetSec = ec.notBeforeOffsetSec = -3600L * 24 * 400;
            rsa.notAfterOffsetSec = ec.notAfterOffsetSec = -3600L * 24 * 30;
        }
        slot->rsa = testing::makeCert(rsa);
        slot->ec = testing::makeCert(ec);
    }
    s.objects.clear();
    CK_OBJECT_HANDLE next = 1;
    const bool openToken = is("open-token");
    const bool keys = !is("key-missing");
    auto addCert = [&](const testing::TestKey& k, std::uint8_t id, const char* label) {
        Object o;
        o.handle = next++;
        o.cls = CKO_CERTIFICATE;
        o.id = {id};
        const auto der = testing::certDer(k);
        o.value.assign(der.begin(), der.end());
        o.label = label;
        s.objects.push_back(std::move(o));
    };
    auto addKeys = [&](const testing::TestKey& k, std::uint8_t id, CK_KEY_TYPE type) {
        Object pub;
        pub.handle = next++;
        pub.cls = CKO_PUBLIC_KEY;
        pub.id = {id};
        pub.keyType = type;
        s.objects.push_back(std::move(pub));
        Object priv;
        priv.handle = next++;
        priv.cls = CKO_PRIVATE_KEY;
        priv.id = {id};
        priv.keyType = type;
        priv.priv = !openToken;
        priv.key = k.pkey.get();
        s.objects.push_back(std::move(priv));
    };
    addCert(slot->rsa, 0x01, "rsa-cert");
    if (keys) addKeys(slot->rsa, 0x01, CKK_RSA);
    addCert(slot->ec, 0x02, "ec-cert");
    if (keys) addKeys(slot->ec, 0x02, CKK_EC);
    addCert(slot->ca, 0x03, "ca-cert");
}

CK_FLAGS tokenFlags() {
    State& s = state();
    CK_FLAGS flags = CKF_RNG | CKF_USER_PIN_INITIALIZED | CKF_TOKEN_INITIALIZED;
    if (!is("open-token")) flags |= CKF_LOGIN_REQUIRED;
    if (s.failures == 1) flags |= CKF_USER_PIN_COUNT_LOW;
    if (s.failures >= 2) flags |= CKF_USER_PIN_COUNT_LOW | CKF_USER_PIN_FINAL_TRY;
    if (is("locked-flag")) flags |= CKF_USER_PIN_LOCKED;
    return flags;
}

const Object* findObject(CK_OBJECT_HANDLE h) {
    for (const Object& o : state().objects)
        if (o.handle == h) return &o;
    return nullptr;
}

bool visible(const Object& o) { return !o.priv || state().loggedIn; }

const EVP_MD* mdForHashMechanism(CK_MECHANISM_TYPE m) {
    switch (m) {
    case CKM_SHA256: return EVP_sha256();
    case CKM_SHA384: return EVP_sha384();
    case CKM_SHA512: return EVP_sha512();
    default: return nullptr;
    }
}

// ---- function implementations (every entry runs under the state mutex) -----

CK_RV mInitialize(void*) {
    std::lock_guard lock(state().mutex);
    if (is("init-fails")) return CKR_GENERAL_ERROR;
    if (is("already-initialized")) {
        state().initialized = true; // another component of the process initialised the module first
        return CKR_CRYPTOKI_ALREADY_INITIALIZED;
    }
    if (state().initialized) return CKR_CRYPTOKI_ALREADY_INITIALIZED;
    state().initialized = true;
    return CKR_OK;
}

CK_RV mFinalize(void*) {
    std::lock_guard lock(state().mutex);
    if (!state().initialized) return CKR_CRYPTOKI_NOT_INITIALIZED;
    state().initialized = false;
    state().sessions.clear();
    state().loggedIn = false;
    return CKR_OK;
}

CK_RV mGetSlotList(CK_BBOOL, CK_SLOT_ID* list, CK_ULONG* count) {
    std::lock_guard lock(state().mutex);
    if (!state().initialized) return CKR_CRYPTOKI_NOT_INITIALIZED;
    if (!count) return CKR_ARGUMENTS_BAD;
    const CK_ULONG have = is("no-slots") ? 0 : 1;
    if (!list) {
        *count = have;
        return CKR_OK;
    }
    if (*count < have) {
        *count = have;
        return CKR_BUFFER_TOO_SMALL;
    }
    if (have) list[0] = kSlot;
    *count = have;
    return CKR_OK;
}

CK_RV mGetSlotInfo(CK_SLOT_ID slot, CK_SLOT_INFO* info) {
    std::lock_guard lock(state().mutex);
    if (slot != kSlot || is("no-slots")) return CKR_SLOT_ID_INVALID;
    *info = CK_SLOT_INFO{};
    padCopy(info->slotDescription, sizeof info->slotDescription, "Mock reader 0");
    padCopy(info->manufacturerID, sizeof info->manufacturerID, "Rivet tests");
    info->flags = CKF_TOKEN_PRESENT;
    return CKR_OK;
}

CK_RV mGetTokenInfo(CK_SLOT_ID slot, CK_TOKEN_INFO* info) {
    std::lock_guard lock(state().mutex);
    if (slot != kSlot) return CKR_SLOT_ID_INVALID;
    if (is("no-slots")) return CKR_TOKEN_NOT_PRESENT;
    *info = CK_TOKEN_INFO{};
    padCopy(info->label, sizeof info->label, "Mock Token");
    padCopy(info->manufacturerID, sizeof info->manufacturerID, "Rivet tests");
    padCopy(info->model, sizeof info->model, "Mock");
    padCopy(info->serialNumber, sizeof info->serialNumber, "MOCK0001");
    info->flags = tokenFlags();
    return CKR_OK;
}

CK_RV mGetMechanismList(CK_SLOT_ID slot, CK_MECHANISM_TYPE* list, CK_ULONG* count) {
    std::lock_guard lock(state().mutex);
    if (slot != kSlot) return CKR_SLOT_ID_INVALID;
    if (is("no-mechlist")) return CKR_FUNCTION_NOT_SUPPORTED;
    std::vector<CK_MECHANISM_TYPE> all;
    if (is("raw-mechanisms")) {
        all = {CKM_RSA_PKCS, CKM_RSA_PKCS_PSS, CKM_ECDSA};
    } else {
        all = {CKM_RSA_PKCS,        CKM_SHA256_RSA_PKCS,     CKM_SHA384_RSA_PKCS,     CKM_SHA512_RSA_PKCS,
               CKM_RSA_PKCS_PSS,    CKM_SHA256_RSA_PKCS_PSS, CKM_SHA384_RSA_PKCS_PSS, CKM_SHA512_RSA_PKCS_PSS,
               CKM_ECDSA,           CKM_ECDSA_SHA256};
    }
    if (!list) {
        *count = all.size();
        return CKR_OK;
    }
    if (*count < all.size()) {
        *count = all.size();
        return CKR_BUFFER_TOO_SMALL;
    }
    std::copy(all.begin(), all.end(), list);
    *count = all.size();
    return CKR_OK;
}

CK_RV mOpenSession(CK_SLOT_ID slot, CK_FLAGS, void*, void*, CK_SESSION_HANDLE* out) {
    std::lock_guard lock(state().mutex);
    if (!state().initialized) return CKR_CRYPTOKI_NOT_INITIALIZED;
    if (slot != kSlot) return CKR_SLOT_ID_INVALID;
    if (is("no-slots")) return CKR_TOKEN_NOT_PRESENT;
    const CK_SESSION_HANDLE h = state().nextHandle++;
    state().sessions[h].slot = slot;
    *out = h;
    return CKR_OK;
}

CK_RV mCloseSession(CK_SESSION_HANDLE h) {
    std::lock_guard lock(state().mutex);
    if (state().sessions.erase(h) == 0) return CKR_SESSION_HANDLE_INVALID;
    if (state().sessions.empty()) state().loggedIn = false; // closing the last session logs out
    return CKR_OK;
}

CK_RV mGetSessionInfo(CK_SESSION_HANDLE h, CK_SESSION_INFO* info) {
    std::lock_guard lock(state().mutex);
    auto it = state().sessions.find(h);
    if (it == state().sessions.end()) return CKR_SESSION_HANDLE_INVALID;
    info->slotID = it->second.slot;
    info->state = state().loggedIn ? CKS_RO_USER_FUNCTIONS : CKS_RO_PUBLIC_SESSION;
    info->flags = CKF_SERIAL_SESSION;
    info->ulDeviceError = 0;
    return CKR_OK;
}

CK_RV mLogin(CK_SESSION_HANDLE h, CK_USER_TYPE type, CK_UTF8CHAR* pin, CK_ULONG len) {
    std::lock_guard lock(state().mutex);
    State& s = state();
    if (!s.sessions.contains(h)) return CKR_SESSION_HANDLE_INVALID;
    if (type != CKU_USER) return CKR_ARGUMENTS_BAD;
    ++s.loginCalls;
    if (is("locked")) return CKR_PIN_LOCKED;
    if (s.loggedIn) return CKR_USER_ALREADY_LOGGED_IN;
    if (!pin || len != std::strlen(kPin) || std::memcmp(pin, kPin, len) != 0) {
        ++s.failures; // never locks: the wrong-pin scenario must reach its third attempt
        return CKR_PIN_INCORRECT;
    }
    s.failures = 0;
    s.loggedIn = true;
    return CKR_OK;
}

CK_RV mGetAttributeValue(CK_SESSION_HANDLE h, CK_OBJECT_HANDLE object, CK_ATTRIBUTE* templ, CK_ULONG count) {
    std::lock_guard lock(state().mutex);
    if (!state().sessions.contains(h)) return CKR_SESSION_HANDLE_INVALID;
    const Object* o = findObject(object);
    if (!o || !visible(*o)) return CKR_KEY_HANDLE_INVALID; // stands in for CKR_OBJECT_HANDLE_INVALID
    CK_RV result = CKR_OK;
    for (CK_ULONG i = 0; i < count; ++i) {
        CK_ATTRIBUTE& a = templ[i];
        const void* data = nullptr;
        std::size_t size = 0;
        CK_ULONG ulongValue = 0;
        CK_BBOOL boolValue = 0;
        switch (a.type) {
        case CKA_CLASS: ulongValue = o->cls; data = &ulongValue; size = sizeof ulongValue; break;
        case CKA_CERTIFICATE_TYPE:
            if (o->cls == CKO_CERTIFICATE) { ulongValue = CKC_X_509; data = &ulongValue; size = sizeof ulongValue; }
            break;
        case CKA_KEY_TYPE:
            if (o->cls != CKO_CERTIFICATE) { ulongValue = o->keyType; data = &ulongValue; size = sizeof ulongValue; }
            break;
        case CKA_VALUE:
            if (o->cls == CKO_CERTIFICATE) { data = o->value.data(); size = o->value.size(); }
            break;
        case CKA_ID: data = o->id.data(); size = o->id.size(); break;
        case CKA_LABEL: data = o->label.data(); size = o->label.size(); break;
        case CKA_SIGN:
            if (o->cls == CKO_PRIVATE_KEY) { boolValue = 1; data = &boolValue; size = sizeof boolValue; }
            break;
        case CKA_PRIVATE: boolValue = o->priv ? 1 : 0; data = &boolValue; size = sizeof boolValue; break;
        default: break;
        }
        if (!data && !(a.type == CKA_ID || a.type == CKA_LABEL || a.type == CKA_VALUE)) {
            a.ulValueLen = CK_UNAVAILABLE_INFORMATION;
            result = CKR_ATTRIBUTE_TYPE_INVALID;
            continue;
        }
        if (!a.pValue) {
            a.ulValueLen = size;
        } else if (a.ulValueLen < size) {
            a.ulValueLen = CK_UNAVAILABLE_INFORMATION;
            result = CKR_BUFFER_TOO_SMALL;
        } else {
            if (size) std::memcpy(a.pValue, data, size);
            a.ulValueLen = size;
        }
    }
    return result;
}

CK_RV mFindObjectsInit(CK_SESSION_HANDLE h, CK_ATTRIBUTE* templ, CK_ULONG count) {
    std::lock_guard lock(state().mutex);
    auto it = state().sessions.find(h);
    if (it == state().sessions.end()) return CKR_SESSION_HANDLE_INVALID;
    Session& session = it->second;
    session.found.clear();
    session.position = 0;
    for (const Object& o : state().objects) {
        if (!visible(o)) continue;
        bool match = true;
        for (CK_ULONG i = 0; i < count && match; ++i) {
            const CK_ATTRIBUTE& a = templ[i];
            if (a.type == CKA_CLASS) {
                CK_ULONG want = 0;
                std::memcpy(&want, a.pValue, std::min<std::size_t>(sizeof want, a.ulValueLen));
                match = o.cls == want;
            } else if (a.type == CKA_ID) {
                match = a.ulValueLen == o.id.size() && std::memcmp(a.pValue, o.id.data(), o.id.size()) == 0;
            } else {
                match = false;
            }
        }
        if (match) session.found.push_back(o.handle);
    }
    return CKR_OK;
}

CK_RV mFindObjects(CK_SESSION_HANDLE h, CK_OBJECT_HANDLE* out, CK_ULONG max, CK_ULONG* count) {
    std::lock_guard lock(state().mutex);
    auto it = state().sessions.find(h);
    if (it == state().sessions.end()) return CKR_SESSION_HANDLE_INVALID;
    Session& session = it->second;
    CK_ULONG n = 0;
    while (n < max && session.position < session.found.size()) out[n++] = session.found[session.position++];
    *count = n;
    return CKR_OK;
}

CK_RV mFindObjectsFinal(CK_SESSION_HANDLE h) {
    std::lock_guard lock(state().mutex);
    auto it = state().sessions.find(h);
    if (it == state().sessions.end()) return CKR_SESSION_HANDLE_INVALID;
    it->second.found.clear();
    return CKR_OK;
}

CK_RV mSignInit(CK_SESSION_HANDLE h, CK_MECHANISM* mech, CK_OBJECT_HANDLE key) {
    std::lock_guard lock(state().mutex);
    auto it = state().sessions.find(h);
    if (it == state().sessions.end()) return CKR_SESSION_HANDLE_INVALID;
    const Object* o = findObject(key);
    if (!o || o->cls != CKO_PRIVATE_KEY) return CKR_KEY_HANDLE_INVALID;
    if (!visible(*o)) return CKR_USER_NOT_LOGGED_IN;
    Session& session = it->second;
    session.signActive = true;
    session.mechanism = mech->mechanism;
    session.key = key;
    session.hasPss = false;
    state().lastMechanism = mech->mechanism;
    const bool pssMechanism = mech->mechanism == CKM_RSA_PKCS_PSS || (mech->mechanism >= CKM_SHA256_RSA_PKCS_PSS && mech->mechanism <= CKM_SHA512_RSA_PKCS_PSS);
    if (pssMechanism) {
        if (!mech->pParameter || mech->ulParameterLen != sizeof(CK_RSA_PKCS_PSS_PARAMS)) return CKR_MECHANISM_PARAM_INVALID;
        std::memcpy(&session.pss, mech->pParameter, sizeof session.pss);
        session.hasPss = true;
        state().lastPssSaltLength = session.pss.sLen;
    }
    return CKR_OK;
}

// DER ECDSA-Sig-Value -> raw r||s with fixed field width.
std::vector<std::uint8_t> ecDerToRaw(const std::vector<std::uint8_t>& der, std::size_t width) {
    const unsigned char* p = der.data();
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size()));
    std::vector<std::uint8_t> raw(width * 2);
    if (sig) {
        BN_bn2binpad(ECDSA_SIG_get0_r(sig), raw.data(), static_cast<int>(width));
        BN_bn2binpad(ECDSA_SIG_get0_s(sig), raw.data() + width, static_cast<int>(width));
        ECDSA_SIG_free(sig);
    }
    return raw;
}

bool signWithKey(const Session& session, const Object& key, const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
    const CK_MECHANISM_TYPE m = session.mechanism;
    EVP_PKEY* pkey = key.key;
    auto digestSign = [&](const EVP_MD* md, bool pss) {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        EVP_PKEY_CTX* pctx = nullptr;
        if (EVP_DigestSignInit(ctx.get(), &pctx, md, nullptr, pkey) <= 0) return false;
        if (pss) {
            if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0) return false;
            if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, static_cast<int>(session.pss.sLen)) <= 0) return false;
            if (EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, md) <= 0) return false;
        }
        std::size_t len = 0;
        if (EVP_DigestSign(ctx.get(), nullptr, &len, data, size) <= 0) return false;
        out.assign(len, 0);
        if (EVP_DigestSign(ctx.get(), out.data(), &len, data, size) <= 0) return false;
        out.resize(len);
        return true;
    };
    auto rawSign = [&](int padding, const EVP_MD* md) {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new(pkey, nullptr), EVP_PKEY_CTX_free);
        if (!ctx || EVP_PKEY_sign_init(ctx.get()) <= 0) return false;
        if (padding != 0) {
            if (EVP_PKEY_CTX_set_rsa_padding(ctx.get(), padding) <= 0) return false;
            if (padding == RSA_PKCS1_PSS_PADDING) {
                if (EVP_PKEY_CTX_set_signature_md(ctx.get(), md) <= 0) return false;
                if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(), md) <= 0) return false;
                if (EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx.get(), static_cast<int>(session.pss.sLen)) <= 0) return false;
            }
        }
        std::size_t len = 0;
        if (EVP_PKEY_sign(ctx.get(), nullptr, &len, data, size) <= 0) return false;
        out.assign(len, 0);
        if (EVP_PKEY_sign(ctx.get(), out.data(), &len, data, size) <= 0) return false;
        out.resize(len);
        return true;
    };
    switch (m) {
    case CKM_RSA_PKCS: return rawSign(RSA_PKCS1_PADDING, nullptr);
    case CKM_SHA256_RSA_PKCS: return digestSign(EVP_sha256(), false);
    case CKM_SHA384_RSA_PKCS: return digestSign(EVP_sha384(), false);
    case CKM_SHA512_RSA_PKCS: return digestSign(EVP_sha512(), false);
    case CKM_SHA256_RSA_PKCS_PSS: return session.pss.hashAlg == CKM_SHA256 && digestSign(EVP_sha256(), true);
    case CKM_SHA384_RSA_PKCS_PSS: return session.pss.hashAlg == CKM_SHA384 && digestSign(EVP_sha384(), true);
    case CKM_SHA512_RSA_PKCS_PSS: return session.pss.hashAlg == CKM_SHA512 && digestSign(EVP_sha512(), true);
    case CKM_RSA_PKCS_PSS: {
        const EVP_MD* md = mdForHashMechanism(session.pss.hashAlg);
        return md && rawSign(RSA_PKCS1_PSS_PADDING, md);
    }
    case CKM_ECDSA: {
        if (!rawSign(0, nullptr)) return false;
        out = ecDerToRaw(out, 32);
        return true;
    }
    case CKM_ECDSA_SHA256: {
        if (!digestSign(EVP_sha256(), false)) return false;
        out = ecDerToRaw(out, 32);
        return true;
    }
    default: return false;
    }
}

CK_RV mSign(CK_SESSION_HANDLE h, CK_BYTE* data, CK_ULONG size, CK_BYTE* signature, CK_ULONG* signatureLength) {
    std::lock_guard lock(state().mutex);
    State& s = state();
    auto it = s.sessions.find(h);
    if (it == s.sessions.end()) return CKR_SESSION_HANDLE_INVALID;
    Session& session = it->second;
    if (!session.signActive) return CKR_FUNCTION_FAILED;
    ++s.signCalls;
    if (is("token-removed")) return CKR_DEVICE_REMOVED;
    if (is("session-expired-always") || (is("session-expired-once") && !s.expiredDone)) {
        s.expiredDone = true;
        s.sessions.erase(it); // the session is gone, and so is the login
        s.loggedIn = false;
        return CKR_SESSION_HANDLE_INVALID;
    }
    const Object* key = findObject(session.key);
    std::vector<std::uint8_t> out;
    if (!key || !signWithKey(session, *key, data, size, out)) return CKR_MECHANISM_INVALID;
    if (!signature) {
        *signatureLength = out.size();
        return CKR_OK;
    }
    if (*signatureLength < out.size()) {
        *signatureLength = out.size();
        return CKR_BUFFER_TOO_SMALL;
    }
    std::memcpy(signature, out.data(), out.size());
    *signatureLength = out.size();
    session.signActive = false;
    return CKR_OK;
}

CK_FUNCTION_LIST g_list{};

} // namespace

MOCK_EXPORT CK_RV C_GetFunctionList(CK_FUNCTION_LIST** list) {
    if (!list) return CKR_ARGUMENTS_BAD;
    std::lock_guard lock(state().mutex);
    g_list = CK_FUNCTION_LIST{};
    g_list.version = {2, 40};
    g_list.C_Initialize = mInitialize;
    g_list.C_Finalize = mFinalize;
    g_list.C_GetSlotList = mGetSlotList;
    g_list.C_GetSlotInfo = is("sparse") ? nullptr : mGetSlotInfo;
    g_list.C_GetTokenInfo = mGetTokenInfo;
    g_list.C_GetMechanismList = is("sparse") ? nullptr : mGetMechanismList;
    g_list.C_OpenSession = mOpenSession;
    g_list.C_CloseSession = mCloseSession;
    g_list.C_GetSessionInfo = is("sparse") ? nullptr : mGetSessionInfo;
    g_list.C_Login = mLogin;
    g_list.C_GetAttributeValue = mGetAttributeValue;
    g_list.C_FindObjectsInit = mFindObjectsInit;
    g_list.C_FindObjects = mFindObjects;
    g_list.C_FindObjectsFinal = mFindObjectsFinal;
    g_list.C_SignInit = mSignInit;
    g_list.C_Sign = mSign;
    *list = &g_list;
    return CKR_OK;
}

// ---- test-control exports ---------------------------------------------------

MOCK_EXPORT void rivet_mock_set_scenario(const char* name) {
    std::lock_guard lock(state().mutex);
    State& s = state();
    s.scenario = name ? name : "ok";
    s.loggedIn = false;
    s.failures = 0;
    s.expiredDone = false;
    s.loginCalls = 0;
    s.signCalls = 0;
    s.lastMechanism = 0;
    s.lastPssSaltLength = 0;
    s.sessions.clear();
    s.initialized = false; // a fresh scenario starts from an uninitialised module
    rebuildLocked();
}

MOCK_EXPORT unsigned rivet_mock_login_calls() { std::lock_guard lock(state().mutex); return state().loginCalls; }
MOCK_EXPORT unsigned rivet_mock_sign_calls() { std::lock_guard lock(state().mutex); return state().signCalls; }
MOCK_EXPORT unsigned rivet_mock_open_sessions() { std::lock_guard lock(state().mutex); return static_cast<unsigned>(state().sessions.size()); }
MOCK_EXPORT unsigned long rivet_mock_last_mechanism() { std::lock_guard lock(state().mutex); return state().lastMechanism; }
MOCK_EXPORT unsigned long rivet_mock_last_pss_salt_length() { std::lock_guard lock(state().mutex); return state().lastPssSaltLength; }
MOCK_EXPORT int rivet_mock_is_initialized() { std::lock_guard lock(state().mutex); return state().initialized ? 1 : 0; }
