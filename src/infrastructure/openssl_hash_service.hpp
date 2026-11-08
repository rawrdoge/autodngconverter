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

// Forward-declare EVP_MAC / BIO etc. — we include openssl/evp.h in the .cpp.
struct evp_md_ctx_st;
struct evp_md_st;

namespace rawimport::infrastructure {

class OpenSSLHashService final : public domain::HashService {
public:
    domain::SHA256Hash sha256_bytes(std::span<const uint8_t> data) override;
    uint64_t   fnv1a_64(std::span<const uint8_t> data) override;
};

} // namespace rawimport::infrastructure