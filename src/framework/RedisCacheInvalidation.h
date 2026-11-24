/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

// RedisCacheInvalidation.h — Phase 4.2: Post-Commit Cache Invalidation Helper
//
// Provides a reusable, thread-safe, multi-instance-safe mechanism for registering
// Redis cache invalidation operations that execute ONLY after a successful PostgreSQL
// commit via DbTransaction::AfterCommit().
//
// Design guarantees:
//   - Strict pre-commit target validation: validates all cache keys and committed versions
//     BEFORE registering the callback. Invalid registrations fail immediately, mark the
//     DbTransaction as failed (preventing commit), and return false.
//   - Invalidation runs only when the PostgreSQL commit succeeds.
//   - Pending invalidation is silently discarded when the transaction rolls back or fails.
//   - Each target is invalidated independently; a Redis failure on one target does not
//     prevent invalidation of the remaining targets.
//   - Redis failures are logged at ERROR level and counted in RedisCache::InvalidationFailures().
//     They never surface as a failed database commit (Specification §6.3.1).
//   - Callback data is captured by value — no dangling references to transaction-local state.
//   - Multi-key: a single registration can cover all resources mutated by one transaction.

#include <string>
#include <vector>

#include "Poco/Logger.h"
#include "fmt/format.h"

#include "framework/DbTransaction.h"
#include "framework/RedisCache.h"

namespace OpenWifi {

/// One cache key and its newly committed version to invalidate after a PostgreSQL commit.
struct CacheInvalidationTarget {
    std::string cacheKey;         ///< Full cache key, e.g. MakeCacheKey("inventory", id).
    std::string committedVersion; ///< Decimal version string of the just-committed record.
};

/// Validates a single CacheInvalidationTarget.
/// Returns true if cacheKey is cacheable ("api-cache:<type>:<id>") and committedVersion
/// is a valid positive uint64 decimal string.
[[nodiscard]] inline bool ValidateCacheInvalidationTarget(const CacheInvalidationTarget &target) noexcept {
    return IsCacheableKey(target.cacheKey) && IsValidVersion(target.committedVersion);
}

/// Validates a list of CacheInvalidationTargets.
/// Returns true if targets is non-empty and all targets satisfy ValidateCacheInvalidationTarget.
[[nodiscard]] inline bool ValidateCacheInvalidationTargets(const std::vector<CacheInvalidationTarget> &targets) noexcept {
    if (targets.empty()) return false;
    for (const auto &t : targets) {
        if (!ValidateCacheInvalidationTarget(t)) return false;
    }
    return true;
}

/// Registers Redis cache invalidation for one or more affected cache keys so that
/// invalidation runs ONLY after a successful PostgreSQL commit.
///
/// Pre-commit validation:
///   Validates all invalidation targets before registering the callback.
///   If any target has an invalid cache key or version, or if targets is empty,
///   the registration is rejected immediately, tx is marked failed (refusing to commit),
///   and this function returns false.
///
/// Post-commit error handling:
///   If Redis is unreachable or fails during execution of the post-commit callback,
///   the error is logged at ERROR level and incremented in RedisCache::InvalidationFailures().
///   It does not throw and does not affect the already-committed PostgreSQL transaction.
///
/// Usage — inside a DbTransaction scope, before calling tx.Commit():
/// @code
///   if (!RegisterCacheInvalidationAfterCommit(tx, *Cache_,
///           { {MakeCacheKey("inventory", id), std::to_string(newVersion)} })) {
///       // Registration failed; tx is already marked failed and will refuse to commit.
///       return false;
///   }
///   if (!tx.Commit()) { ... handle error ... }
/// @endcode
///
/// Requirements:
///   - targets must be non-empty and all targets must pass ValidateCacheInvalidationTarget.
///   - cache and transaction must outlive the callback (guaranteed by DbTransaction's
///     synchronous callback execution model — callbacks run before Commit() returns).
///   - tx must have an active transaction; throws Poco::IllegalStateException if not.
[[nodiscard]] inline bool RegisterCacheInvalidationAfterCommit(
        DbTransaction &tx,
        RedisCache    &cache,
        std::vector<CacheInvalidationTarget> targets) {

    Poco::Logger &log = Poco::Logger::get("RedisCache");

    if (targets.empty()) {
        poco_warning(log, "RegisterCacheInvalidationAfterCommit: Rejected registration with empty targets list.");
        tx.MarkFailed("22023", "RegisterCacheInvalidationAfterCommit: targets list cannot be empty");
        return false;
    }

    for (const auto &t : targets) {
        if (!IsCacheableKey(t.cacheKey)) {
            poco_warning(log, fmt::format(
                "RegisterCacheInvalidationAfterCommit: Rejected registration with invalid cache key '{}'.",
                t.cacheKey));
            tx.MarkFailed("22023", "RegisterCacheInvalidationAfterCommit: invalid cache key '" + t.cacheKey + "'");
            return false;
        }
        if (!IsValidVersion(t.committedVersion)) {
            poco_warning(log, fmt::format(
                "RegisterCacheInvalidationAfterCommit: Rejected registration for key '{}': version '{}' must be a positive uint64 decimal.",
                t.cacheKey, t.committedVersion));
            tx.MarkFailed("22023", "RegisterCacheInvalidationAfterCommit: invalid committed version '" + t.committedVersion + "' for key '" + t.cacheKey + "'");
            return false;
        }
    }

    // Capture targets by value so the callback is self-contained with no dangling references.
    tx.AfterCommit([&cache, targets = std::move(targets)]() {
        for (const auto &t : targets) {
            // Invalidate() logs ERROR and increments InvalidationFailures() on genuine Redis failure.
            // It never throws and never affects the already-committed PostgreSQL transaction.
            (void)cache.Invalidate(t.cacheKey, t.committedVersion);
        }
    });

    return true;
}

} // namespace OpenWifi
