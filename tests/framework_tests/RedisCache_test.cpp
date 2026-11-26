/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Poco/AutoPtr.h"
#include "Poco/Channel.h"
#include "Poco/Logger.h"
#include "Poco/Message.h"
#include "Poco/Net/StreamSocket.h"
#include "Poco/Net/SocketAddress.h"
#include "fmt/format.h"

#ifdef USE_POCO_REDIS
#include "Poco/Net/ServerSocket.h"
#include "Poco/Redis/Array.h"
#include "Poco/Redis/Client.h"
#include "Poco/Redis/Command.h"
#include "Poco/Redis/Type.h"
#include "Poco/Timespan.h"
#endif

#include "Poco/Data/SQLite/Connector.h"
#include "Poco/Data/Session.h"
#include "Poco/Data/Statement.h"
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

    // 2. Host empty or "none"
    g_TestConfig["openwifi.redis.enable"] = "true";
    g_TestConfig["openwifi.redis.host"] = "";
    auto cfg2 = MakeRedisCacheConfig();
    TEST_ASSERT(!cfg2.enabled, "Config must be disabled when host is empty");

    g_TestConfig["openwifi.redis.enable"] = "true";
    g_TestConfig["openwifi.redis.host"] = "none";
    auto cfgNone = MakeRedisCacheConfig();
    TEST_ASSERT(!cfgNone.enabled, "Config must be disabled when Redis host is 'none'");

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

    // 4. Bounded Staleness TTL Policy & ClampTtl Verification
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

    // 4a. Lower-bound clamping: requested 1s (< 10s minimum policy) must be clamped up to 10s
    bool setMin = cache.Set(keyMin, {"{\"name\":\"min_clamped\"}", "100"}, 1);
    TEST_ASSERT(setMin, "Set() with requested TTL 1s must succeed");
    Poco::Redis::Array ttlMinCmd; ttlMinCmd << "TTL" << keyMin;
    auto ttlMin = rClient.execute<Poco::Int64>(ttlMinCmd);
    TEST_ASSERT(ttlMin >= 8 && ttlMin <= 10,
                "Lower-bound TTL (requested 1s) must be clamped to policy minimum (10s)");

    // 4b. Upper-bound clamping: requested 500s (> 60s maximum policy) must be clamped down to 60s
    bool setMax = cache.Set(keyMax, {"{\"name\":\"max_clamped\"}", "100"}, 500);
    TEST_ASSERT(setMax, "Set() with requested TTL 500s must succeed");
    Poco::Redis::Array ttlMaxCmd; ttlMaxCmd << "TTL" << keyMax;
    auto ttlMax = rClient.execute<Poco::Int64>(ttlMaxCmd);
    TEST_ASSERT(ttlMax >= 58 && ttlMax <= 60,
                "Upper-bound TTL (requested 500s) must be clamped to policy maximum (60s)");

    // 4c. Default TTL: requested 0s must use configured cacheTtlSec (30s)
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

// ---------------------------------------------------------------------------
// Controlled TCP Proxy for Network Outage & Concurrency Fault Simulation
// ---------------------------------------------------------------------------
class ControlledTcpProxy {
  public:
    ControlledTcpProxy(const std::string &targetHost, int targetPort)
        : TargetHost_(targetHost), TargetPort_(targetPort),
          Server_(Poco::Net::SocketAddress("127.0.0.1", 0)) {
        ProxyPort_ = Server_.address().port();
        Server_.setReceiveTimeout(Poco::Timespan(200 * 1000LL));
        Thread_ = std::thread(&ControlledTcpProxy::AcceptLoop, this);
    }

    ~ControlledTcpProxy() {
        Stop();
    }

    [[nodiscard]] int Port() const noexcept {
        return ProxyPort_;
    }

    [[nodiscard]] uint64_t AcceptedConnections() const noexcept {
        return AcceptedConnections_.load();
    }

    void Disconnect() {
        Enabled_.store(false);
        std::lock_guard<std::mutex> lock(SocketsMutex_);
        for (auto &conn : Conns_) {
            if (conn->pinned.load()) continue;
            CloseConnSockets(*conn);
        }
    }

    void Restore() {
        Enabled_.store(true);
    }

    // Hold all client commands at a barrier until N distinct connections arrive.
    void SetBarrier(int targetCount) {
        std::lock_guard<std::mutex> lock(HookMutex_);
        BarrierTarget_ = targetCount;
        BarrierArrived_ = 0;
        BarrierReleased_ = false;
    }

    bool WaitForBarrier(int targetCount, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(HookMutex_);
        return HookCv_.wait_for(lock, timeout, [&]() {
            return BarrierArrived_ >= targetCount || Stop_.load();
        }) && !Stop_.load();
    }

    void ReleaseBarrier() {
        {
            std::lock_guard<std::mutex> lock(HookMutex_);
            BarrierReleased_ = true;
            BarrierTarget_ = 0;
        }
        HookCv_.notify_all();
    }

    // Inject a raw RESP error string (e.g. "-NOPERM ...\r\n") on the very next client command
    // instead of forwarding it to Redis. Keeps the TCP socket healthy and stream synchronized.
    void InjectNextError(std::string respError) {
        std::lock_guard<std::mutex> lock(HookMutex_);
        InjectedError_ = std::move(respError);
    }

    // Intercept and pin the next `count` client commands in flight (immune to Disconnect).
    void HoldNextReplies(int count) {
        std::lock_guard<std::mutex> lock(HookMutex_);
        HoldRepliesRemaining_ += count;
    }

    bool WaitForHeldReplies(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(HookMutex_);
        return HookCv_.wait_for(lock, timeout, [&]() {
            return HeldConns_.size() >= count || Stop_.load();
        }) && !Stop_.load();
    }

    void ReleaseHeldReply(size_t index) {
        std::shared_ptr<ConnState> conn;
        {
            std::lock_guard<std::mutex> lock(HookMutex_);
            if (index >= HeldConns_.size()) return;
            conn = HeldConns_[index];
            conn->action = ConnAction::Release;
        }
        HookCv_.notify_all();
    }

    void FailHeldReply(size_t index) {
        std::shared_ptr<ConnState> conn;
        {
            std::lock_guard<std::mutex> lock(HookMutex_);
            if (index >= HeldConns_.size()) return;
            conn = HeldConns_[index];
            conn->action = ConnAction::Fail;
        }
        CloseConnSockets(*conn);
        HookCv_.notify_all();
    }

    bool WaitForHeldClientClosed(size_t index, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(HookMutex_);
        if (index >= HeldConns_.size()) return false;
        auto conn = HeldConns_[index];
        return HookCv_.wait_for(lock, timeout, [&]() {
            return conn->clientClosed.load() || Stop_.load();
        }) && conn->clientClosed.load();
    }

    // Intercept and pin the next connection's SELECT command during ConnectRedisClient().
    void HoldNextSelect() {
        std::lock_guard<std::mutex> lock(HookMutex_);
        HoldNextSelect_ = true;
        ConnectingConn_.reset();
    }

    bool WaitForConnectingHeld(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(HookMutex_);
        return HookCv_.wait_for(lock, timeout, [&]() {
            return ConnectingConn_ != nullptr || Stop_.load();
        }) && !Stop_.load();
    }

    void ReleaseConnectingHeld() {
        {
            std::lock_guard<std::mutex> lock(HookMutex_);
            if (!ConnectingConn_) return;
            ConnectingConn_->action = ConnAction::Release;
        }
        HookCv_.notify_all();
    }

    bool WaitForConnectingClientClosed(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(HookMutex_);
        if (!ConnectingConn_) return false;
        auto conn = ConnectingConn_;
        return HookCv_.wait_for(lock, timeout, [&]() {
            return conn->clientClosed.load() || Stop_.load();
        }) && conn->clientClosed.load();
    }

    void Stop() {
        Stop_.store(true);
        Enabled_.store(false);
        {
            std::lock_guard<std::mutex> lock(SocketsMutex_);
            for (auto &conn : Conns_) {
                conn->pinned.store(false);
                CloseConnSockets(*conn);
            }
            Conns_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(HookMutex_);
            for (auto &c : HeldConns_) c->action = ConnAction::Fail;
            if (ConnectingConn_) ConnectingConn_->action = ConnAction::Fail;
        }
        HookCv_.notify_all();
        try { Server_.close(); } catch (...) {}
        if (Thread_.joinable()) {
            Thread_.join();
        }
        std::vector<std::thread> toJoin;
        {
            std::lock_guard<std::mutex> lock(SocketsMutex_);
            toJoin = std::move(ForwardThreads_);
            ForwardThreads_.clear();
        }
        for (auto &t : toJoin) {
            if (t.joinable()) t.join();
        }
    }

  private:
    enum class ConnAction { Hold, Release, Fail };

    struct ConnState {
        Poco::Net::StreamSocket clientSock;
        Poco::Net::StreamSocket redisSock;
        std::atomic<bool> pinned{false};
        std::atomic<bool> clientClosed{false};
        ConnAction action{ConnAction::Release};
    };

    static void CloseConnSockets(ConnState &conn) {
        try { conn.clientSock.shutdown(); } catch (...) {}
        try { conn.clientSock.close(); } catch (...) {}
        try { conn.redisSock.shutdown(); } catch (...) {}
        try { conn.redisSock.close(); } catch (...) {}
    }

    void Forward(std::shared_ptr<ConnState> conn, bool clientToRedis) {
        Poco::Net::StreamSocket src = clientToRedis ? conn->clientSock : conn->redisSock;
        Poco::Net::StreamSocket dst = clientToRedis ? conn->redisSock : conn->clientSock;
        char buf[8192];
        src.setReceiveTimeout(Poco::Timespan(200 * 1000LL));
        while (!Stop_.load() && (Enabled_.load() || conn->pinned.load())) {
            try {
                int n = src.receiveBytes(buf, sizeof(buf));
                if (n <= 0) {
                    if (clientToRedis) {
                        conn->clientClosed.store(true);
                        HookCv_.notify_all();
                    }
                    break;
                }

                if (clientToRedis) {
                    // 1. Optional synthetic RESP error injection
                    std::string errToSend;
                    {
                        std::lock_guard<std::mutex> lock(HookMutex_);
                        if (!InjectedError_.empty()) {
                            errToSend = std::move(InjectedError_);
                            InjectedError_.clear();
                        }
                    }
                    if (!errToSend.empty()) {
                        src.sendBytes(errToSend.data(), static_cast<int>(errToSend.size()));
                        continue;
                    }

                    // 2. Optional SELECT handshake interception during ConnectRedisClient
                    bool waitAction = false;
                    {
                        std::unique_lock<std::mutex> lock(HookMutex_);
                        const std::string_view payload(buf, static_cast<size_t>(n));
                        if (HoldNextSelect_ && payload.find("SELECT") != std::string_view::npos) {
                            HoldNextSelect_ = false;
                            conn->pinned.store(true);
                            conn->action = ConnAction::Hold;
                            ConnectingConn_ = conn;
                            waitAction = true;
                            HookCv_.notify_all();
                        } else if (HoldRepliesRemaining_ > 0) {
                            --HoldRepliesRemaining_;
                            conn->pinned.store(true);
                            conn->action = ConnAction::Hold;
                            HeldConns_.push_back(conn);
                            waitAction = true;
                            HookCv_.notify_all();
                        }
                        if (waitAction) {
                            HookCv_.wait(lock, [&]() {
                                return conn->action != ConnAction::Hold || Stop_.load();
                            });
                            if (Stop_.load() || conn->action == ConnAction::Fail) break;
                        }
                    }

                    // 3. Optional multi-connection warmup barrier
                    {
                        std::unique_lock<std::mutex> lock(HookMutex_);
                        if (BarrierTarget_ > 0 && !BarrierReleased_) {
                            ++BarrierArrived_;
                            HookCv_.notify_all();
                            HookCv_.wait_for(lock, std::chrono::seconds(10), [&]() {
                                return BarrierReleased_ || Stop_.load();
                            });
                            if (Stop_.load()) break;
                        }
                    }
                }

                dst.sendBytes(buf, n);
            } catch (const Poco::TimeoutException &) {
                continue;
            } catch (...) {
                if (clientToRedis) {
                    conn->clientClosed.store(true);
                    HookCv_.notify_all();
                }
                break;
            }
        }
        if (clientToRedis) {
            conn->clientClosed.store(true);
            HookCv_.notify_all();
        }
        CloseConnSockets(*conn);
    }

    void AcceptLoop() {
        while (!Stop_.load()) {
            try {
                Poco::Net::StreamSocket clientSock = Server_.acceptConnection();
                ++AcceptedConnections_;
                if (!Enabled_.load()) {
                    try { clientSock.shutdown(); } catch (...) {}
                    try { clientSock.close(); } catch (...) {}
                    continue;
                }
                try {
                    Poco::Net::StreamSocket redisSock(
                        Poco::Net::SocketAddress(TargetHost_, static_cast<Poco::UInt16>(TargetPort_)));
                    auto conn = std::make_shared<ConnState>();
                    conn->clientSock = clientSock;
                    conn->redisSock = redisSock;
                    std::lock_guard<std::mutex> lock(SocketsMutex_);
                    Conns_.push_back(conn);
                    ForwardThreads_.emplace_back(&ControlledTcpProxy::Forward, this, conn, true);
                    ForwardThreads_.emplace_back(&ControlledTcpProxy::Forward, this, conn, false);
                } catch (...) {
                    try { clientSock.shutdown(); } catch (...) {}
                    try { clientSock.close(); } catch (...) {}
                }
            } catch (const Poco::TimeoutException &) {
            } catch (...) {}
        }
    }

    std::string TargetHost_;
    int TargetPort_;
    Poco::Net::ServerSocket Server_;
    int ProxyPort_ = 0;
    std::atomic<bool> Enabled_{true};
    std::atomic<bool> Stop_{false};
    std::atomic<uint64_t> AcceptedConnections_{0};
    std::thread Thread_;
    std::mutex SocketsMutex_;
    std::vector<std::shared_ptr<ConnState>> Conns_;
    std::vector<std::thread> ForwardThreads_;

    std::mutex HookMutex_;
    std::condition_variable HookCv_;
    int BarrierTarget_ = 0;
    int BarrierArrived_ = 0;
    bool BarrierReleased_ = false;
    std::string InjectedError_;
    int HoldRepliesRemaining_ = 0;
    std::vector<std::shared_ptr<ConnState>> HeldConns_;
    bool HoldNextSelect_ = false;
    std::shared_ptr<ConnState> ConnectingConn_;
};

// ---------------------------------------------------------------------------
// Custom Poco Channel to Capture and Assert Log Messages
// ---------------------------------------------------------------------------
class TestLogCaptureChannel : public Poco::Channel {
  public:
    explicit TestLogCaptureChannel(Poco::Channel *passthrough = nullptr)
        : Passthrough_(passthrough, true) {}

    void log(const Poco::Message &msg) override {
        {
            std::lock_guard<std::mutex> lock(Mutex_);
            Messages_.push_back(msg);
        }
        if (Passthrough_) {
            try { Passthrough_->log(msg); } catch (...) {}
        }
    }

    [[nodiscard]] bool HasErrorContaining(const std::string &sub) const {
        std::lock_guard<std::mutex> lock(Mutex_);
        for (const auto &m : Messages_) {
            if (m.getPriority() <= Poco::Message::PRIO_ERROR &&
                m.getText().find(sub) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(Mutex_);
        Messages_.clear();
    }

    [[nodiscard]] Poco::Channel *Passthrough() noexcept {
        return Passthrough_.get();
    }

  private:
    mutable std::mutex Mutex_;
    std::vector<Poco::Message> Messages_;
    Poco::AutoPtr<Poco::Channel> Passthrough_;
};

// ---------------------------------------------------------------------------
// Suite 5 - Test E: Live Redis Outage & Recovery (Finding 5A)
// ---------------------------------------------------------------------------
static void test_live_redis_outage_and_recovery(int redisPort) {
    std::cout << "--- Suite 5 - Test E: Live Redis Outage & Recovery (Port " << redisPort << ") ---" << std::endl;
    Poco::Logger &log = Poco::Logger::get("TestLiveOutage");
    Poco::AutoPtr<Poco::Channel> origChannel = log.getChannel();
    Poco::AutoPtr<TestLogCaptureChannel> logCapture = new TestLogCaptureChannel(origChannel.get());
    log.setChannel(logCapture);
    log.setLevel(Poco::Message::PRIO_DEBUG);

    const std::string host = GetTestRedisHost();

    // Start controlled TCP proxy pointing to the live test Redis
    ControlledTcpProxy proxy(host, redisPort);
    const int proxyPort = proxy.Port();
    TEST_ASSERT(proxyPort > 0, "Proxy port must be bound successfully");

    // Configure both independent cache instances through the local controlled proxy.
    // Use short TTL (10s) to test bounded-staleness window and small pool limits.
    RedisCacheConfig cfg;
    cfg.enabled = true;
    cfg.host = "127.0.0.1";
    cfg.port = proxyPort;
    cfg.cacheTtlSec = 10;
    cfg.timeoutMs = 1000;
    cfg.maxTotalConnections = 4;
    cfg.maxIdleConnections = 2;

    // Keep the EXACT same cache instances throughout all stages of the test (requirement)
    RedisCache cache1(cfg, log);
    RedisCache cache2(cfg, log);

    const std::string key = MakeCacheKey("inventory", "outage_recovery_key");
    const std::string verKey = MakeVersionKey(key);
    const std::string recoveryProbe = MakeCacheKey("inventory", "outage_recovery_probe");
    const std::string verRecoveryProbe = MakeVersionKey(recoveryProbe);

    Poco::Redis::Client directRc(host, redisPort);
    auto cleanup = [&]() {
        try {
            Poco::Redis::Array cmd;
            cmd << "DEL" << key << verKey << recoveryProbe << verRecoveryProbe;
            directRc.execute<Poco::Int64>(cmd);
        } catch (...) {}
    };

    cleanup();

    Poco::Data::SQLite::Connector::registerConnector();
    Poco::Data::Session dbSession("SQLite", ":memory:");
    try {
        dbSession << "DROP TABLE IF EXISTS outage_test", Poco::Data::Keywords::now;
    } catch (...) {}
    dbSession << "CREATE TABLE IF NOT EXISTS outage_test (id INTEGER PRIMARY KEY, version INTEGER)", Poco::Data::Keywords::now;

    // =======================================================================
    // Stage 1: Warm the pool and verify cache hits across independent instances
    // =======================================================================
    {
        bool loaderCalled = false;
        auto loaderV1 = [&]() -> std::optional<RedisCache::Entry> {
            loaderCalled = true;
            return RedisCache::Entry{R"({"name":"device-outage","version":"100"})", "100"};
        };

        // Cache miss on cache1 triggers loader and populates Redis
        auto missOutcome = cache1.Get(key, loaderV1);
        TEST_ASSERT(loaderCalled, "Stage 1: Initial miss on cache1 must invoke loader");
        TEST_ASSERT(missOutcome.result == RedisCache::Result::Miss,
                    "Stage 1: cache1 must report Result::Miss (populated)");
        TEST_ASSERT(missOutcome.entry.has_value() && missOutcome.entry->version == "100",
                    "Stage 1: Populated entry must have version 100");
        TEST_ASSERT(cache1.IsRedisHealthy(), "Stage 1: cache1 must report healthy");

        // Direct cache hit on cache1
        auto hit1 = cache1.Get(key);
        TEST_ASSERT(hit1.has_value() && hit1->version == "100",
                    "Stage 1: cache1 must return cache hit with version 100");

        // Shared cache hit on independent cache2
        auto hit2 = cache2.Get(key);
        TEST_ASSERT(hit2.has_value() && hit2->version == "100",
                    "Stage 1: cache2 must return shared cache hit with version 100");
        TEST_ASSERT(cache2.IsRedisHealthy(), "Stage 1: cache2 must report healthy");
    }

    // =======================================================================
    // Stage 2: Disconnect Redis and verify authoritative-loader fallback
    // =======================================================================
    {
        proxy.Disconnect();

        // cache1 encounters severed socket, disposes stale pooled client, falls back to loader
        bool fallbackLoader1Called = false;
        auto fbLoader1 = [&]() -> std::optional<RedisCache::Entry> {
            fallbackLoader1Called = true;
            return RedisCache::Entry{R"({"name":"device-outage","version":"100"})", "100"};
        };
        auto fbOutcome1 = cache1.Get(key, fbLoader1);
        TEST_ASSERT(fallbackLoader1Called, "Stage 2: cache1 authoritative loader must be invoked during outage");
        TEST_ASSERT(fbOutcome1.result == RedisCache::Result::Fallback,
                    "Stage 2: cache1 must return Result::Fallback during outage");
        TEST_ASSERT(fbOutcome1.entry.has_value() && fbOutcome1.entry->version == "100",
                    "Stage 2: cache1 must return authoritative fallback entry");
        TEST_ASSERT(!cache1.IsRedisHealthy(), "Stage 2: cache1 must be marked unhealthy during outage");

        // cache2 encounters severed socket, disposes stale pooled client, falls back to loader
        bool fallbackLoader2Called = false;
        auto fbLoader2 = [&]() -> std::optional<RedisCache::Entry> {
            fallbackLoader2Called = true;
            return RedisCache::Entry{R"({"name":"device-outage","version":"100"})", "100"};
        };
        auto fbOutcome2 = cache2.Get(key, fbLoader2);
        TEST_ASSERT(fallbackLoader2Called, "Stage 2: cache2 authoritative loader must be invoked during outage");
        TEST_ASSERT(fbOutcome2.result == RedisCache::Result::Fallback,
                    "Stage 2: cache2 must return Result::Fallback during outage");
        TEST_ASSERT(fbOutcome2.entry.has_value() && fbOutcome2.entry->version == "100",
                    "Stage 2: cache2 must return authoritative fallback entry");
        TEST_ASSERT(!cache2.IsRedisHealthy(), "Stage 2: cache2 must be marked unhealthy during outage");

        // Verify fail-fast during backoff window: direct Get() immediately returns nullopt without blocking
        auto fastFail1 = cache1.Get(key);
        TEST_ASSERT(!fastFail1.has_value(), "Stage 2: Direct Get() during backoff must fail fast");
    }

    // =======================================================================
    // Stage 3: Commit DB mutation during outage; verify commit, ERROR log, and counter increment
    // =======================================================================
    {
        logCapture->Clear();
        const uint64_t failuresBefore = GetRedisInvalidationFailuresTotal();
        {
            DbTransaction tx(dbSession, log);

            // Execute an actual DB mutation (INSERT) inside the transaction before commit
            dbSession << "INSERT INTO outage_test (id, version) VALUES (1, 200)",
                Poco::Data::Keywords::now;

            bool regOk = RegisterCacheInvalidationAfterCommit(tx, cache1, {{key, "200"}});
            TEST_ASSERT(regOk, "Stage 3: RegisterCacheInvalidationAfterCommit must return true");

            // Transaction commit MUST succeed even though post-commit invalidation fails
            bool commitOk = tx.Commit();
            TEST_ASSERT(commitOk, "Stage 3: DB mutation tx.Commit() must succeed despite Redis outage");
            TEST_ASSERT(!tx.HasFailed(), "Stage 3: Transaction must NOT be marked failed");
        }

        // Verify that the actual DB mutation was committed and persisted to SQLite table
        int persistedVersion = 0;
        dbSession << "SELECT version FROM outage_test WHERE id = 1",
            Poco::Data::Keywords::into(persistedVersion),
            Poco::Data::Keywords::now;
        TEST_ASSERT(persistedVersion == 200, "Stage 3: Actual DB mutation must be committed and visible in table");

        // Verify that post-commit invalidation failure emitted the required ERROR log
        TEST_ASSERT(logCapture->HasErrorContaining("RedisCache: Invalidation failed for key '" + key + "' version '200'"),
                    "Stage 3: Must emit required ERROR log for invalidation failure during outage");

        // Verify telemetry metric increment
        const uint64_t failuresAfter = GetRedisInvalidationFailuresTotal();
        TEST_ASSERT(failuresAfter == failuresBefore + 1,
                    "Stage 3: owprov_redis_invalidation_failures_total must increment by 1");
        TEST_ASSERT(cache1.InvalidationFailures() == failuresAfter,
                    "Stage 3: cache1.InvalidationFailures() must match global metric");
    }

    // =======================================================================
    // Stage 4: Restore connectivity
    // =======================================================================
    const auto connectionsBeforeRecovery = proxy.AcceptedConnections();
    {
        proxy.Restore();
    }

    // =======================================================================
    // Stage 5: Verify recovery after configured backoff & disposal of stale pooled connections
    // =======================================================================
    {
        // Wait 5200ms to exceed the 5000ms RECONNECT_BACKOFF_MS window
        std::this_thread::sleep_for(std::chrono::milliseconds(5200));

        // Immediately test that the SAME cache1 instance recovers right after backoff
        // on a separate probe key, before waiting for the stale target key's TTL.
        bool recovered = cache1.Set(
            recoveryProbe,
            RedisCache::Entry{R"({"probe":"ok","version":"1"})", "1"},
            10);

        TEST_ASSERT(recovered,
                    "Stage 5: Same RedisCache instance must reconnect and succeed immediately after backoff");
        TEST_ASSERT(cache1.IsRedisHealthy(),
                    "Stage 5: Cache must be healthy after post-backoff reconnection");

        // Verify that recovery established a fresh TCP connection after stale pooled socket failure
        TEST_ASSERT(
            proxy.AcceptedConnections() > connectionsBeforeRecovery,
            "Recovery must establish a fresh TCP connection after stale pooled socket failure");

        // Verify cache hit on the probe key via independent cache2 as well
        auto probeHit2 = cache2.Get(recoveryProbe);
        TEST_ASSERT(probeHit2.has_value() && probeHit2->version == "1",
                    "Stage 5: cache2 must also read recovery probe key");
        TEST_ASSERT(cache2.IsRedisHealthy(),
                    "Stage 5: cache2 must be healthy after post-backoff reconnection");

        try {
            Poco::Redis::Array cmd;
            cmd << "DEL" << recoveryProbe << verRecoveryProbe;
            directRc.execute<Poco::Int64>(cmd);
        } catch (...) {}

        // Respect short-TTL (10s) bounded-staleness policy:
        // Poll EXISTS until the stale cache entry is actually gone, with a bounded timeout.
        bool expired = false;
        for (int i = 0; i < 120; ++i) {
            Poco::Redis::Array existsCmd;
            existsCmd << "EXISTS" << key;

            if (directRc.execute<Poco::Int64>(existsCmd) == 0) {
                expired = true;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        TEST_ASSERT(
            expired,
            "Stale cache entry must expire within bounded TTL window");
    }

    // =======================================================================
    // Stage 6: Verify latest authoritative version populates miss & shared hits resume
    // =======================================================================
    {
        // Stale pooled connections were already disposed upon failure in Stage 2.
        // cache1 now acquires a fresh connection through restored proxy.
        bool loaderV2Called = false;
        auto loaderV2 = [&]() -> std::optional<RedisCache::Entry> {
            loaderV2Called = true;
            return RedisCache::Entry{R"({"name":"device-outage","version":"200"})", "200"};
        };

        // Cache miss on cache1 triggers loaderV2 and populates restored Redis
        auto recOutcome = cache1.Get(key, loaderV2);
        TEST_ASSERT(loaderV2Called, "Stage 6: loaderV2 must be invoked on cache miss");
        TEST_ASSERT(recOutcome.result == RedisCache::Result::Miss,
                    "Stage 6: cache1 must report Result::Miss (populated) after recovery");
        TEST_ASSERT(recOutcome.entry.has_value() && recOutcome.entry->version == "200",
                    "Stage 6: Populated entry must have latest version 200");
        TEST_ASSERT(cache1.IsRedisHealthy(), "Stage 6: cache1 must report healthy after recovery");

        // Independent instance cache2 reads the updated entry from restored Redis
        auto hitCache2 = cache2.Get(key);
        TEST_ASSERT(hitCache2.has_value() && hitCache2->version == "200",
                    "Stage 6: cache2 must return shared cache hit with version 200");
        TEST_ASSERT(cache2.IsRedisHealthy(), "Stage 6: cache2 must report healthy after recovery");

        // Subsequent reads on both instances continue to hit version 200
        auto subsequentHit1 = cache1.Get(key);
        TEST_ASSERT(subsequentHit1.has_value() && subsequentHit1->version == "200",
                    "Stage 6: Subsequent cache1 hit must return version 200");

        auto subsequentHit2 = cache2.Get(key);
        TEST_ASSERT(subsequentHit2.has_value() && subsequentHit2->version == "200",
                    "Stage 6: Subsequent cache2 hit must return version 200");
    }

    try {
        dbSession << "DROP TABLE IF EXISTS outage_test", Poco::Data::Keywords::now;
    } catch (...) {}

    cleanup();
    proxy.Stop();
    log.setChannel(origChannel);
    std::cout << "  PASS: test_live_redis_outage_and_recovery" << std::endl;
}

// ---------------------------------------------------------------------------
// Suite 5 - Test F: Finding 5B — Pool Generation Invalidation & Stale Socket Fencing
//
// Verifies all Finding 5B requirements deterministically:
//   1. Valid Redis server errors (WRONGTYPE, NOPERM) do NOT invalidate the pool or start backoff.
//   2. Warming multiple concurrent pooled sockets via proxy barrier, then disconnecting,
//      invalidates the generation and discards all idle stale sockets so recovery finishes
//      within a single 5s backoff window across all subsequent requests.
//   3. Older-generation leased connections kept alive across invalidation and recovery:
//      (a) healthy release disconnects the obsolete socket instead of re-entering IdleClients;
//      (b) late transport failure does NOT purge the recovered pool or reset backoff.
//   4. Older-generation connecting clients suspended in ConnectRedisClient() across a
//      generation bump do NOT mark Redis healthy during backoff and are discarded on release.
// ---------------------------------------------------------------------------
static void test_stale_pool_purge_on_transport_failure(int redisPort) {
    std::cout << "--- Suite 5 - Test F: Finding 5B Stale Pool & Generation Fencing (Port " << redisPort << ") ---" << std::endl;
    Poco::Logger &log = Poco::Logger::get("Test5B");
    const std::string host = GetTestRedisHost();

    auto cleanup5b = [&](int db) {
        try {
            Poco::Redis::Client rc(host, redisPort);
            if (db != 0) {
                Poco::Redis::Array sel; sel << "SELECT" << std::to_string(db);
                rc.execute<std::string>(sel);
            }
            Poco::Redis::Array delCmd;
            delCmd << "DEL";
            for (const char *name : {"5b_wrongtype_key", "5b_healthcheck", "5b_held_release",
                                     "5b_held_fail", "5b_trigger_fail", "5b_post_late",
                                     "5b_conn_warm", "5b_conn_active", "5b_connecting", "5b_conn_rec",
                                     "5b_waiter_block", "5b_waiter"}) {
                const auto k = MakeCacheKey("inventory", name);
                delCmd << k << MakeVersionKey(k);
            }
            for (int i = 0; i < 6; ++i) {
                for (const char *pfx : {"5b_p1_{}", "5b_warm_{}", "5b_rec_{}"}) {
                    const auto k = MakeCacheKey("inventory", fmt::format(pfx, i));
                    delCmd << k << MakeVersionKey(k);
                }
            }
            rc.execute<Poco::Int64>(delCmd);
        } catch (...) {}
    };
    cleanup5b(0);
    cleanup5b(1);

    // =========================================================================
    // Part 1: Valid Redis server errors (WRONGTYPE & NOPERM) must NOT purge
    //         the idle pool or trigger reconnect backoff.
    // =========================================================================
    {
        ControlledTcpProxy proxy(host, redisPort);
        TEST_ASSERT(proxy.Port() > 0, "Part 1: Proxy must bind");

        Poco::Redis::Client directRc(host, redisPort);
        const std::string wrongTypeKey = MakeCacheKey("inventory", "5b_wrongtype_key");
        const std::string wrongTypeVerKey = MakeVersionKey(wrongTypeKey);
        const std::string healthKey = MakeCacheKey("inventory", "5b_healthcheck");
        try {
            Poco::Redis::Array delCmd; delCmd << "DEL" << wrongTypeKey << wrongTypeVerKey << healthKey << MakeVersionKey(healthKey);
            directRc.execute<Poco::Int64>(delCmd);
            Poco::Redis::Array lpushCmd; lpushCmd << "LPUSH" << wrongTypeKey << "value1";
            directRc.execute<Poco::Int64>(lpushCmd);
        } catch (...) {}

        RedisCacheConfig cfgWT;
        cfgWT.enabled = true;
        cfgWT.host = "127.0.0.1";
        cfgWT.port = proxy.Port();
        cfgWT.timeoutMs = 2000;
        cfgWT.maxTotalConnections = 4;
        cfgWT.maxIdleConnections = 3;
        cfgWT.cacheTtlSec = 30;

        RedisCache wtCache(cfgWT, log);

        // Warm 3 idle sockets deterministically so after 2 server-error commands,
        // an untouched idle socket must still remain in the pool (no pool purge).
        {
            proxy.SetBarrier(3);
            std::vector<std::thread> t;
            for (int i = 0; i < 3; ++i) {
                t.emplace_back([&, i]() {
                    bool ok = wtCache.Set(MakeCacheKey("inventory", fmt::format("5b_p1_{}", i)), {"d", "1"});
                    TEST_ASSERT(ok, "Part 1: Warmup Set must succeed");
                });
            }
            TEST_ASSERT(proxy.WaitForBarrier(3, std::chrono::seconds(10)), "Part 1: 3 sockets must reach barrier");
            proxy.ReleaseBarrier();
            for (auto &th : t) th.join();
        }
        const uint64_t connsBeforeErrors = proxy.AcceptedConnections();
        TEST_ASSERT(connsBeforeErrors == 3, "Part 1: Exactly 3 connections warmed");
        TEST_ASSERT(wtCache.IsRedisHealthy(), "Part 1: Cache healthy after warmup");

        // 1a. Live WRONGTYPE reply from Redis
        auto wtResult = wtCache.Get(wrongTypeKey);
        TEST_ASSERT(!wtResult.has_value(), "Part 1: WRONGTYPE GET returns nullopt");
        TEST_ASSERT(wtCache.IsRedisHealthy(), "Part 1: WRONGTYPE must NOT trigger backoff");

        // 1b. Non-whitelist server error (NOPERM) injected over healthy socket
        proxy.InjectNextError("-NOPERM this user has no permissions to run 'get'\r\n");
        auto npResult = wtCache.Get(healthKey);
        TEST_ASSERT(!npResult.has_value(), "Part 1: NOPERM GET returns nullopt");
        TEST_ASSERT(wtCache.IsRedisHealthy(), "Part 1: NOPERM must NOT trigger backoff");

        // Remaining idle socket must still be reused without establishing new connections
        TEST_ASSERT(wtCache.Set(healthKey, {"{\"ok\":1}", "1"}), "Part 1: Subsequent Set succeeds without backoff");
        TEST_ASSERT(proxy.AcceptedConnections() == connsBeforeErrors,
            "Part 1: Server errors must NOT purge idle pool (no reconnects opened)");

        try {
            Poco::Redis::Array delCmd; delCmd << "DEL" << wrongTypeKey << wrongTypeVerKey << healthKey << MakeVersionKey(healthKey);
            directRc.execute<Poco::Int64>(delCmd);
        } catch (...) {}
        proxy.Stop();
    }

    // =========================================================================
    // Part 2: Generation invalidation, single-backoff recovery across multiple
    //         stale idle sockets, and fencing of old-generation leased clients
    // =========================================================================
    {
        ControlledTcpProxy proxy(host, redisPort);
        TEST_ASSERT(proxy.Port() > 0, "Part 2: Proxy must bind successfully");

        RedisCacheConfig cfg;
        cfg.enabled = true;
        cfg.host = "127.0.0.1";
        cfg.port = proxy.Port();
        cfg.timeoutMs = 15000; // Kept above backoff window so held gen-1 sockets do not timeout
        cfg.maxTotalConnections = 10;
        cfg.maxIdleConnections = 6;
        cfg.cacheTtlSec = 30;

        RedisCache cache(cfg, log);

        // --- Stage 1: Deterministically warm 6 concurrent connections into IdleClients (gen 1) ---
        {
            const int N = 6;
            proxy.SetBarrier(N);
            std::vector<std::thread> workers;
            std::vector<int> ok(N, 0);
            for (int i = 0; i < N; ++i) {
                workers.emplace_back([&, i]() {
                    ok[i] = cache.Set(MakeCacheKey("inventory", fmt::format("5b_warm_{}", i)),
                                      {fmt::format("{{\"v\":{}}}", i), "1"}) ? 1 : 0;
                });
            }
            TEST_ASSERT(proxy.WaitForBarrier(N, std::chrono::seconds(10)),
                "Part 2 Stage 1: All 6 concurrent Redis Sets must enter proxy barrier");
            proxy.ReleaseBarrier();
            for (auto &w : workers) w.join();
            for (int i = 0; i < N; ++i) {
                TEST_ASSERT(ok[i], fmt::format("Part 2 Stage 1: Warmup Set {} must succeed", i));
            }
        }
        TEST_ASSERT(proxy.AcceptedConnections() == 6, "Part 2 Stage 1: Pool holds 6 gen-1 connections");
        TEST_ASSERT(cache.IsRedisHealthy(), "Part 2 Stage 1: Cache healthy after warmup");

        // --- Stage 2: Hold 2 gen-1 leased connections in flight across invalidation ---
        // Hold sequentially so HeldConns_[0] deterministically owns threadHeldRelease
        // and HeldConns_[1] deterministically owns threadHeldFail.
        proxy.HoldNextReplies(1);
        bool heldReleaseOk = false;
        std::thread threadHeldRelease([&]() {
            heldReleaseOk = cache.Set(MakeCacheKey("inventory", "5b_held_release"), {"{\"held\":0}", "1"});
        });
        TEST_ASSERT(proxy.WaitForHeldReplies(1, std::chrono::seconds(10)),
            "Part 2 Stage 2: Proxy must pin first gen-1 in-flight request (release candidate)");

        proxy.HoldNextReplies(1);
        std::optional<RedisCache::Entry> heldFailRes = RedisCache::Entry{"sentinel", "99"};
        std::thread threadHeldFail([&]() {
            heldFailRes = cache.Get(MakeCacheKey("inventory", "5b_held_fail"));
        });
        TEST_ASSERT(proxy.WaitForHeldReplies(2, std::chrono::seconds(10)),
            "Part 2 Stage 2: Proxy must pin second gen-1 in-flight request (late-failure candidate)");

        // --- Stage 3: Disconnect the 4 idle gen-1 sockets and trigger generation 1 -> 2 ---
        proxy.Disconnect();

        // Next request pops 1 of the 4 broken idle sockets, fails, invalidates gen 1 -> 2,
        // discards the other 3 stale idle sockets, and starts one 5s backoff window.
        auto triggerFail = cache.Get(MakeCacheKey("inventory", "5b_trigger_fail"));
        TEST_ASSERT(!triggerFail.has_value(), "Part 2 Stage 3: Outage Get must fail");
        TEST_ASSERT(!cache.IsRedisHealthy(), "Part 2 Stage 3: Cache unhealthy during backoff");

        // --- Stage 4: Restore connectivity, wait 1 backoff, verify all 6 calls succeed cleanly ---
        proxy.Restore();
        std::this_thread::sleep_for(std::chrono::milliseconds(5300));

        const auto t0 = std::chrono::steady_clock::now();
        int recoverySuccess = 0;
        for (int i = 0; i < 6; ++i) {
            if (cache.Set(MakeCacheKey("inventory", fmt::format("5b_rec_{}", i)),
                          {fmt::format("{{\"r\":{}}}", i), "1"})) {
                ++recoverySuccess;
            }
        }
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        TEST_ASSERT(recoverySuccess == 6,
            fmt::format("Part 2 Stage 4: All 6 Sets must succeed after single backoff (got {})", recoverySuccess));
        TEST_ASSERT(elapsedMs < 1500,
            fmt::format("Part 2 Stage 4: Recovery took {}ms (>1500ms implies stepping through stale sockets)", elapsedMs));
        TEST_ASSERT(cache.IsRedisHealthy(), "Part 2 Stage 4: Cache must be healthy at gen 2");
        const uint64_t connsAfterRecovery = proxy.AcceptedConnections();
        TEST_ASSERT(connsAfterRecovery > 6, "Part 2 Stage 4: Fresh gen-2 connection established");

        // --- Stage 5a: Healthy release of gen-1 leased connection must NOT re-enter IdleClients ---
        proxy.ReleaseHeldReply(0);
        threadHeldRelease.join();
        TEST_ASSERT(heldReleaseOk, "Part 2 Stage 5a: Released gen-1 Set succeeds");
        TEST_ASSERT(proxy.WaitForHeldClientClosed(0, std::chrono::seconds(5)),
            "Part 2 Stage 5a: Release() must disconnect obsolete gen-1 socket instead of pooling it");
        TEST_ASSERT(cache.IsRedisHealthy(), "Part 2 Stage 5a: Pool remains healthy");

        // --- Stage 5b: Late transport failure from gen-1 leased connection must NOT reset recovery ---
        proxy.FailHeldReply(1);
        threadHeldFail.join();
        TEST_ASSERT(!heldFailRes.has_value(), "Part 2 Stage 5b: Failed gen-1 Get returns nullopt");
        TEST_ASSERT(cache.IsRedisHealthy(),
            "Part 2 Stage 5b: Obsolete gen-1 late failure must NOT mark recovered gen-2 cache unhealthy");

        // Healthy gen-2 idle connection must still work immediately with no new backoff and no new connect
        const auto tLate0 = std::chrono::steady_clock::now();
        bool postLateOk = cache.Set(MakeCacheKey("inventory", "5b_post_late"), {"{\"ok\":2}", "1"});
        const auto lateMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tLate0).count();
        TEST_ASSERT(postLateOk && lateMs < 500,
            "Part 2 Stage 5b: Subsequent request must succeed immediately without backoff");
        TEST_ASSERT(proxy.AcceptedConnections() == connsAfterRecovery,
            "Part 2 Stage 5b: Late obsolete failure must NOT purge recovered gen-2 idle pool");

        proxy.Stop();
    }

    // =========================================================================
    // Part 3: Older-generation connecting client suspended inside ConnectRedisClient()
    //         across generation invalidation must NOT mark Redis healthy during
    //         backoff and must NOT enter IdleClients upon release.
    // =========================================================================
    {
        ControlledTcpProxy proxy(host, redisPort);
        TEST_ASSERT(proxy.Port() > 0, "Part 3: Proxy must bind");

        RedisCacheConfig cfg;
        cfg.enabled = true;
        cfg.host = "127.0.0.1";
        cfg.port = proxy.Port();
        cfg.db = 1; // Triggers synchronous SELECT 1 during ConnectRedisClient()
        cfg.timeoutMs = 15000;
        cfg.maxTotalConnections = 4;
        cfg.maxIdleConnections = 2;
        cfg.cacheTtlSec = 30;

        RedisCache cache(cfg, log);

        // 1. Establish 1 gen-1 socket in IdleClients
        TEST_ASSERT(cache.Set(MakeCacheKey("inventory", "5b_conn_warm"), {"{\"w\":1}", "1"}),
            "Part 3: Initial warm Set must succeed");

        // 2. Borrow that 1 idle socket so IdleClients is now empty
        proxy.HoldNextReplies(1);
        std::optional<RedisCache::Entry> activeRes;
        std::thread activeThread([&]() {
            activeRes = cache.Get(MakeCacheKey("inventory", "5b_conn_active"));
        });
        TEST_ASSERT(proxy.WaitForHeldReplies(1, std::chrono::seconds(5)),
            "Part 3: Active gen-1 socket must be held in flight");

        // 3. Start a new connecting client at gen 1 and suspend it mid-SELECT inside ConnectRedisClient()
        proxy.HoldNextSelect();
        bool connectingSetOk = true;
        std::thread connectingThread([&]() {
            connectingSetOk = cache.Set(MakeCacheKey("inventory", "5b_connecting"), {"{\"c\":1}", "1"});
        });
        TEST_ASSERT(proxy.WaitForConnectingHeld(std::chrono::seconds(5)),
            "Part 3: Connecting client must be paused inside ConnectRedisClient() SELECT 1");

        // 4. Fail the active gen-1 socket -> bumps generation 1 -> 2, enters 5s backoff
        proxy.FailHeldReply(0);
        activeThread.join();
        TEST_ASSERT(!activeRes.has_value(), "Part 3: Active socket failed");
        TEST_ASSERT(!cache.IsRedisHealthy(), "Part 3: Cache must be unhealthy in gen-2 backoff");

        // 5. While STILL inside gen-2 backoff, let the gen-1 ConnectRedisClient() finish
        proxy.ReleaseConnectingHeld();
        connectingThread.join();
        TEST_ASSERT(!connectingSetOk,
            "Part 3: Obsolete connecting client must fall back cleanly instead of poisoning gen 2");
        TEST_ASSERT(!cache.IsRedisHealthy(),
            "Part 3: Obsolete connecting client completing after invalidation must NOT mark Redis healthy");
        TEST_ASSERT(proxy.WaitForConnectingClientClosed(std::chrono::seconds(5)),
            "Part 3: Obsolete connecting socket must be closed instead of entering IdleClients");

        // 6. After backoff expires, a fresh gen-2 connection succeeds cleanly
        std::this_thread::sleep_for(std::chrono::milliseconds(5300));
        TEST_ASSERT(cache.Set(MakeCacheKey("inventory", "5b_conn_rec"), {"{\"rec\":1}", "1"}),
            "Part 3: Fresh gen-2 connection must succeed after backoff");
        TEST_ASSERT(cache.IsRedisHealthy(), "Part 3: Cache healthy after recovery");

        proxy.Stop();
    }

    // =========================================================================
    // Part 4: A request already waiting for pool capacity inside Acquire() must
    //         NOT establish or use a fresh connection if backoff begins while waiting.
    // =========================================================================
    {
        ControlledTcpProxy proxy(host, redisPort);
        TEST_ASSERT(proxy.Port() > 0, "Part 4: Proxy must bind");

        RedisCacheConfig cfg;
        cfg.enabled = true;
        cfg.host = "127.0.0.1";
        cfg.port = proxy.Port();
        cfg.timeoutMs = 5000;
        cfg.maxTotalConnections = 1;
        cfg.maxIdleConnections = 1;
        cfg.cacheTtlSec = 30;

        RedisCache cache(cfg, log);

        // Occupy the single pool slot (MaxTotal = 1) in flight
        proxy.HoldNextReplies(1);
        std::optional<RedisCache::Entry> holderRes;
        std::thread holderThread([&]() {
            holderRes = cache.Get(MakeCacheKey("inventory", "5b_waiter_block"));
        });
        TEST_ASSERT(proxy.WaitForHeldReplies(1, std::chrono::seconds(5)),
            "Part 4: Holder request must occupy the single pool connection");
        TEST_ASSERT(proxy.AcceptedConnections() == 1, "Part 4: Only 1 connection opened");

        // Launch a second request that blocks inside Pool.Acquire() waiting for capacity
        bool waiterOk = true;
        std::thread waiterThread([&]() {
            waiterOk = cache.Set(MakeCacheKey("inventory", "5b_waiter"), {"{\"w\":2}", "1"});
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // Fail the holder connection -> triggers generation bump and 5s backoff while waiter is waiting
        proxy.FailHeldReply(0);
        holderThread.join();
        waiterThread.join();

        TEST_ASSERT(!holderRes.has_value(), "Part 4: Holder failed");
        TEST_ASSERT(!waiterOk, "Part 4: Waiter must abort into fallback instead of connecting during backoff");
        TEST_ASSERT(!cache.IsRedisHealthy(), "Part 4: Waiter must NOT mark Redis healthy during backoff");
        TEST_ASSERT(proxy.AcceptedConnections() == 1,
            "Part 4: Waiter must NOT open a new TCP connection during active backoff");

        proxy.Stop();
    }

    cleanup5b(0);
    cleanup5b(1);
    std::cout << "  PASS: test_stale_pool_purge_on_transport_failure" << std::endl;
}

// ---------------------------------------------------------------------------
// Finding 1: Socket send timeout on stalled TCP peer (send-buffer saturation)
// Verifies timely failure/backoff, authoritative-loader fallback, unhealthy
// socket disposal, and pool lease release when peer stops consuming TCP data.
// ---------------------------------------------------------------------------
static void test_send_timeout_on_stalled_peer() {
    std::cout << "--- Finding 1: Socket Send Timeout on Stalled Peer ---" << std::endl;
    Poco::Logger &log = Poco::Logger::get("TestSendTimeout");

    // Bind allocates the socket fd; set SO_RCVBUF before listen() so SYN-ACK advertises small window.
    Poco::Net::ServerSocket server;
    server.bind(Poco::Net::SocketAddress("127.0.0.1", 0), true);
    server.setReceiveBufferSize(4096);
    server.listen(16);
    const int port = server.address().port();

    std::atomic<bool> stop{false};
    std::atomic<int> acceptedCount{0};
    std::mutex sockMtx;
    std::vector<Poco::Net::StreamSocket> acceptedSocks;
    std::thread acceptThread([&]() {
        while (!stop.load()) {
            try {
                if (!server.poll(Poco::Timespan(50 * 1000LL), Poco::Net::Socket::SELECT_READ)) {
                    continue;
                }
                // Accept connections but intentionally never read from them.
                Poco::Net::StreamSocket s = server.acceptConnection();
                ++acceptedCount;
                std::lock_guard<std::mutex> lk(sockMtx);
                acceptedSocks.push_back(s);
            } catch (...) {
                if (stop.load()) break;
            }
        }
    });

    // Compute a payload guaranteed to exceed kernel tcp_wmem max + peer receive buffer.
    size_t tcpWmemMax = 4 * 1024 * 1024;
    if (std::ifstream f("/proc/sys/net/ipv4/tcp_wmem"); f.is_open()) {
        size_t wMin = 0, wDef = 0, wMax = 0;
        if (f >> wMin >> wDef >> wMax && wMax > 0) {
            tcpWmemMax = wMax;
        }
    }
    const size_t payloadBytes = tcpWmemMax + 4 * 1024 * 1024;

    RedisCacheConfig cfg;
    cfg.enabled = true;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.timeoutMs = 250;
    cfg.maxTotalConnections = 1;
    cfg.maxIdleConnections = 1;

    RedisCache cache(cfg, log);
    const std::string key = MakeCacheKey("inventory", "send_timeout_item");
    std::string largePayload(payloadBytes, 'x');
    const std::string endSentinel = "__PAYLOAD_END__";
    largePayload.replace(largePayload.size() - endSentinel.size(), endSentinel.size(), endSentinel);

    // 1. Large Set() must saturate send buffer and abort mid-write via socket send timeout (~250ms)
    const auto t0 = std::chrono::steady_clock::now();
    bool setOk = cache.Set(key, {largePayload, "1"});
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    TEST_ASSERT(!setOk, "Set() must fail on stalled TCP peer");
    TEST_ASSERT(elapsedMs >= 180 && elapsedMs < 2000,
        fmt::format("Send timeout (~250ms) must trigger promptly without blocking (took {}ms)", elapsedMs));
    TEST_ASSERT(!cache.IsRedisHealthy(), "Cache must be marked unhealthy after send timeout");
    TEST_ASSERT(acceptedCount.load() == 1, "Initial attempt must have opened 1 TCP connection");

    // Verify write aborted before sending the end of the payload (proves send timeout fired, not receive timeout)
    size_t drainedBytes = 0;
    bool sawSentinel = false;
    {
        Poco::Net::StreamSocket peerSock;
        {
            std::lock_guard<std::mutex> lk(sockMtx);
            peerSock = acceptedSocks.at(0);
        }
        peerSock.setReceiveTimeout(Poco::Timespan(200 * 1000LL));
        std::vector<char> buf(65536);
        std::string tail;
        while (true) {
            try {
                int n = peerSock.receiveBytes(buf.data(), static_cast<int>(buf.size()));
                if (n <= 0) break;
                drainedBytes += static_cast<size_t>(n);
                tail.append(buf.data(), static_cast<size_t>(n));
                if (tail.size() > endSentinel.size() * 2) {
                    tail.erase(0, tail.size() - endSentinel.size() * 2);
                }
                if (tail.find(endSentinel) != std::string::npos) {
                    sawSentinel = true;
                }
            } catch (...) {
                break;
            }
        }
    }
    TEST_ASSERT(drainedBytes > 0 && drainedBytes < largePayload.size(),
        fmt::format("Client must abort mid-write on send timeout (drained={} bytes, payload={} bytes)",
                    drainedBytes, largePayload.size()));
    TEST_ASSERT(!sawSentinel,
        "End-of-payload sentinel must NOT arrive when send timeout aborts saturated write");

    // 2. Subsequent Get() must fall back promptly to the authoritative loader
    int loaderCalls = 0;
    const auto fbStart = std::chrono::steady_clock::now();
    auto out = cache.Get(key, [&]() -> std::optional<RedisCache::Entry> {
        ++loaderCalls;
        return RedisCache::Entry{"{\"fallback\":true}", "10"};
    });
    const auto fbMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - fbStart).count();

    TEST_ASSERT(loaderCalls == 1, "Authoritative loader must be invoked on fallback");
    TEST_ASSERT(out.result == RedisCache::Result::Fallback, "Must return Result::Fallback");
    TEST_ASSERT(out.entry.has_value() && out.entry->data == "{\"fallback\":true}", "Fallback data matches");
    TEST_ASSERT(fbMs < 200, fmt::format("Fallback Get() during backoff must not block (took {}ms)", fbMs));

    // 3. After backoff expires, next Set() on maxTotalConnections=1 opens a fresh socket
    //    (proving the stalled connection was disposed and the single pool lease was released).
    std::this_thread::sleep_for(std::chrono::milliseconds(5200));
    const auto t1 = std::chrono::steady_clock::now();
    bool retryOk = cache.Set(key, {largePayload, "2"});
    const auto retryMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t1).count();

    TEST_ASSERT(!retryOk, "Retry against stalled peer must also fail");
    TEST_ASSERT(retryMs >= 180 && retryMs < 2000,
        fmt::format("Retry send timeout must remain bounded (took {}ms)", retryMs));
    TEST_ASSERT(acceptedCount.load() == 2,
        fmt::format("Second attempt must open a fresh TCP connection (got {}, elapsedMs={}, retryMs={})",
                    acceptedCount.load(), elapsedMs, retryMs));

    stop.store(true);
    try { server.close(); } catch (...) {}
    acceptThread.join();
    {
        std::lock_guard<std::mutex> lk(sockMtx);
        for (auto &s : acceptedSocks) {
            try { s.close(); } catch (...) {}
        }
    }

    std::cout << "  PASS: test_send_timeout_on_stalled_peer" << std::endl;
}
#endif

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    std::cout << "=== Running RedisCache Framework Tests ===" << std::endl;

    // Suites 1 and 2 run in all environments (including GitHub Actions CI without Redis)
    test_config_and_namespaces();
    test_fallback_and_validation();
#ifdef USE_POCO_REDIS
    test_send_timeout_on_stalled_peer();
#endif

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
        test_live_redis_outage_and_recovery(redisPort);
        test_stale_pool_purge_on_transport_failure(redisPort); // Finding 5B regression
#endif
    }

    std::cout << "=== All RedisCache Tests PASSED! ===" << std::endl;
    return 0;
}
