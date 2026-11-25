/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Poco/Logger.h"

namespace OpenWifi {

/// Redis connection settings consumed by RedisCache.
struct RedisCacheConfig {
    bool        enabled             = false; ///< openwifi.redis.enable (false when host is empty)
    std::string host;                        ///< openwifi.redis.host
    int         port                = 6379;  ///< openwifi.redis.port
    std::string password;                    ///< openwifi.redis.password (never logged)
    int         db                  = 0;     ///< openwifi.redis.db
    int         timeoutMs           = 2000;  ///< openwifi.redis.timeout (ms)
    int         cacheTtlSec         = 60;    ///< openwifi.redis.cache.ttl (clamped to 10-60s)
    size_t      maxTotalConnections = 32;    ///< Maximum total (active + idle) connections in pool
    size_t      maxIdleConnections  = 16;    ///< Maximum idle connections kept in pool
};

/// Reads openwifi.redis.* properties to build RedisCacheConfig.
/// Validates settings with clear warning logs on invalid values. Never throws.
[[nodiscard]] RedisCacheConfig MakeRedisCacheConfig();

/// Validates that str is a strictly positive, decimal uint64 version string.
/// Rejects: empty strings, strings > 20 digits, signs ('+', '-'), whitespace,
/// non-digits, zero ("0", "00"), and values overflowing 64-bit unsigned integer.
[[nodiscard]] bool IsValidVersion(std::string_view str);

/// Checks whether Key conforms to the valid application cache namespace "api-cache:<type>:<id>".
[[nodiscard]] bool IsCacheableKey(std::string_view key);

/// Prometheus / telemetry metric for Redis cache invalidation failures (Spec §6.3.1).
/// Incremented whenever post-commit invalidation fails to contact Redis.
extern std::atomic<uint64_t> owprov_redis_invalidation_failures_total;

/// Accessor for the owprov_redis_invalidation_failures_total metric.
[[nodiscard]] inline uint64_t GetRedisInvalidationFailuresTotal() noexcept {
    return owprov_redis_invalidation_failures_total.load(std::memory_order_relaxed);
}

/// Build a deterministic cache key: "api-cache:<type>:<id>".
/// Returns empty string if arguments contain invalid characters or are empty.
[[nodiscard]] std::string MakeCacheKey(const std::string &ResourceType,
                                       const std::string &ResourceId);

/// Build a deterministic version-marker key: "api-ver:<type>:<id>".
/// Uses a separate namespace from api-cache: to prevent key collisions.
/// Returns empty string if arguments contain invalid characters or are empty.
[[nodiscard]] std::string MakeVersionKey(const std::string &ResourceType,
                                         const std::string &ResourceId);

/// Build a deterministic version-marker key from an existing cache key:
/// Converts "api-cache:<type>:<id>" -> "api-ver:<type>:<id>".
[[nodiscard]] std::string MakeVersionKey(const std::string &CacheKey);

/// Thread-safe, shared Redis cache-aside component for API data.
/// Uses a bounded connection pool with fallback for concurrent request handling,
/// enforces version-aware stale-repopulation protection via non-expiring monotonic version markers,
/// and 10-60s data TTL.
class RedisCache {
  public:
    /// Cached record holding serialized payload and a strictly positive uint64 decimal version string.
    struct Entry {
        std::string data;    ///< Serialized domain payload (typically JSON).
        std::string version; ///< Monotonically increasing record version (> 0 decimal string).
    };

    /// Result classification for cache operations.
    enum class Result {
        Hit,      ///< Value returned directly from Redis cache.
        Miss,     ///< Key absent in Redis; loaded from authoritative source and populated in Redis.
        Fallback, ///< Redis offline, in backoff, pool exhausted, or population skipped; loaded directly from DB.
        NotFound, ///< Key absent in Redis and authoritative loader reported no record.
        Error     ///< Authoritative loader threw an exception or reported failure.
    };

    /// Outcome returned by cache-aside operations.
    struct Outcome {
        Result               result = Result::NotFound;
        std::optional<Entry> entry;
        std::string          error;
    };

    using Loader = std::function<std::optional<Entry>()>;

    explicit RedisCache(RedisCacheConfig Config, Poco::Logger &Logger);
    ~RedisCache();

    RedisCache(const RedisCache &) = delete;
    RedisCache &operator=(const RedisCache &) = delete;
    RedisCache(RedisCache &&) = delete;
    RedisCache &operator=(RedisCache &&) = delete;

    /// Retrieve an entry using cache-aside pattern with authoritative loader fallback.
    /// Releases the Redis connection before executing loader() to avoid holding connections during DB I/O.
    Outcome Get(const std::string &Key, Loader loader);

    /// Direct cache read primitive. Returns cached entry on hit, or nullopt on miss/error.
    [[nodiscard]] std::optional<Entry> Get(const std::string &Key);

    /// Cache write primitive. Atomically populates entry in Redis with TTL (10-60s policy).
    /// Drops write if an equal or newer version exists in Redis or if version < committed version marker.
    /// Returns true on successful population.
    bool Set(const std::string &Key, const Entry &entry, int TtlSec = 0);

    /// Cache delete primitive. Deletes a key from Redis. Returns true on success or if key absent.
    bool Delete(const std::string &Key);

    /// Atomically invalidates a cache entry and records the committed version marker.
    /// Deletes the cached entry for Key if its version <= committedVersion, then stores the
    /// version marker permanently. The marker prevents future delayed readers/writers from
    /// repopulating Redis with stale data.
    ///
    /// Returns true on success or safe no-op (e.g. cached entry already newer than committedVersion).
    /// Returns false only on Redis failure. Never throws. Logs ERROR on failure (Spec §6.3.1).
    /// Thread-safe; safe to call from multiple OWPROV instances sharing the same Redis.
    bool Invalidate(const std::string &Key, const std::string &CommittedVersion);

    /// Returns the total number of Invalidate() calls that failed to contact Redis since startup.
    /// Exposes the owprov_redis_invalidation_failures_total metric.
    [[nodiscard]] uint64_t InvalidationFailures() const noexcept {
        return owprov_redis_invalidation_failures_total.load(std::memory_order_relaxed);
    }

    /// Check if Redis is currently healthy and not in reconnect backoff.
    [[nodiscard]] bool IsRedisHealthy() const noexcept;

    /// Return active configuration.
    [[nodiscard]] const RedisCacheConfig &Config() const noexcept;

  private:
    RedisCacheConfig      Config_;
    Poco::Logger         &Log_;
    std::atomic<bool>     RedisHealthy_{false};
    std::atomic<int64_t>  NextReconnectTimeMs_{0};
    /// Counts Invalidate() calls that failed to contact Redis (never decremented).
    /// Exposed via InvalidationFailures() for external metrics collection.
    std::atomic<uint64_t> InvalidationFailures_{0};

    struct Impl;
    std::unique_ptr<Impl> Impl_;

    void OnRedisFailure(const std::string &Operation, const std::string &Key, const std::string &Reason);
};

} // namespace OpenWifi
