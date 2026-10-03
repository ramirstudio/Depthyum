#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace depthyum {

template <class T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// FNV-1a style accumulator for cache keys.
struct Hasher {
    uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, size_t n) {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ULL; }
    }
    template <class T> void add(const T& v) { bytes(&v, sizeof(T)); }
};

} // namespace depthyum
