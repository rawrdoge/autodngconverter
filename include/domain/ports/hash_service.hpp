/*
 * rawimport-pipeline - HashService port interface
 */
#pragma once

#include <cstdint>
#include <string>
#include <span>
#include <memory>

#include "domain/types.hpp"

namespace rawimport::domain {

// Incremental hasher interface for streaming large files without buffering.
class Hasher {
public:
    virtual ~Hasher() = default;
    virtual void update(std::span<const uint8_t> data) = 0;
    virtual SHA256Hash finalize() = 0;
};

class HashService {
public:
    virtual ~HashService() = default;

    // SHA-256 over a byte span (convenience for small data).
    virtual SHA256Hash sha256_bytes(std::span<const uint8_t> data) = 0;

    // Create an incremental hasher for streaming large files.
    virtual std::unique_ptr<Hasher> create_sha256_hasher() = 0;

    // FNV-1a 64-bit over a byte span.
    virtual uint64_t fnv1a_64(std::span<const uint8_t> data) = 0;
};

// Free utility for hex-encoding a SHA-256 hash (defined in domain/types.hpp).
std::string sha256_hex(const SHA256Hash& h);

} // namespace rawimport::domain
