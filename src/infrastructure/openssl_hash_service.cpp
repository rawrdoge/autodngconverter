/*
 * rawimport-pipeline - OpenSSL HashService adapter implementation
 */
#include "infrastructure/openssl_hash_service.hpp"

#include <openssl/evp.h>
#include <stdexcept>
#include <iomanip>
#include <sstream>

namespace rawimport::infrastructure {

OpenSSLHasher::OpenSSLHasher() : ctx_(EVP_MD_CTX_new()) {
    if (!ctx_) throw std::runtime_error("EVP_MD_CTX_new failed");
    if (EVP_DigestInit_ex(ctx_, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx_);
        throw std::runtime_error("EVP_DigestInit_ex failed");
    }
}

OpenSSLHasher::~OpenSSLHasher() {
    if (ctx_) EVP_MD_CTX_free(ctx_);
}

void OpenSSLHasher::update(std::span<const uint8_t> data) {
    if (ctx_ && !data.empty()) {
        EVP_DigestUpdate(ctx_, data.data(), data.size());
    }
}

domain::SHA256Hash OpenSSLHasher::finalize() {
    domain::SHA256Hash hash{};
    if (ctx_) {
        EVP_DigestFinal_ex(ctx_, hash.data(), nullptr);
        EVP_MD_CTX_free(ctx_);
        ctx_ = nullptr;
    }
    return hash;
}

domain::SHA256Hash OpenSSLHashService::sha256_bytes(std::span<const uint8_t> data) {
    domain::SHA256Hash hash{};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, hash.data(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_Digest operation failed");
    }
    EVP_MD_CTX_free(ctx);
    return hash;
}

std::unique_ptr<domain::Hasher> OpenSSLHashService::create_sha256_hasher() {
    return std::make_unique<OpenSSLHasher>();
}

uint64_t OpenSSLHashService::fnv1a_64(std::span<const uint8_t> data) {
    uint64_t h = 14695981039346656037ULL; // FNV-1a 64-bit offset basis
    for (auto byte : data) {
        h ^= static_cast<uint64_t>(byte);
        h *= 1099511628211ULL; // FNV-1a 64-bit prime
    }
    return h;
}

} // namespace rawimport::infrastructure
