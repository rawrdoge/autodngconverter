/*
 * rawimport-pipeline - OpenSSL HashService adapter
 */
#pragma once

#include "domain/types.hpp"
#include "domain/ports/hash_service.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <memory>

// Forward-declare EVP_MAC / BIO etc. — we include openssl/evp.h in the .cpp.
struct evp_md_ctx_st;
struct evp_md_st;

namespace rawimport::infrastructure {

class OpenSSLHasher final : public domain::Hasher {
public:
    OpenSSLHasher();
    ~OpenSSLHasher() override;
    void update(std::span<const uint8_t> data) override;
    domain::SHA256Hash finalize() override;

private:
    evp_md_ctx_st* ctx_;
};

class OpenSSLHashService final : public domain::HashService {
public:
    domain::SHA256Hash sha256_bytes(std::span<const uint8_t> data) override;
    std::unique_ptr<domain::Hasher> create_sha256_hasher() override;
    uint64_t   fnv1a_64(std::span<const uint8_t> data) override;
};

} // namespace rawimport::infrastructure