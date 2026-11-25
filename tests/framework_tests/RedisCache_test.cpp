/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

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

static std::string GetTestRedisHost() {
    if (const char *envHost = std::getenv("TEST_REDIS_HOST")) {
        if (*envHost) return envHost;
    }
    return "127.0.0.1";
}

static bool IsStrictRedisMode() {
    if (const char *envStrict = std::getenv("REQUIRE_REDIS_TESTS")) {
        std::string s(envStrict);
        return (s == "1" || s == "true" || s == "TRUE" || s == "yes");
    }
    return false;
}

static bool PingRedis(const std::string &host, int port) {
#ifdef USE_POCO_REDIS
    try {
        Poco::Redis::Client client(host, port);
        Poco::Redis::Array pingCmd;
        pingCmd << "PING";
        auto res = client.execute<std::string>(pingCmd);
        return res == "PONG";
    } catch (...) {
        return false;
    }
#else
    (void)host; (void)port;
    return false;
#endif
}

// Detects available test Redis port using live Redis PING commands.
// In strict mode (REQUIRE_REDIS_TESTS=1), any failure to ping a live Redis server,
// invalid port configuration, or binary built without USE_POCO_REDIS causes immediate process failure.
static int DetectRedisPort() {
    const std::string host = GetTestRedisHost();
    const bool strict = IsStrictRedisMode();

#ifndef USE_POCO_REDIS
    if (strict) {
        std::cerr << "FATAL: REQUIRE_REDIS_TESTS=1 is set, but test binary was compiled without USE_POCO_REDIS!"
                  << std::endl;
        std::exit(1);
    }
    return 0;
#endif

    // If explicit port provided, verify it directly via Redis PING
    if (const char *envPort = std::getenv("TEST_REDIS_PORT")) {
        try {
            int port = std::stoi(envPort);
            if (port > 0 && PingRedis(host, port)) {
                return port;
            }
            if (strict) {
                std::cerr << "FATAL: REQUIRE_REDIS_TESTS=1 is set and TEST_REDIS_PORT=" << port
                          << ", but Redis PING failed on " << host << ":" << port << std::endl;
                std::exit(1);
            }
        } catch (...) {
            if (strict) {
                std::cerr << "FATAL: REQUIRE_REDIS_TESTS=1 is set but TEST_REDIS_PORT is invalid: " << envPort << std::endl;
                std::exit(1);
            }
        }
    }

    // Auto-probe standard local test ports (16379, 6379) using Redis PING
    for (int port : {16379, 6379}) {
        if (PingRedis(host, port)) {
            return port;
        }
    }

    if (strict) {
        std::cerr << "FATAL: REQUIRE_REDIS_TESTS=1 is set, but no Redis server responded to PING on "
                  << host << " (ports tested: 16379, 6379)" << std::endl;
        std::exit(1);
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

    // 5. Deterministic collision-free key generation & canonicalization
    std::string cacheKey = MakeCacheKey("inventory", "dev:1");
    std::string verKey   = MakeVersionKey("inventory", "dev:1");
    TEST_ASSERT(cacheKey == "api-cache:inventory:dev:1", "Cache key format matches namespace");
    TEST_ASSERT(verKey   == "api-ver:inventory:dev:1", "Version key format matches namespace");
    TEST_ASSERT(cacheKey != verKey, "Cache key and version key namespaces must never collide");
    TEST_ASSERT(MakeVersionKey(cacheKey) == verKey, "MakeVersionKey from cache key matches");

    // Canonicalization of mixed-case resource types via helpers
    TEST_ASSERT(MakeCacheKey("Inventory", "dev:1") == cacheKey, "MakeCacheKey canonicalizes mixed-case resource type");
    TEST_ASSERT(MakeVersionKey("Inventory", "dev:1") == verKey, "MakeVersionKey canonicalizes mixed-case resource type");

    // IsCacheableKey accepts canonical lowercase and rejects mixed/upper case
    TEST_ASSERT(IsCacheableKey("api-cache:inventory:dev:1"), "Canonical lowercase key is cacheable");
    TEST_ASSERT(!IsCacheableKey("api-cache:Inventory:dev:1"), "Mixed-case resource type rejected by IsCacheableKey");
    TEST_ASSERT(!IsCacheableKey("api-cache:INVENTORY:dev:1"), "All-caps resource type rejected by IsCacheableKey");
    TEST_ASSERT(IsCacheableKey("api-cache:dev_type-1.0:id"), "Alphanumeric, underscore, hyphen, dot permitted in resource type");
    TEST_ASSERT(!IsCacheableKey("api-cache:inv entory:123"), "Space rejected in resource type");
    TEST_ASSERT(!IsCacheableKey("api-cache::123"), "Empty resource type rejected");
    TEST_ASSERT(!IsCacheableKey("api-cache:inventory:"), "Empty resource id rejected");
    TEST_ASSERT(!IsCacheableKey("api-ver:inventory:dev:1"), "Non-cacheable prefix rejected");
    TEST_ASSERT(!IsCacheableKey("random_string"), "Malformed string rejected");

    // Full-key MakeVersionKey() input validation
    TEST_ASSERT(MakeVersionKey("api-cache:inventory:dev:1") == "api-ver:inventory:dev:1", "Valid full key converted");
    TEST_ASSERT(MakeVersionKey("api-cache:Inventory:dev:1").empty(), "Mixed-case full key rejected by MakeVersionKey");
    TEST_ASSERT(MakeVersionKey("api-cache:INVENTORY:dev:1").empty(), "All-caps full key rejected by MakeVersionKey");
    TEST_ASSERT(MakeVersionKey("api-cache:inventory:").empty(), "Empty resource id rejected by MakeVersionKey");
    TEST_ASSERT(MakeVersionKey("api-cache:").empty(), "Incomplete key rejected by MakeVersionKey");
    TEST_ASSERT(MakeVersionKey("api-ver:inventory:dev:1").empty(), "Version key rejected by MakeVersionKey");
    TEST_ASSERT(MakeVersionKey("not_a_cache_key").empty(), "Malformed key rejected by MakeVersionKey");

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
    TEST_ASSERT(!offlineCache.Invalidate(key, ""), "Empty version invalidation rejected");
    TEST_ASSERT(!offlineCache.Invalidate(key, "0"), "Zero version invalidation rejected");

    // 5b. Key validation: raw mixed-case keys rejected at boundary
    TEST_ASSERT(!offlineCache.Set("api-cache:Inventory:offline_item", {"data", "100"}), "Mixed-case Set() rejected");
    TEST_ASSERT(!offlineCache.Invalidate("api-cache:Inventory:offline_item", "100"), "Mixed-case Invalidate() rejected");
    TEST_ASSERT(!offlineCache.Delete("api-cache:Inventory:offline_item"), "Mixed-case Delete() rejected");
    TEST_ASSERT(!offlineCache.Get("api-cache:Inventory:offline_item").has_value(), "Mixed-case Get() returns nullopt");
    TEST_ASSERT(!ValidateCacheInvalidationTarget({"api-cache:Inventory:offline_item", "100"}), "Mixed-case target rejected");

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

    // 7e. Mixed-case cache key rejected before commit, tx marked failed
    {
        DbTransaction tx(dbSession, log);
        bool regOk = RegisterCacheInvalidationAfterCommit(tx, offlineCache, {{"api-cache:Inventory:offline_item", "100"}});
        TEST_ASSERT(!regOk, "Registration with mixed-case cache key must return false");
        TEST_ASSERT(tx.HasFailed(), "Transaction must be marked failed on mixed-case key");
        TEST_ASSERT(!tx.Commit(), "Transaction cannot commit after mixed-case key registration");
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
    const std::string host = GetTestRedisHost();

    RedisCacheConfig cfg;
    cfg.enabled = true;
    cfg.host = host;
    cfg.port = redisPort;
    cfg.cacheTtlSec = 30;

    RedisCache cache(cfg, log);
    std::string key = MakeCacheKey("inventory", "live_item_test");
    std::string verKey = MakeVersionKey(key);

    // Helper to clean key and marker in Redis
    try {
        Poco::Redis::Client cleanupClient(host, redisPort);
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

    Poco::Redis::Client rClient(host, redisPort);

    // Inspect TTL of the populated api-cache:* entry: must match configured cacheTtlSec (30s) within 1-2s tolerance
    Poco::Redis::Array cacheTtlCmd;
    cacheTtlCmd << "TTL" << key;
    auto cacheTtl = rClient.execute<Poco::Int64>(cacheTtlCmd);
    TEST_ASSERT(cacheTtl >= 28 && cacheTtl <= 30,
                "api-cache:* entry TTL after Get() population must match configured cacheTtlSec (30s)");

    // 2. Atomic Invalidation and Non-Expiring Monotonic Version Marker
    // Cache currently contains V200 from the preceding cache hit.
    // Invalidate(key, "300") atomically removes cached V200 and advances marker to V300.
    bool invOk = cache.Invalidate(key, "300");
    TEST_ASSERT(invOk, "Invalidate(key, 300) must succeed");

    // The cached V200 entry is removed and Get(key) does not return V200
    auto afterInv = cache.Get(key);
    TEST_ASSERT(!afterInv.has_value(), "Cached V200 entry must be removed after Invalidate(300)");

    Poco::Redis::Array ttlCmd;
    ttlCmd << "TTL" << verKey;
    auto vmTtl = rClient.execute<Poco::Int64>(ttlCmd);
    TEST_ASSERT(vmTtl == -1, "Committed version marker must be non-expiring (TTL == -1)");

    Poco::Redis::Array getVerCmd;
    getVerCmd << "GET" << verKey;
    auto curMarker = rClient.execute<Poco::Redis::BulkString>(getVerCmd);
    TEST_ASSERT(!curMarker.isNull() && curMarker.value() == "300", "Version marker must equal V300");

    // Monotonicity: delayed invalidation for older version V250 must NOT roll back V300 marker
    bool delInv = cache.Invalidate(key, "250");
    TEST_ASSERT(delInv, "Delayed Invalidate(250) must succeed as safe no-op");
    auto markerAfterOld = rClient.execute<Poco::Redis::BulkString>(getVerCmd);
    TEST_ASSERT(!markerAfterOld.isNull() && markerAfterOld.value() == "300", "Marker must remain at highest version (300)");

    // 3. Stale Repopulation Protection:
    // Attempting to populate V200 again against marker V300 is rejected
    bool staleSet = cache.Set(key, {"{\"name\":\"stale\"}", "200"});
    TEST_ASSERT(!staleSet, "Stale write (< 300) must be rejected");

    // Populating V300 (== marker) or V400 (> marker) succeeds
    bool validSet = cache.Set(key, {"{\"name\":\"updated\"}", "300"});
    TEST_ASSERT(validSet, "Write equal to marker (300) must succeed");
    auto curCached = cache.Get(key);
    TEST_ASSERT(curCached.has_value() && curCached->version == "300", "Cached record is now V300");

    // Inspect TTL of entry populated by Set(): must match configured cacheTtlSec (30s) within 1-2s tolerance
    Poco::Redis::Array setTtlCmd;
    setTtlCmd << "TTL" << key;
    auto setTtl = rClient.execute<Poco::Int64>(setTtlCmd);
    TEST_ASSERT(setTtl >= 28 && setTtl <= 30,
                "api-cache:* entry TTL after Set() must match configured cacheTtlSec (30s)");

    // 4. Invalidation / Delete
    bool delOk = cache.Delete(key);
    TEST_ASSERT(delOk, "Delete must succeed");
    auto afterDel = cache.Get(key);
    TEST_ASSERT(!afterDel.has_value(), "Key must not be cached after Delete");

    // 5. Bounded Staleness TTL Policy & ClampTtl Verification
    // Normal cache data TTL must always be clamped to [10s, 60s] to bound staleness if invalidation fails.
    const std::string keyMin = MakeCacheKey("inventory", "ttl_clamp_min_test");
    const std::string keyMax = MakeCacheKey("inventory", "ttl_clamp_max_test");
    const std::string keyDef = MakeCacheKey("inventory", "ttl_clamp_def_test");

    // Clean up clamping test keys beforehand
    try {
        Poco::Redis::Array delClampPreCmd;
        delClampPreCmd << "DEL" << keyMin << MakeVersionKey(keyMin)
                                << keyMax << MakeVersionKey(keyMax)
                                << keyDef << MakeVersionKey(keyDef);
        rClient.execute<Poco::Int64>(delClampPreCmd);
    } catch (...) {}

    // 5a. Lower-bound clamping: requested 1s (< 10s minimum policy) must be clamped up to 10s
    bool setMin = cache.Set(keyMin, {"{\"name\":\"min_clamped\"}", "100"}, 1);
    TEST_ASSERT(setMin, "Set() with requested TTL 1s must succeed");
    Poco::Redis::Array ttlMinCmd; ttlMinCmd << "TTL" << keyMin;
    auto ttlMin = rClient.execute<Poco::Int64>(ttlMinCmd);
    TEST_ASSERT(ttlMin >= 8 && ttlMin <= 10,
                "Lower-bound TTL (requested 1s) must be clamped to policy minimum (10s)");

    // 5b. Upper-bound clamping: requested 500s (> 60s maximum policy) must be clamped down to 60s
    bool setMax = cache.Set(keyMax, {"{\"name\":\"max_clamped\"}", "100"}, 500);
    TEST_ASSERT(setMax, "Set() with requested TTL 500s must succeed");
    Poco::Redis::Array ttlMaxCmd; ttlMaxCmd << "TTL" << keyMax;
    auto ttlMax = rClient.execute<Poco::Int64>(ttlMaxCmd);
    TEST_ASSERT(ttlMax >= 58 && ttlMax <= 60,
                "Upper-bound TTL (requested 500s) must be clamped to policy maximum (60s)");

    // 5c. Default TTL: requested 0s must use configured cacheTtlSec (30s)
    bool setDef = cache.Set(keyDef, {"{\"name\":\"default_clamped\"}", "100"}, 0);
    TEST_ASSERT(setDef, "Set() with default TTL (0) must succeed");
    Poco::Redis::Array ttlDefCmd; ttlDefCmd << "TTL" << keyDef;
    auto ttlDef = rClient.execute<Poco::Int64>(ttlDefCmd);
    TEST_ASSERT(ttlDef >= 28 && ttlDef <= 30,
                "Default TTL (0) must use configured cacheTtlSec (30s)");

    // Cleanup clamping test keys
    try {
        Poco::Redis::Array delClampPostCmd;
        delClampPostCmd << "DEL" << keyMin << MakeVersionKey(keyMin)
                                 << keyMax << MakeVersionKey(keyMax)
                                 << keyDef << MakeVersionKey(keyDef);
        rClient.execute<Poco::Int64>(delClampPostCmd);
    } catch (...) {}

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
    const std::string host = GetTestRedisHost();

    RedisCacheConfig cfg;
    cfg.enabled  = true;
    cfg.host     = host;
    cfg.port     = redisPort;
    cfg.cacheTtlSec = 30;

    RedisCache cache(cfg, log);

    // Helper keys
    const std::string key    = MakeCacheKey("inventory", "inv_invalidate_test");
    const std::string verKey = MakeVersionKey(key);

    // Helper: direct Redis client for state inspection
    Poco::Redis::Client rc(host, redisPort);

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
    cache.Invalidate(key, "50");
    TEST_ASSERT(getMarker() == "50", "C: Pre-condition: marker set to V50");
    // Delayed invalidation V30 (older than marker V50)
    ok = cache.Invalidate(key, "30");
    TEST_ASSERT(ok, "C: Older invalidation must succeed as safe no-op");
    TEST_ASSERT(getMarker() == "50", "C: Marker must remain at V50 after older invalidation V30");

    // ---- Test C2: Stale-cache handling when existing marker is newer ----
    // Existing marker: V30, Existing cached record: V20, Delayed invalidation: V10
    // Cached record V20 must be DELETED because V20 <= highest known committed (V30).
    cleanup();
    cache.Invalidate(key, "30");
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
    cache.Invalidate(key, "3");
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

        // 2. Subsequent Invalidate advances marker and preserves non-expiring retention
        cache.Invalidate(key, "200");
        Poco::Redis::Array ttlCmd2; ttlCmd2 << "TTL" << verKey;
        auto ttl2 = rc.execute<Poco::Int64>(ttlCmd2);
        TEST_ASSERT(ttl2 == -1, "I: Advanced version marker must be non-expiring (TTL == -1)");
        TEST_ASSERT(getMarker() == "200", "I: Invalidate must advance marker to V200");
    }

    // ---- Test J: Mixed-Case Key Canonicalization and Invalidation Consistency ----
    // Callers using MakeCacheKey with different casing ("Inventory" vs "inventory")
    // produce the exact same canonical Redis key, ensuring shared cache hits and no missed invalidations.
    {
        const std::string keyA = MakeCacheKey("Inventory", "mixed_case_device");
        const std::string keyB = MakeCacheKey("inventory", "mixed_case_device");
        const std::string verKeyJ = MakeVersionKey(keyA);

        auto cleanupJ = [&]() {
            try {
                Poco::Redis::Array cmd; cmd << "DEL" << keyA << verKeyJ;
                rc.execute<Poco::Int64>(cmd);
            } catch (...) {}
        };
        cleanupJ();

        TEST_ASSERT(keyA == keyB, "J: MakeCacheKey canonicalizes casing to identical key");
        TEST_ASSERT(keyA == "api-cache:inventory:mixed_case_device", "J: Canonical key is lowercase");

        // Caller A populates V10
        bool setOk = cache.Set(keyA, {"{\"name\":\"dev_mixed\"}", "10"});
        TEST_ASSERT(setOk, "J: Set via keyA must succeed");

        // Caller B reads V10 from Redis
        auto getB = cache.Get(keyB);
        TEST_ASSERT(getB.has_value() && getB->version == "10", "J: Caller B reads V10 populated by Caller A");

        // Caller A invalidates V20
        bool invOk = cache.Invalidate(keyA, "20");
        TEST_ASSERT(invOk, "J: Invalidate via keyA must succeed");

        // Caller B observes cache miss (invalidated)
        auto afterInvB = cache.Get(keyB);
        TEST_ASSERT(!afterInvB.has_value(), "J: Caller B observes cache miss after Caller A invalidation");

        // Attempting to bypass MakeCacheKey with a raw mixed-case key is strictly rejected at the boundary
        const std::string rawMixedKey = "api-cache:Inventory:mixed_case_device";
        TEST_ASSERT(!cache.Set(rawMixedKey, {"{\"name\":\"dev_raw\"}", "30"}),
                    "J: Raw mixed-case Set() must be rejected at boundary");
        TEST_ASSERT(!cache.Invalidate(rawMixedKey, "30"),
                    "J: Raw mixed-case Invalidate() must be rejected at boundary");
        TEST_ASSERT(!cache.Get(rawMixedKey).has_value(),
                    "J: Raw mixed-case Get() must return nullopt");
        TEST_ASSERT(MakeVersionKey(rawMixedKey).empty(),
                    "J: MakeVersionKey must reject raw mixed-case key");

        cleanupJ();
    }

    // ---- Test K: Exact 64-bit Version Ordering (> 2^53 and near uint64 max) ----
    // Verifies that Redis Lua scripts execute exact string-based uint64 comparison without
    // double-precision loss (which occurs above 2^53 in Lua 5.1).
    {
        const std::string keyK = MakeCacheKey("inventory", "uint64_precision_dev");
        const std::string verKeyK = MakeVersionKey(keyK);

        auto cleanupK = [&]() {
            try {
                Poco::Redis::Array cmd; cmd << "DEL" << keyK << verKeyK;
                rc.execute<Poco::Int64>(cmd);
            } catch (...) {}
        };
        auto getMarkerK = [&]() -> std::string {
            try {
                Poco::Redis::Array cmd; cmd << "GET" << verKeyK;
                auto r = rc.execute<Poco::Redis::BulkString>(cmd);
                return r.isNull() ? "" : r.value();
            } catch (...) { return ""; }
        };

        // --------------------------------------------------------------------
        // Part 1: Boundary around 2^53 (9007199254740992 vs 9007199254740993)
        // In Lua 5.1, tonumber("9007199254740992") == tonumber("9007199254740993")
        // would lose precision. cmp_uint64 must correctly distinguish them.
        // --------------------------------------------------------------------
        cleanupK();
        const std::string v2pow53        = "9007199254740992"; // 2^53
        const std::string v2pow53_plus1  = "9007199254740993"; // 2^53 + 1
        const std::string v2pow53_plus2  = "9007199254740994"; // 2^53 + 2

        // 1a. Marker monotonicity across 2^53: establish marker 2^53+1
        bool invOk = cache.Invalidate(keyK, v2pow53_plus1);
        TEST_ASSERT(invOk, "K: Invalidate(2^53+1) must succeed");
        TEST_ASSERT(getMarkerK() == v2pow53_plus1, "K: Marker set to 9007199254740993");

        // Delayed invalidation for 2^53 must NOT roll back marker 2^53+1
        bool delInv = cache.Invalidate(keyK, v2pow53);
        TEST_ASSERT(delInv, "K: Delayed Invalidate(2^53) must succeed as safe no-op");
        TEST_ASSERT(getMarkerK() == v2pow53_plus1, "K: Marker must remain at 9007199254740993");

        // 1b. Stale repopulation rejection across 2^53:
        // Attempting to populate 2^53 against marker 2^53+1 must be rejected by Lua
        bool staleSet = cache.Set(keyK, {"{\"name\":\"dev_stale\"}", v2pow53});
        TEST_ASSERT(!staleSet, "K: Population with 2^53 must be rejected against marker 2^53+1");
        TEST_ASSERT(!cache.Get(keyK).has_value(), "K: Cache must not contain stale 2^53 entry");

        // Populating 2^53+1 (equal to marker) must succeed
        bool validSet = cache.Set(keyK, {"{\"name\":\"dev_exact\"}", v2pow53_plus1});
        TEST_ASSERT(validSet, "K: Population with version equal to marker (2^53+1) must succeed");
        auto curEntry = cache.Get(keyK);
        TEST_ASSERT(curEntry.has_value() && curEntry->version == v2pow53_plus1,
                    "K: Cached record matches 2^53+1");

        // Attempting to overwrite 2^53+1 with 2^53 must be rejected
        bool staleOverwrite = cache.Set(keyK, {"{\"name\":\"dev_stale\"}", v2pow53});
        TEST_ASSERT(!staleOverwrite, "K: Overwrite with 2^53 must be rejected against cached 2^53+1");

        // Populating 2^53+2 (> cached and marker) must succeed
        bool newerSet = cache.Set(keyK, {"{\"name\":\"dev_newer\"}", v2pow53_plus2});
        TEST_ASSERT(newerSet, "K: Population with 2^53+2 must succeed");
        curEntry = cache.Get(keyK);
        TEST_ASSERT(curEntry.has_value() && curEntry->version == v2pow53_plus2,
                    "K: Cached record is now 2^53+2");

        // 1c. Preservation of newer value across 2^53 on delayed invalidation:
        // Delayed invalidations with 2^53 and 2^53+1 must preserve cached 2^53+2
        TEST_ASSERT(cache.Invalidate(keyK, v2pow53), "K: Delayed invalidation 2^53 safe no-op");
        curEntry = cache.Get(keyK);
        TEST_ASSERT(curEntry.has_value() && curEntry->version == v2pow53_plus2,
                    "K: Cached 2^53+2 must be PRESERVED on delayed invalidation of 2^53");

        TEST_ASSERT(cache.Invalidate(keyK, v2pow53_plus1), "K: Delayed invalidation 2^53+1 safe no-op");
        curEntry = cache.Get(keyK);
        TEST_ASSERT(curEntry.has_value() && curEntry->version == v2pow53_plus2,
                    "K: Cached 2^53+2 must be PRESERVED on delayed invalidation of 2^53+1");

        // Invalidation of 2^53+2 deletes the cached record
        TEST_ASSERT(cache.Invalidate(keyK, v2pow53_plus2), "K: Invalidation of 2^53+2 must succeed");
        TEST_ASSERT(!cache.Get(keyK).has_value(), "K: Cached 2^53+2 must be deleted");
        TEST_ASSERT(getMarkerK() == v2pow53_plus2, "K: Marker advanced to 2^53+2");

        // --------------------------------------------------------------------
        // Part 2: Boundary near UINT64_MAX (18446744073709551614 vs 18446744073709551615)
        // --------------------------------------------------------------------
        cleanupK();
        const std::string vMaxMinus1 = "18446744073709551614"; // 2^64 - 2
        const std::string vMax       = "18446744073709551615"; // 2^64 - 1 (UINT64_MAX)
        const std::string vOverflow  = "18446744073709551616"; // 2^64 (overflow)

        // 2a. Marker monotonicity near UINT64_MAX: establish marker UINT64_MAX
        TEST_ASSERT(cache.Invalidate(keyK, vMax), "K: Invalidate(UINT64_MAX) must succeed");
        TEST_ASSERT(getMarkerK() == vMax, "K: Marker set to UINT64_MAX");

        // Delayed invalidation for UINT64_MAX-1 must NOT roll back UINT64_MAX marker
        TEST_ASSERT(cache.Invalidate(keyK, vMaxMinus1), "K: Delayed Invalidate(MAX-1) must succeed as safe no-op");
        TEST_ASSERT(getMarkerK() == vMax, "K: Marker must remain at UINT64_MAX");

        // 2b. Stale repopulation rejection near UINT64_MAX:
        // Attempting to populate MAX-1 against marker MAX must be rejected
        bool staleMaxMinus1 = cache.Set(keyK, {"{\"name\":\"dev_stale_max\"}", vMaxMinus1});
        TEST_ASSERT(!staleMaxMinus1, "K: Population with MAX-1 must be rejected against marker MAX");
        TEST_ASSERT(!cache.Get(keyK).has_value(), "K: Cache must not contain stale MAX-1");

        // Populating MAX (equal to marker) must succeed
        bool validMax = cache.Set(keyK, {"{\"name\":\"dev_max\"}", vMax});
        TEST_ASSERT(validMax, "K: Population with UINT64_MAX must succeed");
        curEntry = cache.Get(keyK);
        TEST_ASSERT(curEntry.has_value() && curEntry->version == vMax, "K: Cached record matches UINT64_MAX");

        // 2c. Preservation of newer value near UINT64_MAX:
        // Delayed invalidation with MAX-1 must preserve cached MAX entry
        TEST_ASSERT(cache.Invalidate(keyK, vMaxMinus1), "K: Delayed invalidation MAX-1 safe no-op");
        curEntry = cache.Get(keyK);
        TEST_ASSERT(curEntry.has_value() && curEntry->version == vMax,
                    "K: Cached UINT64_MAX must be PRESERVED on delayed invalidation MAX-1");

        // Invalidation of MAX deletes cached entry
        TEST_ASSERT(cache.Invalidate(keyK, vMax), "K: Invalidation of UINT64_MAX must succeed");
        TEST_ASSERT(!cache.Get(keyK).has_value(), "K: Cached UINT64_MAX must be deleted");
        TEST_ASSERT(getMarkerK() == vMax, "K: Marker remains at UINT64_MAX");

        // 2d. Overflow rejection: version 18446744073709551616 rejected by C++ and Lua
        TEST_ASSERT(!cache.Set(keyK, {"{\"name\":\"overflow\"}", vOverflow}), "K: Overflow version Set rejected");
        TEST_ASSERT(!cache.Invalidate(keyK, vOverflow), "K: Overflow version Invalidate rejected");

        // --------------------------------------------------------------------
        // Part 3: Digit Length Boundary (19 digits vs 20 digits)
        // Lexicographically "9..." > "1...", but numerically 19 digits < 20 digits
        // --------------------------------------------------------------------
        cleanupK();
        const std::string v19digits = "9999999999999999999";  // 19 digits
        const std::string v20digits = "10000000000000000000"; // 20 digits

        // Establish marker v20digits
        TEST_ASSERT(cache.Invalidate(keyK, v20digits), "K: Invalidate(20 digits) must succeed");
        TEST_ASSERT(getMarkerK() == v20digits, "K: Marker set to 20 digits");

        // Delayed invalidation for 19 digits must not roll back 20-digit marker
        TEST_ASSERT(cache.Invalidate(keyK, v19digits), "K: Delayed Invalidate(19 digits) must succeed as safe no-op");
        TEST_ASSERT(getMarkerK() == v20digits, "K: Marker must remain at 20 digits");

        // Stale write with 19 digits rejected against 20-digit marker
        TEST_ASSERT(!cache.Set(keyK, {"{\"name\":\"dev_19\"}", v19digits}),
                    "K: Population with 19 digits rejected against 20-digit marker");

        // Populate 20 digits succeeds
        TEST_ASSERT(cache.Set(keyK, {"{\"name\":\"dev_20\"}", v20digits}),
                    "K: Population with 20 digits must succeed");
        curEntry = cache.Get(keyK);
        TEST_ASSERT(curEntry.has_value() && curEntry->version == v20digits, "K: Cached record matches 20 digits");

        // Delayed 19-digit invalidation preserves cached 20-digit entry
        TEST_ASSERT(cache.Invalidate(keyK, v19digits), "K: Delayed 19-digit invalidation must succeed as safe no-op");
        curEntry = cache.Get(keyK);
        TEST_ASSERT(curEntry.has_value() && curEntry->version == v20digits,
                    "K: Cached 20-digit entry must be PRESERVED on delayed 19-digit invalidation");

        cleanupK();
    }

    cleanup();
    std::cout << "  PASS: test_atomic_invalidation" << std::endl;
#else
    (void)redisPort;
    std::cout << "  SKIPPED: USE_POCO_REDIS not defined" << std::endl;
#endif
}

#ifdef USE_POCO_REDIS
// ---------------------------------------------------------------------------
// Suite 5 - Test A: Two Independent RedisCache Instances sharing the same Redis server
// ---------------------------------------------------------------------------
static void test_two_independent_instances(int redisPort) {
    std::cout << "--- Suite 5 - Test A: Two Independent RedisCache Instances (Port " << redisPort << ") ---" << std::endl;
    Poco::Logger &log = Poco::Logger::get("Test");
    const std::string host = GetTestRedisHost();

    RedisCacheConfig cfgA;
    cfgA.enabled = true;
    cfgA.host = host;
    cfgA.port = redisPort;
    cfgA.cacheTtlSec = 30;

    RedisCacheConfig cfgB = cfgA;

    RedisCache cacheA(cfgA, log);
    RedisCache cacheB(cfgB, log);

    const std::string key = MakeCacheKey("inventory", "two_inst_shared_dev");
    const std::string verKey = MakeVersionKey(key);

    Poco::Redis::Client rc(host, redisPort);
    auto cleanup = [&]() {
        try {
            Poco::Redis::Array cmd; cmd << "DEL" << key << verKey;
            rc.execute<Poco::Int64>(cmd);
        } catch (...) {}
    };
    cleanup();

    std::atomic<int> dbReadsA{0};
    std::atomic<int> dbReadsB{0};

    // 1. Instance A loads version V1 through authoritative loader and populates Redis
    auto loaderA_V1 = [&]() -> std::optional<RedisCache::Entry> {
        dbReadsA++;
        return RedisCache::Entry{R"({"serialNumber":"DEV-01","version":"1"})", "1"};
    };
    auto outA1 = cacheA.Get(key, loaderA_V1);
    TEST_ASSERT(outA1.result == RedisCache::Result::Miss, "A: initial read must be Miss");
    TEST_ASSERT(outA1.entry.has_value() && outA1.entry->version == "1", "A: returns loaded V1");
    TEST_ASSERT(dbReadsA == 1, "A: DB loader must be invoked once");

    // 2. Instance B reads the same key and obtains V1 directly from Redis without calling its DB loader
    auto loaderB_Fail = [&]() -> std::optional<RedisCache::Entry> {
        dbReadsB++;
        return RedisCache::Entry{R"({"serialNumber":"DEV-01","version":"1"})", "1"};
    };
    auto outB1 = cacheB.Get(key, loaderB_Fail);
    TEST_ASSERT(outB1.result == RedisCache::Result::Hit, "B: reads V1 as Hit from shared Redis");
    TEST_ASSERT(outB1.entry.has_value() && outB1.entry->version == "1", "B: returns V1");
    TEST_ASSERT(dbReadsB == 0, "B: DB loader must NOT be invoked on Hit");

    // 3. Instance A invalidates the cache after a simulated commit of V2
    bool invA = cacheA.Invalidate(key, "2");
    TEST_ASSERT(invA, "A: invalidation for committed V2 must succeed");

    // Verify key deleted from Redis and marker advanced to 2
    {
        Poco::Redis::Array getCacheCmd; getCacheCmd << "GET" << key;
        auto cVal = rc.execute<Poco::Redis::BulkString>(getCacheCmd);
        TEST_ASSERT(cVal.isNull(), "A: key must be deleted in Redis after invalidation");
        Poco::Redis::Array getMarkerCmd; getMarkerCmd << "GET" << verKey;
        auto mVal = rc.execute<Poco::Redis::BulkString>(getMarkerCmd);
        TEST_ASSERT(!mVal.isNull() && mVal.value() == "2", "A: marker must be advanced to 2 in Redis");
    }

    // 4. Instance B encounters a cache miss, loads V2, and repopulates Redis
    auto loaderB_V2 = [&]() -> std::optional<RedisCache::Entry> {
        dbReadsB++;
        return RedisCache::Entry{R"({"serialNumber":"DEV-01","version":"2"})", "2"};
    };
    auto outB2 = cacheB.Get(key, loaderB_V2);
    TEST_ASSERT(outB2.result == RedisCache::Result::Miss, "B: encounters Miss after invalidation");
    TEST_ASSERT(outB2.entry.has_value() && outB2.entry->version == "2", "B: loads V2 from DB");
    TEST_ASSERT(dbReadsB == 1, "B: DB loader invoked once for V2");

    // 5. Instance A subsequently reads V2 from Redis without calling its DB loader
    auto outA2 = cacheA.Get(key, loaderA_V1);
    TEST_ASSERT(outA2.result == RedisCache::Result::Hit, "A: subsequently reads V2 as Hit from shared Redis");
    TEST_ASSERT(outA2.entry.has_value() && outA2.entry->version == "2", "A: returns V2");
    TEST_ASSERT(dbReadsA == 1, "A: DB loader must NOT be invoked again");

    cleanup();
    std::cout << "  PASS: test_two_independent_instances" << std::endl;
}

// ---------------------------------------------------------------------------
// Suite 5 - Test B: Deterministic Stale-Repopulation Race
// ---------------------------------------------------------------------------
static void test_deterministic_stale_repopulation_race(int redisPort) {
    std::cout << "--- Suite 5 - Test B: Deterministic Stale-Repopulation Race (Port " << redisPort << ") ---" << std::endl;
    Poco::Logger &log = Poco::Logger::get("Test");
    const std::string host = GetTestRedisHost();

    RedisCacheConfig cfg;
    cfg.enabled = true;
    cfg.host = host;
    cfg.port = redisPort;
    cfg.cacheTtlSec = 30;

    RedisCache cacheA(cfg, log);
    RedisCache cacheB(cfg, log);

    const std::string key = MakeCacheKey("inventory", "race_stale_repop_dev");
    const std::string verKey = MakeVersionKey(key);

    Poco::Redis::Client rc(host, redisPort);
    auto cleanup = [&]() {
        try {
            Poco::Redis::Array cmd; cmd << "DEL" << key << verKey;
            rc.execute<Poco::Int64>(cmd);
        } catch (...) {}
    };

    // 1. Redis initially has no cached entry
    cleanup();

    std::mutex mtx;
    std::condition_variable cv_entered;
    std::condition_variable cv_resume;
    bool loader_entered = false;
    bool resume_loader = false;
    RedisCache::Outcome outcomeA;

    // 2. Instance A begins Get() and enters its authoritative loader
    std::thread threadA([&]() {
        auto loaderA = [&]() -> std::optional<RedisCache::Entry> {
            // 3. Instance A loads V1 but is paused before returning from the loader
            {
                std::lock_guard<std::mutex> lock(mtx);
                loader_entered = true;
            }
            cv_entered.notify_one();

            {
                std::unique_lock<std::mutex> lock(mtx);
                bool resumed = cv_resume.wait_for(lock, std::chrono::seconds(5), [&]() {
                    return resume_loader;
                });
                TEST_ASSERT(resumed, "Timed out waiting to resume Instance A loader");
            }
            return RedisCache::Entry{R"({"serialNumber":"RACE-01","version":"1"})", "1"};
        };

        outcomeA = cacheA.Get(key, loaderA);
    });

    // Wait until Instance A has entered its loader
    {
        std::unique_lock<std::mutex> lock(mtx);
        bool entered = cv_entered.wait_for(lock, std::chrono::seconds(5), [&]() {
            return loader_entered;
        });
        TEST_ASSERT(entered, "Timed out waiting for Instance A to enter loader");
    }

    // 4. Instance B invalidates the key with committed version V2
    bool invB = cacheB.Invalidate(key, "2");
    TEST_ASSERT(invB, "Instance B invalidation V2 must succeed");

    // 5. Confirm Redis records the V2 committed-version marker
    {
        Poco::Redis::Array getMarkerCmd; getMarkerCmd << "GET" << verKey;
        auto mVal = rc.execute<Poco::Redis::BulkString>(getMarkerCmd);
        TEST_ASSERT(!mVal.isNull() && mVal.value() == "2", "Redis must record marker V2");
    }

    // 6. Resume Instance A's loader
    {
        std::lock_guard<std::mutex> lock(mtx);
        resume_loader = true;
    }
    cv_resume.notify_one();

    // 7. Instance A attempts to populate the old V1; wait for thread A
    threadA.join();

    // Assert Instance A returns the loaded V1 database snapshot as Result::Fallback
    // because Lua rejected writing stale V1 into Redis (V1 < marker V2).
    TEST_ASSERT(outcomeA.result == RedisCache::Result::Fallback,
                "Instance A must return Result::Fallback when stale population is rejected by Lua");
    TEST_ASSERT(outcomeA.entry.has_value() && outcomeA.entry->version == "1",
                "Instance A must return the loaded V1 database snapshot to the caller");
    TEST_ASSERT(cacheA.IsRedisHealthy(),
                "Instance A must report Redis as healthy (demonstrates stale-rejection, not connection failure)");

    // 8 & 9. Verify the Lua population operation rejected V1 and Redis never contains V1 after V2 invalidation
    {
        Poco::Redis::Array getCacheCmd; getCacheCmd << "GET" << key;
        auto cVal = rc.execute<Poco::Redis::BulkString>(getCacheCmd);
        TEST_ASSERT(cVal.isNull(), "Redis cache must NOT contain V1 after V2 invalidation (stale write rejected by Lua)");
    }

    // 10. Instance B loads V2 and successfully populates Redis
    auto loaderB_V2 = []() -> std::optional<RedisCache::Entry> {
        return RedisCache::Entry{R"({"serialNumber":"RACE-01","version":"2"})", "2"};
    };
    auto outB = cacheB.Get(key, loaderB_V2);
    TEST_ASSERT(outB.result == RedisCache::Result::Miss, "Instance B loader populates V2");
    TEST_ASSERT(outB.entry.has_value() && outB.entry->version == "2", "Instance B returns loaded V2");

    // 11. Verify subsequent reads from both instances return cached V2 without calling loaders
    std::atomic<int> dummyLoads{0};
    auto dummyLoader = [&]() -> std::optional<RedisCache::Entry> {
        dummyLoads++;
        return std::nullopt;
    };
    auto hitA = cacheA.Get(key, dummyLoader);
    TEST_ASSERT(hitA.result == RedisCache::Result::Hit && hitA.entry.has_value() && hitA.entry->version == "2",
                "Instance A reads cached V2");
    auto hitB = cacheB.Get(key, dummyLoader);
    TEST_ASSERT(hitB.result == RedisCache::Result::Hit && hitB.entry.has_value() && hitB.entry->version == "2",
                "Instance B reads cached V2");
    TEST_ASSERT(dummyLoads == 0, "No database loader called on cache hits");

    // 12. Multi-instance race across Lua 2^53 boundary: 9007199254740992 vs 9007199254740993
    cleanup();
    const std::string vLow  = "9007199254740992"; // 2^53
    const std::string vHigh = "9007199254740993"; // 2^53 + 1
    loader_entered = false;
    resume_loader = false;
    outcomeA = RedisCache::Outcome{};

    std::thread threadA_High([&]() {
        auto loaderA_High = [&]() -> std::optional<RedisCache::Entry> {
            {
                std::lock_guard<std::mutex> lock(mtx);
                loader_entered = true;
            }
            cv_entered.notify_one();

            {
                std::unique_lock<std::mutex> lock(mtx);
                bool resumed = cv_resume.wait_for(lock, std::chrono::seconds(5), [&]() {
                    return resume_loader;
                });
                TEST_ASSERT(resumed, "Timed out waiting to resume Instance A high-version loader");
            }
            return RedisCache::Entry{R"({"serialNumber":"RACE-HIGH","version":"9007199254740992"})", vLow};
        };

        outcomeA = cacheA.Get(key, loaderA_High);
    });

    // Wait until Instance A enters loader
    {
        std::unique_lock<std::mutex> lock(mtx);
        bool entered = cv_entered.wait_for(lock, std::chrono::seconds(5), [&]() {
            return loader_entered;
        });
        TEST_ASSERT(entered, "Timed out waiting for Instance A to enter high-version loader");
    }

    // Instance B invalidates with 2^53 + 1
    bool invB_High = cacheB.Invalidate(key, vHigh);
    TEST_ASSERT(invB_High, "Instance B invalidation 2^53+1 must succeed");

    // Resume Instance A
    {
        std::lock_guard<std::mutex> lock(mtx);
        resume_loader = true;
    }
    cv_resume.notify_one();
    threadA_High.join();

    // Instance A's population of 2^53 must be rejected by Lua (since 2^53 < 2^53 + 1)
    TEST_ASSERT(outcomeA.result == RedisCache::Result::Fallback,
                "Instance A must return Fallback when 2^53 stale population is rejected against 2^53+1");
    TEST_ASSERT(outcomeA.entry.has_value() && outcomeA.entry->version == vLow,
                "Instance A returns loaded 2^53 database snapshot");
    TEST_ASSERT(cacheA.IsRedisHealthy(), "Instance A reports Redis healthy");

    // Redis cache does not contain stale 2^53
    {
        Poco::Redis::Array getCacheCmd; getCacheCmd << "GET" << key;
        auto cVal = rc.execute<Poco::Redis::BulkString>(getCacheCmd);
        TEST_ASSERT(cVal.isNull(), "Redis cache must NOT contain 2^53 after 2^53+1 invalidation");
    }

    // Instance B loads 2^53 + 1 and populates Redis
    auto loaderB_High = [&]() -> std::optional<RedisCache::Entry> {
        return RedisCache::Entry{R"({"serialNumber":"RACE-HIGH","version":"9007199254740993"})", vHigh};
    };
    auto outB_High = cacheB.Get(key, loaderB_High);
    TEST_ASSERT(outB_High.result == RedisCache::Result::Miss, "Instance B loader populates 2^53+1");
    TEST_ASSERT(outB_High.entry.has_value() && outB_High.entry->version == vHigh, "Instance B returns 2^53+1");

    // Both instances hit cached 2^53 + 1
    dummyLoads = 0;
    auto hitA_High = cacheA.Get(key, dummyLoader);
    TEST_ASSERT(hitA_High.result == RedisCache::Result::Hit && hitA_High.entry->version == vHigh,
                "Instance A reads cached 2^53+1");
    auto hitB_High = cacheB.Get(key, dummyLoader);
    TEST_ASSERT(hitB_High.result == RedisCache::Result::Hit && hitB_High.entry->version == vHigh,
                "Instance B reads cached 2^53+1");
    TEST_ASSERT(dummyLoads == 0, "No database loader called on cache hits for 2^53+1");

    cleanup();
    std::cout << "  PASS: test_deterministic_stale_repopulation_race" << std::endl;
}

// ---------------------------------------------------------------------------
// Suite 5 - Test C: Concurrent and Out-of-Order Invalidation
// ---------------------------------------------------------------------------
static void test_concurrent_and_out_of_order_invalidation(int redisPort) {
    std::cout << "--- Suite 5 - Test C: Concurrent & Out-of-Order Invalidation (Port " << redisPort << ") ---" << std::endl;
    Poco::Logger &log = Poco::Logger::get("Test");
    const std::string host = GetTestRedisHost();

    RedisCacheConfig cfg;
    cfg.enabled = true;
    cfg.host = host;
    cfg.port = redisPort;
    cfg.cacheTtlSec = 30;

    RedisCache cacheA(cfg, log);
    RedisCache cacheB(cfg, log);

    const std::string key = MakeCacheKey("inventory", "concurrent_inv_dev");
    const std::string verKey = MakeVersionKey(key);

    Poco::Redis::Client rc(host, redisPort);
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

    cleanup();

    // 1. Populate cached V1 on Instance A
    bool setV1 = cacheA.Set(key, {R"({"name":"dev","version":"1"})", "1"}, 30);
    TEST_ASSERT(setV1, "Set V1 must succeed");
    TEST_ASSERT(cacheA.Get(key).has_value(), "Cached V1 must exist");

    // V2 invalidation on Instance A deletes cached V1
    bool invV2 = cacheA.Invalidate(key, "2");
    TEST_ASSERT(invV2, "V2 invalidation must succeed");
    TEST_ASSERT(!cacheA.Get(key).has_value(), "Cached V1 must be deleted after V2 invalidation");
    TEST_ASSERT(getMarker() == "2", "Marker must be V2");

    // 2. A later V3 invalidation on Instance B advances the marker
    bool invV3 = cacheB.Invalidate(key, "3");
    TEST_ASSERT(invV3, "V3 invalidation must succeed");
    TEST_ASSERT(getMarker() == "3", "Marker must advance to V3");

    // 3. Populate cache with V3 on Instance B (V3 == marker 3)
    bool setV3 = cacheB.Set(key, {R"({"name":"dev","version":"3"})", "3"}, 30);
    TEST_ASSERT(setV3, "Set V3 must succeed");
    TEST_ASSERT(cacheA.Get(key).has_value() && cacheA.Get(key)->version == "3", "Cache contains V3");

    // 4. A delayed V2 invalidation from Instance A does NOT regress V3 marker
    bool delInvV2 = cacheA.Invalidate(key, "2");
    TEST_ASSERT(delInvV2, "Delayed V2 invalidation must succeed as safe no-op");
    TEST_ASSERT(getMarker() == "3", "Marker must remain V3 after delayed V2 invalidation");

    // 5. A delayed V2 invalidation does NOT delete cached V3
    auto cachedAfterDel = cacheA.Get(key);
    TEST_ASSERT(cachedAfterDel.has_value() && cachedAfterDel->version == "3",
                "Cached V3 must be preserved upon delayed V2 invalidation");

    // 6. Repeated invalidations remain idempotent:
    // 6a. Repeated delayed V2 invalidation from Instance B is idempotent and preserves cached V3
    bool repDelayed = cacheB.Invalidate(key, "2");
    TEST_ASSERT(repDelayed, "Repeated delayed V2 invalidation must succeed idempotently");
    TEST_ASSERT(getMarker() == "3", "Marker remains V3 after repeated delayed invalidation");
    auto cachedAfterRepDelayed = cacheA.Get(key);
    TEST_ASSERT(cachedAfterRepDelayed.has_value() && cachedAfterRepDelayed->version == "3",
                "Cached V3 preserved after repeated delayed V2 invalidation");

    // 7. An older population attempt cannot overwrite cached V3
    bool staleSet2 = cacheA.Set(key, {R"({"name":"stale","version":"2"})", "2"}, 30);
    TEST_ASSERT(!staleSet2, "Older population V2 must be rejected when V3 is cached");
    bool staleSet1 = cacheB.Set(key, {R"({"name":"stale","version":"1"})", "1"}, 30);
    TEST_ASSERT(!staleSet1, "Older population V1 must be rejected when V3 is cached");
    auto finalCached = cacheA.Get(key);
    TEST_ASSERT(finalCached.has_value() && finalCached->version == "3",
                "Cached record remains V3 after rejected stale population attempts");

    // 6b. Repeated invalidations for V3 itself are idempotent:
    // First V3 invalidation deletes cached V3 and sets marker V3; second call is a safe idempotent no-op
    bool invV3First = cacheA.Invalidate(key, "3");
    TEST_ASSERT(invV3First, "V3 invalidation deletes cached V3");
    TEST_ASSERT(!cacheA.Get(key).has_value(), "Cached V3 deleted after V3 invalidation");
    TEST_ASSERT(getMarker() == "3", "Marker is V3");

    bool invV3Second = cacheB.Invalidate(key, "3");
    TEST_ASSERT(invV3Second, "Repeated identical V3 invalidation must succeed idempotently");
    TEST_ASSERT(getMarker() == "3", "Marker remains V3 after repeated V3 invalidation");

    cleanup();
    std::cout << "  PASS: test_concurrent_and_out_of_order_invalidation" << std::endl;
}

// ---------------------------------------------------------------------------
// Suite 5 - Test D: Post-Commit and Rollback Behavior
// ---------------------------------------------------------------------------
static void test_post_commit_and_rollback(int redisPort) {
    std::cout << "--- Suite 5 - Test D: Post-Commit & Rollback Behavior (Port " << redisPort << ") ---" << std::endl;
    Poco::Logger &log = Poco::Logger::get("Test");
    const std::string host = GetTestRedisHost();

    RedisCacheConfig cfg;
    cfg.enabled = true;
    cfg.host = host;
    cfg.port = redisPort;
    cfg.cacheTtlSec = 30;

    RedisCache cache(cfg, log);

    const std::string key1 = MakeCacheKey("inventory", "tx_key_1");
    const std::string verKey1 = MakeVersionKey(key1);
    const std::string key2 = MakeCacheKey("inventory", "tx_key_2");
    const std::string verKey2 = MakeVersionKey(key2);
    const std::string key3 = MakeCacheKey("inventory", "tx_key_3");
    const std::string verKey3 = MakeVersionKey(key3);
    const std::string keyMultiA = MakeCacheKey("inventory", "tx_multi_a");
    const std::string verKeyMultiA = MakeVersionKey(keyMultiA);
    const std::string keyMultiB = MakeCacheKey("inventory", "tx_multi_b");
    const std::string verKeyMultiB = MakeVersionKey(keyMultiB);

    Poco::Redis::Client rc(host, redisPort);
    auto cleanup = [&]() {
        try {
            Poco::Redis::Array cmd;
            cmd << "DEL" << key1 << verKey1 << key2 << verKey2 << key3 << verKey3
                << keyMultiA << verKeyMultiA << keyMultiB << verKeyMultiB;
            rc.execute<Poco::Int64>(cmd);
        } catch (...) {}
    };

    cleanup();

    Poco::Data::SQLite::Connector::registerConnector();
    Poco::Data::Session dbSession("SQLite", ":memory:");

    // 1. Invalidation is not executed before DbTransaction::Commit()
    // 2. Successful commit executes registered invalidation
    {
        cache.Set(key1, {R"({"val":"orig","version":"1"})", "1"}, 30);
        TEST_ASSERT(cache.Get(key1).has_value(), "Pre-condition: key1 is cached at V1");

        DbTransaction tx(dbSession, log);
        bool regOk = RegisterCacheInvalidationAfterCommit(tx, cache, {{key1, "2"}});
        TEST_ASSERT(regOk, "Registration must succeed");

        // Before commit: invalidation must NOT have run yet
        auto beforeCommit = cache.Get(key1);
        TEST_ASSERT(beforeCommit.has_value() && beforeCommit->version == "1",
                    "Invalidation must NOT execute before DbTransaction::Commit()");

        // Successful commit executes invalidation
        bool commitOk = tx.Commit();
        TEST_ASSERT(commitOk, "Commit must succeed");

        auto afterCommit = cache.Get(key1);
        TEST_ASSERT(!afterCommit.has_value(), "Cached key1 must be deleted after Commit()");
        Poco::Redis::Array getMarkerCmd; getMarkerCmd << "GET" << verKey1;
        auto mVal = rc.execute<Poco::Redis::BulkString>(getMarkerCmd);
        TEST_ASSERT(!mVal.isNull() && mVal.value() == "2", "Marker1 must be set to V2 after Commit()");
    }

    // 3. Explicit rollback does not invalidate Redis
    {
        cache.Invalidate(key2, "1");
        cache.Set(key2, {R"({"val":"orig","version":"1"})", "1"}, 30);
        TEST_ASSERT(cache.Get(key2).has_value(), "Pre-condition: key2 is cached at V1");

        DbTransaction tx(dbSession, log);
        bool regOk = RegisterCacheInvalidationAfterCommit(tx, cache, {{key2, "2"}});
        TEST_ASSERT(regOk, "Registration must succeed");

        bool rbOk = tx.Rollback();
        TEST_ASSERT(rbOk, "Explicit Rollback must succeed");

        // Cache entry must still be intact and marker must still be 1
        auto afterRb = cache.Get(key2);
        TEST_ASSERT(afterRb.has_value() && afterRb->version == "1",
                    "Cached key2 must remain intact after explicit Rollback()");
        Poco::Redis::Array getMarkerCmd; getMarkerCmd << "GET" << verKey2;
        auto mVal = rc.execute<Poco::Redis::BulkString>(getMarkerCmd);
        TEST_ASSERT(!mVal.isNull() && mVal.value() == "1", "Marker2 must NOT advance after Rollback()");
    }

    // 4. Failed transaction does not invalidate Redis
    {
        cache.Invalidate(key3, "1");
        cache.Set(key3, {R"({"val":"orig","version":"1"})", "1"}, 30);
        TEST_ASSERT(cache.Get(key3).has_value(), "Pre-condition: key3 is cached at V1");

        DbTransaction tx(dbSession, log);
        bool regOk = RegisterCacheInvalidationAfterCommit(tx, cache, {{key3, "2"}});
        TEST_ASSERT(regOk, "Registration must succeed");

        tx.MarkFailed("23505", "Simulated unique constraint collision");
        bool commitRes = tx.Commit();
        TEST_ASSERT(!commitRes, "Commit must fail when transaction is marked failed");
        TEST_ASSERT(tx.HasFailed(), "Transaction must report HasFailed()");

        // Cache entry must still be intact and marker must still be 1
        auto afterFail = cache.Get(key3);
        TEST_ASSERT(afterFail.has_value() && afterFail->version == "1",
                    "Cached key3 must remain intact after transaction failure");
        Poco::Redis::Array getMarkerCmd; getMarkerCmd << "GET" << verKey3;
        auto mVal = rc.execute<Poco::Redis::BulkString>(getMarkerCmd);
        TEST_ASSERT(!mVal.isNull() && mVal.value() == "1", "Marker3 must NOT advance after transaction failure");
    }

    // 5. Multiple registered keys are invalidated independently
    {
        cache.Set(keyMultiA, {R"({"val":"a","version":"1"})", "1"}, 30);
        cache.Set(keyMultiB, {R"({"val":"b","version":"10"})", "10"}, 30);
        TEST_ASSERT(cache.Get(keyMultiA).has_value() && cache.Get(keyMultiB).has_value(),
                    "Pre-condition: multi keys cached");

        DbTransaction tx(dbSession, log);
        bool regOk = RegisterCacheInvalidationAfterCommit(tx, cache, {
            {keyMultiA, "2"},
            {keyMultiB, "20"}
        });
        TEST_ASSERT(regOk, "Multi-target registration must succeed");

        bool commitOk = tx.Commit();
        TEST_ASSERT(commitOk, "Multi-target commit must succeed");

        // Both keys must be invalidated independently
        TEST_ASSERT(!cache.Get(keyMultiA).has_value(), "keyMultiA must be deleted");
        TEST_ASSERT(!cache.Get(keyMultiB).has_value(), "keyMultiB must be deleted");

        Poco::Redis::Array getMarkerCmdA; getMarkerCmdA << "GET" << verKeyMultiA;
        auto mA = rc.execute<Poco::Redis::BulkString>(getMarkerCmdA);
        TEST_ASSERT(!mA.isNull() && mA.value() == "2", "verKeyMultiA must be 2");

        Poco::Redis::Array getMarkerCmdB; getMarkerCmdB << "GET" << verKeyMultiB;
        auto mB = rc.execute<Poco::Redis::BulkString>(getMarkerCmdB);
        TEST_ASSERT(!mB.isNull() && mB.value() == "20", "verKeyMultiB must be 20");
    }

    // 6. Redis invalidation failures do not change the result of an already successful PostgreSQL/SQLite transaction
    {
        RedisCacheConfig offCfg;
        offCfg.enabled = true;
        offCfg.host = "127.0.0.1";
        offCfg.port = 16378; // Unreachable port
        offCfg.timeoutMs = 50;
        RedisCache offCache(offCfg, log);

        const std::string offKey = MakeCacheKey("inventory", "tx_offline_key");

        DbTransaction tx(dbSession, log);
        bool regOk = RegisterCacheInvalidationAfterCommit(tx, offCache, {{offKey, "50"}});
        TEST_ASSERT(regOk, "Registration with valid target on offline cache must return true pre-commit");

        // Commit must succeed! Redis failure after commit must NOT make the DB transaction appear failed.
        bool commitOk = tx.Commit();
        TEST_ASSERT(commitOk, "Transaction Commit must return true even if Redis invalidation fails post-commit");
        TEST_ASSERT(!tx.HasFailed(), "Transaction must NOT be marked failed by post-commit Redis failure");
    }

    cleanup();
    std::cout << "  PASS: test_post_commit_and_rollback" << std::endl;
}
#endif

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    std::cout << "=== Running RedisCache Framework Tests ===" << std::endl;

    // Suites 1 and 2 run in all environments (including GitHub Actions CI without Redis)
    test_config_and_namespaces();
    test_fallback_and_validation();

    // Suite 3, 4, 5 run if a test Redis instance is running locally, otherwise cleanly skips.
    // When REQUIRE_REDIS_TESTS=1, DetectRedisPort() fails immediately with fatal exit if Redis is unavailable.
    int redisPort = DetectRedisPort();
    if (redisPort == 0) {
        std::cout << "Notice: No Redis server detected on port 16379 or 6379." << std::endl;
        std::cout << "Live Redis integration tests skipped (normal in offline build environments)." << std::endl;
    } else {
        std::cout << "Live Redis server detected on " << GetTestRedisHost() << ":" << redisPort << std::endl;
        test_live_redis_integration(redisPort);
        test_atomic_invalidation(redisPort);
#ifdef USE_POCO_REDIS
        test_two_independent_instances(redisPort);
        test_deterministic_stale_repopulation_race(redisPort);
        test_concurrent_and_out_of_order_invalidation(redisPort);
        test_post_commit_and_rollback(redisPort);
#endif
    }

    std::cout << "=== All RedisCache Tests PASSED! ===" << std::endl;
    return 0;
}
