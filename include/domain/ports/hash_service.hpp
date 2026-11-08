/*
 * rawimport-pipeline - HashService port interface
 */
#pragma once

#include <cstdint>
#include <string>
#include <span>

#include "domain/types.hpp"

namespace rawimport::domain {

class HashService {
public:
    virtual ~HashService() = default;

    // SHA-256 over a byte span.
    virtual SHA256Hash sha256_bytes(std::span<const uint8_t> data) = 0;

    // FNV-1a 64-bit over a byte span.
    virtual uint64_t fnv1a_64(std::span<const uint8_t> data) = 0;
};

// Free utility for hex-encoding a SHA-256 hash (defined in domain/types.hpp).
std::string sha256_hex(const SHA256Hash& h);

} // namespace rawimport::domain
