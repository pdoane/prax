// A header that C++ reads: a namespace, a template, and the C declarations that it exports.

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__cplusplus)
extern "C" {
#endif

// The statistics of a cache, as C callers read them.
typedef struct CacheStats {
    uint64_t hits;
    uint64_t misses;
} CacheStats;

CacheStats cacheStats(const void* cache);

#if defined(__cplusplus)
}
#endif

namespace cache {

// The share of lookups that hit, or zero before the first.
template <typename Stats>
constexpr double hitRate(const Stats& stats) noexcept {
    auto total = stats.hits + stats.misses;
    return total == 0 ? 0.0 : static_cast<double>(stats.hits) / total;
}

enum class Policy : std::uint8_t { LeastRecent, LeastFrequent };

} // namespace cache
