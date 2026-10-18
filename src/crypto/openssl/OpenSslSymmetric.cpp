// SPDX-License-Identifier: MPL-2.0
#include "crypto/Symmetric.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>

#include <memory>

namespace rivet::crypto {

namespace {

struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

struct MdCtxDeleter {
    void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); }
};
using MdCtx = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

const EVP_CIPHER* cipherFor(AesMode mode, std::size_t keyBytes) {
    if (keyBytes == 16) return mode == AesMode::Cbc ? EVP_aes_128_cbc() : EVP_aes_128_ecb();
    if (keyBytes == 32) return mode == AesMode::Cbc ? EVP_aes_256_cbc() : EVP_aes_256_ecb();
    return nullptr;
}

CryptoResult<Bytes> run(bool encrypt, AesMode mode, ByteView key, ByteView iv, ByteView data, bool pkcs7) {
    const EVP_CIPHER* cipher = cipherFor(mode, key.size());
    if (!cipher) return fail(CryptoErrorCode::InvalidArgument, "AES key must be 16 or 32 bytes");
    if (mode == AesMode::Cbc && iv.size() != 16) return fail(CryptoErrorCode::InvalidArgument, "AES-CBC IV must be 16 bytes");
    if (!pkcs7 && data.size() % 16 != 0) return fail(CryptoErrorCode::InvalidArgument, "unpadded AES input must be a multiple of 16 bytes");
    if (data.size() > (std::size_t{1} << 30)) return fail(CryptoErrorCode::LimitExceeded, "AES input too large");

    CipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return fail(CryptoErrorCode::Internal, "cipher context allocation failed");
    const unsigned char* ivp = mode == AesMode::Cbc ? iv.data() : nullptr;
    if (EVP_CipherInit_ex(ctx.get(), cipher, nullptr, key.data(), ivp, encrypt ? 1 : 0) != 1) {
        ERR_clear_error();
        return fail(CryptoErrorCode::Internal, "cipher initialisation failed");
    }
    EVP_CIPHER_CTX_set_padding(ctx.get(), pkcs7 ? 1 : 0);

    Bytes out(data.size() + 32);
    int n1 = 0;
    if (!data.empty() &&
        EVP_CipherUpdate(ctx.get(), out.data(), &n1, data.data(), static_cast<int>(data.size())) != 1) {
        ERR_clear_error();
        return fail(CryptoErrorCode::Internal, "cipher update failed");
    }
    int n2 = 0;
    if (EVP_CipherFinal_ex(ctx.get(), out.data() + n1, &n2) != 1) {
        ERR_clear_error();
        return fail(encrypt ? CryptoErrorCode::Internal : CryptoErrorCode::CorruptData,
                    encrypt ? "cipher finalisation failed" : "AES data or padding is invalid");
    }
    out.resize(static_cast<std::size_t>(n1 + n2));
    return out;
}

} // namespace

CryptoResult<Bytes> aesEncrypt(AesMode mode, ByteView key, ByteView iv, ByteView data, bool pkcs7) {
    return run(true, mode, key, iv, data, pkcs7);
}

CryptoResult<Bytes> aesDecrypt(AesMode mode, ByteView key, ByteView iv, ByteView data, bool pkcs7) {
    return run(false, mode, key, iv, data, pkcs7);
}

CryptoResult<Bytes> md5(ByteView data) {
    MdCtx ctx(EVP_MD_CTX_new());
    if (!ctx) return fail(CryptoErrorCode::Internal, "digest context allocation failed");
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_DigestInit_ex(ctx.get(), EVP_md5(), nullptr) != 1 ||
        (!data.empty() && EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1) ||
        EVP_DigestFinal_ex(ctx.get(), md, &len) != 1) {
        ERR_clear_error();
        return fail(CryptoErrorCode::Internal, "MD5 failed");
    }
    return Bytes(md, md + len);
}

} // namespace rivet::crypto
