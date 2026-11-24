/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include "Poco/Logger.h"
#include "Poco/Net/StreamSocket.h"
#include "Poco/Net/SocketAddress.h"
#include "fmt/format.h"

#ifdef USE_POCO_REDIS
#include "Poco/Redis/Array.h"
#include "Poco/Redis/Client.h"
#include "Poco/Redis/Command.h"
#include "Poco/Redis/Type.h"
#include "Poco/Timespan.h"
#endif

#include "Poco/Data/SQLite/Connector.h"
#include "Poco/Data/Session.h"
#include "framework/DbTransaction.h"
#include "framework/RedisCache.h"
#include "framework/RedisCacheInvalidation.h"

#define TEST_ASSERT(cond, msg)                                                                   \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::cerr << "TEST FAILURE [" << __FILE__ << ":" << __LINE__ << "]: " << msg        \
                      << std::endl;                                                              \
            std::exit(1);                                                                        \
        }                                                                                        \
    } while (0)

namespace OpenWifi {

// Global map backing test configuration lookups
static std::map<std::string, std::string> g_TestConfig;

std::string MicroServiceConfigGetString(const std::string &Key, const std::string &DefaultValue) {
    auto it = g_TestConfig.find(Key);
    return it != g_TestConfig.end() ? it->second : DefaultValue;
}

uint64_t MicroServiceConfigGetInt(const std::string &Key, uint64_t DefaultValue) {
    auto it = g_TestConfig.find(Key);
    if (it != g_TestConfig.end()) {
        try { return std::stoull(it->second); } catch (...) {}
    }
    return DefaultValue;
}

bool MicroServiceConfigGetBool(const std::string &Key, bool DefaultValue) {
    auto it = g_TestConfig.find(Key);
    if (it == g_TestConfig.end()) return DefaultValue;
    return it->second == "true" || it->second == "1";
}

} // namespace OpenWifi

using namespace OpenWifi;

// Detects available test Redis port (checks env TEST_REDIS_PORT, then 16379, then 6379)
static int DetectRedisPort() {
    if (const char *envPort = std::getenv("TEST_REDIS_PORT")) {
        try { return std::stoi(envPort); } catch (...) {}
    }
    for (int port : {16379, 6379}) {
        try {
            Poco::Net::StreamSocket sock;
            sock.connect(Poco::Net::SocketAddress("127.0.0.1", port), Poco::Timespan(100 * 1000));
            sock.close();
            return port;
        } catch (...) {}
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Suite 1: Configuration parsing and Key namespace validation (Offline - No Redis needed)
// ---------------------------------------------------------------------------
static void test_config_and_namespaces() {
    std::cout << "--- Suite 1: Configuration & Key Namespaces (Offline) ---" << std::endl;

    // 1. Config disabled
    g_TestConfig["openwifi.redis.enable"] = "false";
    g_TestConfig["openwifi.redis.host"] = "127.0.0.1";
    auto cfg1 = MakeRedisCacheConfig();
    TEST_ASSERT(!cfg1.enabled, "Config must be disabled when openwifi.redis.enable is false");

    // 2. Host empty
    g_TestConfig["openwifi.redis.enable"] = "true";
    g_TestConfig["openwifi.redis.host"] = "";
    auto cfg2 = MakeRedisCacheConfig();
    TEST_ASSERT(!cfg2.enabled, "Config must be disabled when host is empty");

    // 3. Out-of-bounds values defaulted safely
    g_TestConfig["openwifi.redis.enable"] = "true";
    g_TestConfig["openwifi.redis.host"] = "redis.local";
    g_TestConfig["openwifi.redis.port"] = "99999";   // > 65535 -> 6379
    g_TestConfig["openwifi.redis.timeout"] = "10";   // < 50 -> 2000
    g_TestConfig["openwifi.redis.db"] = "-5";        // < 0 -> 0
    g_TestConfig["openwifi.redis.cache.ttl"] = "500";// outside 10-60 -> 60
    auto cfg3 = MakeRedisCacheConfig();
    TEST_ASSERT(cfg3.enabled, "Config must be enabled");
    TEST_ASSERT(cfg3.host == "redis.local", "Host matches");
    TEST_ASSERT(cfg3.port == 6379, "Port defaulted to 6379");
    TEST_ASSERT(cfg3.timeoutMs == 2000, "Timeout defaulted to 2000");
    TEST_ASSERT(cfg3.db == 0, "DB defaulted to 0");
    TEST_ASSERT(cfg3.cacheTtlSec == 60, "TTL defaulted to 60");
    TEST_ASSERT(cfg3.maxTotalConnections == 32, "Default maxTotalConnections is 32");
    TEST_ASSERT(cfg3.maxIdleConnections == 16, "Default maxIdleConnections is 16");

    // 4. Custom values parsed
    g_TestConfig["openwifi.redis.port"] = "6380";
    g_TestConfig["openwifi.redis.timeout"] = "1500";
    g_TestConfig["openwifi.redis.db"] = "2";
    g_TestConfig["openwifi.redis.cache.ttl"] = "45";
    g_TestConfig["openwifi.redis.pool.maxtotal"] = "64";
    g_TestConfig["openwifi.redis.pool.maxidle"] = "24";
    auto cfg4 = MakeRedisCacheConfig();
    TEST_ASSERT(cfg4.port == 6380 && cfg4.timeoutMs == 1500 && cfg4.db == 2, "Custom settings match");
    TEST_ASSERT(cfg4.cacheTtlSec == 45, "Custom TTL matches");
    TEST_ASSERT(cfg4.maxTotalConnections == 64 && cfg4.maxIdleConnections == 24, "Custom pool limits match");

    // 5. Deterministic collision-free key generation
    std::string cacheKey = MakeCacheKey("inventory", "dev:1");
    std::string verKey   = MakeVersionKey("inventory", "dev:1");
    TEST_ASSERT(cacheKey == "api-cache:inventory:dev:1", "Cache key format matches namespace");
    TEST_ASSERT(verKey   == "api-ver:inventory:dev:1", "Version key format matches namespace");
    TEST_ASSERT(cacheKey != verKey, "Cache key and version key namespaces must never collide");
    TEST_ASSERT(MakeVersionKey(cacheKey) == verKey, "MakeVersionKey from cache key matches");

    // Invalid keys return empty string
    TEST_ASSERT(MakeCacheKey("", "123").empty(), "Empty resource type returns empty");
    TEST_ASSERT(MakeCacheKey("inv", "").empty(), "Empty resource id returns empty");

    std::cout << "  PASS: test_config_and_namespaces" << std::endl;
}

// ---------------------------------------------------------------------------
// Suite 2: Authoritative loader fallback & input validation (Offline - No Redis needed)
// ---------------------------------------------------------------------------
static void test_fallback_and_validation() {
    std::cout << "--- Suite 2: Fallback & Input Validation (Offline) ---" << std::endl;
    Poco::Logger &log = Poco::Logger::get("Test");

    // Point to unreachable port (nothing listening)
    RedisCacheConfig cfg;
    cfg.enabled = true;
    cfg.host = "127.0.0.1";
    cfg.port = 16378;
    cfg.timeoutMs = 50;

    RedisCache offlineCache(cfg, log);
    std::string key = MakeCacheKey("inventory", "offline_item");

    // 1. Redis offline: loader called, returns Result::Fallback with data
    int loaderCalls = 0;
    auto loaderSuccess = [&]() -> std::optional<RedisCache::Entry> {
        loaderCalls++;
        return RedisCache::Entry{"{\"data\":\"db_data\"}", "100"};
    };

    auto out1 = offlineCache.Get(key, loaderSuccess);
    TEST_ASSERT(out1.result == RedisCache::Result::Fallback, "Result must be Fallback when Redis offline");
    TEST_ASSERT(out1.entry.has_value() && out1.entry->data == "{\"data\":\"db_data\"}", "Loaded data returned");
    TEST_ASSERT(loaderCalls == 1, "Loader called on miss");

    // 2. Subsequent call in backoff window: bypasses Redis immediately, calls loader
    auto out2 = offlineCache.Get(key, loaderSuccess);
    TEST_ASSERT(out2.result == RedisCache::Result::Fallback, "Result is Fallback in backoff window");
    TEST_ASSERT(loaderCalls == 2, "Loader called again");

    // 3. Loader returns std::nullopt -> Result::NotFound
    auto loaderNotFound = []() -> std::optional<RedisCache::Entry> { return std::nullopt; };
    auto outNotFound = offlineCache.Get(key, loaderNotFound);
    TEST_ASSERT(outNotFound.result == RedisCache::Result::NotFound, "NotFound when DB has no record");

    // 4. Loader throws exception -> Result::Error
    auto loaderThrows = []() -> std::optional<RedisCache::Entry> { throw std::runtime_error("DB connection timeout"); };
    auto outError = offlineCache.Get(key, loaderThrows);
    TEST_ASSERT(outError.result == RedisCache::Result::Error, "Result::Error when loader throws");
    TEST_ASSERT(outError.error == "DB connection timeout", "Exception message preserved");

    // 5. Input validation: invalid versions rejected immediately
    TEST_ASSERT(!offlineCache.Set(key, {"data", ""}), "Empty version rejected");
    TEST_ASSERT(!offlineCache.Set(key, {"data", "0"}), "Zero version rejected");
    TEST_ASSERT(!offlineCache.Set(key, {"data", "abc"}), "Non-numeric version rejected");
    TEST_ASSERT(!offlineCache.Set(key, {"data", "-1"}), "Negative version rejected");
    TEST_ASSERT(!offlineCache.Set(key, {"data", "18446744073709551616"}), "uint64 overflow rejected");
    TEST_ASSERT(!offlineCache.SetVersionMarker(key, ""), "Empty version marker rejected");
    TEST_ASSERT(!offlineCache.SetVersionMarker(key, "0"), "Zero version marker rejected");

    // 6. Pre-commit target validation helpers
    TEST_ASSERT(ValidateCacheInvalidationTarget({key, "100"}), "Valid target must pass");
    TEST_ASSERT(!ValidateCacheInvalidationTarget({"bad_key", "100"}), "Invalid key prefix rejected");
    TEST_ASSERT(!ValidateCacheInvalidationTarget({key, "0"}), "Zero version rejected");
    TEST_ASSERT(!ValidateCacheInvalidationTarget({key, ""}), "Empty version rejected");
    TEST_ASSERT(!ValidateCacheInvalidationTargets({}), "Empty target list rejected");
    TEST_ASSERT(ValidateCacheInvalidationTargets({{key, "100"}, {MakeCacheKey("venue", "v1"), "200"}}), "Valid target list passes");

    // 7. Pre-commit rejection with DbTransaction
    Poco::Data::SQLite::Connector::registerConnector();
    Poco::Data::Session dbSession("SQLite", ":memory:");

    // 7a. Empty targets rejected before commit, tx marked failed
    {
        DbTransaction tx(dbSession, log);
        bool regOk = RegisterCacheInvalidationAfterCommit(tx, offlineCache, {});
        TEST_ASSERT(!regOk, "Registration with empty targets must return false");
        TEST_ASSERT(tx.HasFailed(), "Transaction must be marked failed on invalid registration");
        TEST_ASSERT(!tx.Commit(), "Transaction cannot commit after failed registration");
    }

    // 7b. Invalid cache key rejected before commit, tx marked failed
    {
        DbTransaction tx(dbSession, log);
        bool regOk = RegisterCacheInvalidationAfterCommit(tx, offlineCache, {{"bad-key", "100"}});
        TEST_ASSERT(!regOk, "Registration with invalid cache key must return false");
        TEST_ASSERT(tx.HasFailed(), "Transaction must be marked failed on invalid key");
        TEST_ASSERT(!tx.Commit(), "Transaction cannot commit after invalid key registration");
    }

    // 7c. Invalid committed version rejected before commit, tx marked failed
    {
        DbTransaction tx(dbSession, log);
        bool regOk = RegisterCacheInvalidationAfterCommit(tx, offlineCache, {{key, "0"}});
        TEST_ASSERT(!regOk, "Registration with invalid version '0' must return false");
        TEST_ASSERT(tx.HasFailed(), "Transaction must be marked failed on invalid version");
        TEST_ASSERT(!tx.Commit(), "Transaction cannot commit after invalid version registration");
    }

    // 7d. Valid targets succeed and permit clean commit
    {
        DbTransaction tx(dbSession, log);
        bool regOk = RegisterCacheInvalidationAfterCommit(tx, offlineCache, {{key, "100"}});
        TEST_ASSERT(regOk, "Registration with valid targets must return true");
        TEST_ASSERT(!tx.HasFailed(), "Transaction must not be marked failed on valid registration");
        TEST_ASSERT(tx.Commit(), "Transaction commits successfully");
    }

    std::cout << "  PASS: test_fallback_and_validation" << std::endl;
}

// ---------------------------------------------------------------------------
// Suite 3: Consolidated Live Redis Integration (Executed ONLY if Redis is detected)
// ---------------------------------------------------------------------------
static void test_live_redis_integration(int redisPort) {
    std::cout << "--- Suite 3: Consolidated Live Redis Integration (Port " << redisPort << ") ---" << std::endl;
#ifdef USE_POCO_REDIS
    Poco::Logger &log = Poco::Logger::get("Test");

    RedisCacheConfig cfg;
    cfg.enabled = true;
    cfg.host = "127.0.0.1";
    cfg.port = redisPort;
    cfg.cacheTtlSec = 30;

    RedisCache cache(cfg, log);
    std::string key = MakeCacheKey("inventory", "live_item_test");
    std::string verKey = MakeVersionKey(key);

    // Helper to clean key and marker in Redis
    try {
        Poco::Redis::Client cleanupClient("127.0.0.1", redisPort);
        Poco::Redis::Array delCmd;
        delCmd << "DEL" << key << verKey;
        cleanupClient.execute<Poco::Int64>(delCmd);
    } catch (...) {}

    // 1. Cache Miss -> Loader -> Cache Hit
    std::atomic<int> loaderCount = 0;
    auto loader = [&]() -> std::optional<RedisCache::Entry> {
        loaderCount++;
        return RedisCache::Entry{"{\"name\":\"dev1\"}", "200"};
    };

    auto outMiss = cache.Get(key, loader);
    TEST_ASSERT(outMiss.result == RedisCache::Result::Miss, "Initial read must be Miss");
    TEST_ASSERT(outMiss.entry.has_value() && outMiss.entry->version == "200", "Loaded entry matches");
    TEST_ASSERT(loaderCount == 1, "Loader invoked once");

    auto outHit = cache.Get(key, loader);
    TEST_ASSERT(outHit.result == RedisCache::Result::Hit, "Subsequent read must be Hit");
    TEST_ASSERT(outHit.entry.has_value() && outHit.entry->version == "200", "Cached entry matches");
    TEST_ASSERT(loaderCount == 1, "Loader NOT invoked on Hit");

    // 2. Non-Expiring Monotonic Version Marker
    // Set version marker V300 (e.g. on DB update/invalidation)
    bool mSet = cache.SetVersionMarker(key, "300");
    TEST_ASSERT(mSet, "SetVersionMarker must succeed");

    Poco::Redis::Client rClient("127.0.0.1", redisPort);
    Poco::Redis::Array ttlCmd;
    ttlCmd << "TTL" << verKey;
    auto vmTtl = rClient.execute<Poco::Int64>(ttlCmd);
    TEST_ASSERT(vmTtl == -1, "Committed version marker must be non-expiring (TTL == -1)");

    // Monotonicity: setting older marker V250 must NOT roll back V300
    cache.SetVersionMarker(key, "250");
    Poco::Redis::Array getVerCmd;
    getVerCmd << "GET" << verKey;
    auto curMarker = rClient.execute<Poco::Redis::BulkString>(getVerCmd);
    TEST_ASSERT(!curMarker.isNull() && curMarker.value() == "300", "Marker must remain at highest version (300)");

    // 3. Stale Repopulation Protection:
    // Stale write V100 or V200 against marker V300 must be rejected
    bool staleSet = cache.Set(key, {"{\"name\":\"stale\"}", "200"});
    TEST_ASSERT(!staleSet, "Stale write (< 300) must be rejected");

    // Valid write V300 (== marker) or V400 (> marker) must succeed
    bool validSet = cache.Set(key, {"{\"name\":\"updated\"}", "300"});
    TEST_ASSERT(validSet, "Write equal to marker (300) must succeed");

    // 4. Invalidation / Delete
    bool delOk = cache.Delete(key);
    TEST_ASSERT(delOk, "Delete must succeed");
    auto afterDel = cache.Get(key);
    TEST_ASSERT(!afterDel.has_value(), "Key must not be cached after Delete");

    // Cleanup
    try {
        Poco::Redis::Array delCmd;
        delCmd << "DEL" << key << verKey;
        rClient.execute<Poco::Int64>(delCmd);
    } catch (...) {}

    std::cout << "  PASS: test_live_redis_integration" << std::endl;
#else
    (void)redisPort;
#endif
}

// ---------------------------------------------------------------------------
// Suite 4: Phase 4.2 — Atomic Invalidation (Executed ONLY if Redis is detected)
// ---------------------------------------------------------------------------
static void test_atomic_invalidation(int redisPort) {
    std::cout << "--- Suite 4: Phase 4.2 Atomic Invalidation (Port " << redisPort << ") ---" << std::endl;
#ifdef USE_POCO_REDIS
    Poco::Logger &log = Poco::Logger::get("Test");

    RedisCacheConfig cfg;
    cfg.enabled  = true;
    cfg.host     = "127.0.0.1";
    cfg.port     = redisPort;
    cfg.cacheTtlSec = 30;

    RedisCache cache(cfg, log);

    // Helper keys
    const std::string key    = MakeCacheKey("inventory", "inv_invalidate_test");
    const std::string verKey = MakeVersionKey(key);

    // Helper: direct Redis client for state inspection
    Poco::Redis::Client rc("127.0.0.1", redisPort);

    auto cleanup = [&]() {
        try {
            Poco::Redis::Array cmd; cmd << "DEL" << key << verKey;
            rc.execute<Poco::Int64>(cmd);
        } catch (...) {}
    };

    auto getMarker = [&]() -> std::string {
        try {
            Poco::Redis::Array cmd; cmd << "GET" << verKey;
            auto r = rc.execute<Poco::Redis::BulkString>(cmd);
            return r.isNull() ? "" : r.value();
        } catch (...) { return ""; }
    };

    // ---- Test A: Invalidate deletes cached entry and records marker ----
    cleanup();
    cache.Set(key, {"{\"x\":1}", "10"}, 30);        // populate V10
    bool ok = cache.Invalidate(key, "20");           // committed V20
    TEST_ASSERT(ok, "A: Invalidate must succeed");
    // Entry must be deleted (V10 <= V20)
    auto afterInv = cache.Get(key);
    TEST_ASSERT(!afterInv.has_value(), "A: Cached entry must be deleted after invalidation");
    // Marker must record V20 permanently
    TEST_ASSERT(getMarker() == "20", "A: Version marker must be set to committed version");
    // Marker must be non-expiring
    {
        Poco::Redis::Array ttlCmd; ttlCmd << "TTL" << verKey;
        auto ttl = rc.execute<Poco::Int64>(ttlCmd);
        TEST_ASSERT(ttl == -1, "A: Version marker must be non-expiring (TTL == -1)");
    }

    // ---- Test B: Cached entry newer than committed → preserve entry ----
    cleanup();
    cache.Set(key, {"{\"x\":3}", "30"}, 30);        // populate V30 (newer)
    ok = cache.Invalidate(key, "20");               // delayed invalidation V20
    TEST_ASSERT(ok, "B: Stale invalidation must still succeed (safe no-op)");
    // Entry V30 must be preserved because it is newer than committed V20
    auto preserved = cache.Get(key);
    TEST_ASSERT(preserved.has_value() && preserved->version == "30",
                "B: Newer cached entry (V30) must be preserved on delayed invalidation (V20)");

    // ---- Test C: Marker monotonicity — older invalidation never regresses marker ----
    cleanup();
    // Establish marker V50 first
    cache.SetVersionMarker(key, "50");
    TEST_ASSERT(getMarker() == "50", "C: Pre-condition: marker set to V50");
    // Delayed invalidation V30 (older than marker V50)
    ok = cache.Invalidate(key, "30");
    TEST_ASSERT(ok, "C: Older invalidation must succeed as safe no-op");
    TEST_ASSERT(getMarker() == "50", "C: Marker must remain at V50 after older invalidation V30");

    // ---- Test C2: Stale-cache handling when existing marker is newer ----
    // Existing marker: V30, Existing cached record: V20, Delayed invalidation: V10
    // Cached record V20 must be DELETED because V20 <= highest known committed (V30).
    cleanup();
    cache.SetVersionMarker(key, "30");
    {
        Poco::Redis::Array setRaw;
        setRaw << "SET" << key << "{\"fmt\":\"v1\",\"data\":\"{\\\"name\\\":\\\"v20\\\"}\",\"version\":\"20\"}";
        rc.execute<std::string>(setRaw);
    }
    TEST_ASSERT(cache.Get(key).has_value(), "C2: Pre-condition: cached record V20 exists");
    TEST_ASSERT(getMarker() == "30", "C2: Pre-condition: marker is V30");
    ok = cache.Invalidate(key, "10"); // Delayed invalidation request V10 < marker V30
    TEST_ASSERT(ok, "C2: Delayed invalidation must succeed");
    auto afterC2 = cache.Get(key);
    TEST_ASSERT(!afterC2.has_value(),
                "C2: Cached record V20 must be DELETED because marker establishes V30 was already committed");
    TEST_ASSERT(getMarker() == "30", "C2: Marker must remain at V30");

    // Genuinely newer cached record (V40 > marker V30) must be PRESERVED
    {
        Poco::Redis::Array setRaw40;
        setRaw40 << "SET" << key << "{\"fmt\":\"v1\",\"data\":\"{\\\"name\\\":\\\"v40\\\"}\",\"version\":\"40\"}";
        rc.execute<std::string>(setRaw40);
    }
    ok = cache.Invalidate(key, "10"); // Delayed invalidation request V10 < marker V30
    TEST_ASSERT(ok, "C2: Invalidation must succeed as safe no-op for V40");
    auto afterC2Newer = cache.Get(key);
    TEST_ASSERT(afterC2Newer.has_value() && afterC2Newer->version == "40",
                "C2: Genuinely newer record (V40 > marker V30) must be PRESERVED");

    // Review comment 1: Delayed invalidation must NOT delete cached entry equal to marker
    // Sequence:
    // 1. OWPROV-1 commits V2, invalidation delayed
    // 2. OWPROV-2 commits V3, records marker V3
    // 3. Reader loads V3 and caches it
    // 4. Delayed invalidation V2 executes: must PRESERVE cached V3!
    cleanup();
    cache.SetVersionMarker(key, "3");
    {
        Poco::Redis::Array setRaw3;
        setRaw3 << "SET" << key << "{\"fmt\":\"v1\",\"data\":\"{\\\"name\\\":\\\"v3\\\"}\",\"version\":\"3\"}";
        rc.execute<std::string>(setRaw3);
    }
    TEST_ASSERT(cache.Get(key).has_value(), "C2: Pre-condition: cached record V3 exists");
    TEST_ASSERT(getMarker() == "3", "C2: Pre-condition: marker is V3");
    ok = cache.Invalidate(key, "2"); // Delayed V2 invalidation against marker V3 and cached V3
    TEST_ASSERT(ok, "C2: Delayed V2 invalidation must succeed");
    auto afterV3 = cache.Get(key);
    TEST_ASSERT(afterV3.has_value() && afterV3->version == "3",
                "C2: Cached entry V3 (equal to marker V3) must be PRESERVED on delayed V2 invalidation");
    TEST_ASSERT(getMarker() == "3", "C2: Marker must remain at V3");

    // ---- Test D: Idempotent repeated invalidation ----
    cleanup();
    cache.Set(key, {"{\"x\":1}", "10"}, 30);
    cache.Invalidate(key, "20");
    // Second call with same committed version: idempotent
    ok = cache.Invalidate(key, "20");
    TEST_ASSERT(ok, "D: Repeated identical invalidation must succeed");
    TEST_ASSERT(getMarker() == "20", "D: Marker stays at V20 after repeated call");

    // ---- Test E: Absent entry — invalidation records marker without deleting ----
    cleanup();
    // No cached entry; invalidating should still set the marker
    ok = cache.Invalidate(key, "55");
    TEST_ASSERT(ok, "E: Invalidation with absent entry must succeed");
    TEST_ASSERT(getMarker() == "55", "E: Marker must be set even when no cached entry exists");

    // ---- Test F: Invalidation rejects invalid committed version ----
    cleanup();
    uint64_t failsBefore = cache.InvalidationFailures();
    bool badOk = cache.Invalidate(key, "0");
    TEST_ASSERT(!badOk, "F: Version '0' must be rejected");
    bool emptyOk = cache.Invalidate(key, "");
    TEST_ASSERT(!emptyOk, "F: Empty version must be rejected");
    // Validation failures must NOT increment the redis-failure counter
    // (they are caught before the Redis call and logged as warnings, not errors)
    TEST_ASSERT(cache.InvalidationFailures() == failsBefore,
                "F: Validation failures must not increment InvalidationFailures counter");

    // ---- Test G: Post-commit registration end-to-end with live Redis ----
    {
        cleanup();
        cache.Set(key, {"{\"name\":\"dev1\"}", "10"}, 30);
        TEST_ASSERT(cache.Get(key).has_value(), "G: Pre-condition: key is cached at V10");

        Poco::Data::SQLite::Connector::registerConnector();
        Poco::Data::Session dbSession("SQLite", ":memory:");
        DbTransaction tx(dbSession, log);

        bool regOk = RegisterCacheInvalidationAfterCommit(tx, cache, {{key, "20"}});
        TEST_ASSERT(regOk, "G: Valid registration must return true");
        TEST_ASSERT(tx.Commit(), "G: Database transaction commit must succeed");

        // Post-commit callback must have deleted cached V10 and set marker to V20
        auto afterCommit = cache.Get(key);
        TEST_ASSERT(!afterCommit.has_value(), "G: Cached entry must be deleted after DB commit");
        TEST_ASSERT(getMarker() == "20", "G: Version marker must be set to V20 after DB commit");
    }

    // ---- Test H: InvalidationFailures counter increments on Redis offline ----
    {
        RedisCacheConfig offCfg;
        offCfg.enabled   = true;
        offCfg.host      = "127.0.0.1";
        offCfg.port      = 16378; // nothing listening
        offCfg.timeoutMs = 50;
        RedisCache offCache(offCfg, log);

        const std::string offKey = MakeCacheKey("inventory", "offline_inv");
        uint64_t before = offCache.InvalidationFailures();
        uint64_t beforeMetric = GetRedisInvalidationFailuresTotal();
        // Invalidate() with Redis offline must return false, log reason with ERROR, and increment counters.
        bool offOk = offCache.Invalidate(offKey, "1");
        TEST_ASSERT(!offOk, "H: Invalidate with Redis offline must return false");
        TEST_ASSERT(offCache.InvalidationFailures() == before + 1,
                    "H: InvalidationFailures counter must increment on Redis failure");
        TEST_ASSERT(GetRedisInvalidationFailuresTotal() == beforeMetric + 1,
                    "H: owprov_redis_invalidation_failures_total metric must increment on failure");
        TEST_ASSERT(owprov_redis_invalidation_failures_total.load() == beforeMetric + 1,
                    "H: atomic owprov_redis_invalidation_failures_total must increment on failure");
    }

    // ---- Test I: Version markers are strictly non-expiring (TTL == -1) ----
    {
        cleanup();
        // 1. Invalidation records non-expiring marker
        cache.Invalidate(key, "100");
        Poco::Redis::Array ttlCmd1; ttlCmd1 << "TTL" << verKey;
        auto ttl1 = rc.execute<Poco::Int64>(ttlCmd1);
        TEST_ASSERT(ttl1 == -1, "I: Invalidation marker must be non-expiring (TTL == -1)");
        TEST_ASSERT(getMarker() == "100", "I: Invalidation marker must record V100");

        // 2. Direct SetVersionMarker records non-expiring marker
        cache.SetVersionMarker(key, "200");
        Poco::Redis::Array ttlCmd2; ttlCmd2 << "TTL" << verKey;
        auto ttl2 = rc.execute<Poco::Int64>(ttlCmd2);
        TEST_ASSERT(ttl2 == -1, "I: SetVersionMarker must be non-expiring (TTL == -1)");
        TEST_ASSERT(getMarker() == "200", "I: SetVersionMarker must record V200");
    }

    cleanup();
    std::cout << "  PASS: test_atomic_invalidation" << std::endl;
#else
    (void)redisPort;
    std::cout << "  SKIPPED: USE_POCO_REDIS not defined" << std::endl;
#endif
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    std::cout << "=== Running RedisCache Framework Tests ===" << std::endl;

    // Suites 1 and 2 run in all environments (including GitHub Actions CI without Redis)
    test_config_and_namespaces();
    test_fallback_and_validation();

    // Suite 3 runs if a test Redis instance is running locally, otherwise cleanly skips
    int redisPort = DetectRedisPort();
    if (redisPort == 0) {
        std::cout << "Notice: No Redis server detected on port 16379 or 6379." << std::endl;
        std::cout << "Live Redis integration tests skipped (normal in GitHub Actions CI)." << std::endl;
    } else {
        std::cout << "Live Redis server detected on 127.0.0.1:" << redisPort << std::endl;
        test_live_redis_integration(redisPort);
        test_atomic_invalidation(redisPort);
    }

    std::cout << "=== All RedisCache Tests PASSED! ===" << std::endl;
    return 0;
}
