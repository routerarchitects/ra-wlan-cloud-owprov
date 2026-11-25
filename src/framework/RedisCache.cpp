/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#include "framework/RedisCache.h"

#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "Poco/JSON/Object.h"
#include "Poco/JSON/Parser.h"
#include "Poco/JSON/Stringifier.h"
#include "Poco/Logger.h"
#include "Poco/String.h"
#include "fmt/format.h"

#include "framework/MicroServiceFuncs.h"

#ifdef USE_POCO_REDIS
#include "Poco/Redis/Array.h"
#include "Poco/Redis/Client.h"
#include "Poco/Redis/Command.h"
#include "Poco/Redis/Exception.h"
#include "Poco/Redis/Type.h"
#include "Poco/Timespan.h"
#endif

namespace OpenWifi {

// Exported metric counter for Redis cache invalidation failures (Spec §6.3.1).
std::atomic<uint64_t> owprov_redis_invalidation_failures_total{0};

namespace {

// ============================================================================
// Namespace Prefixes & Policy Constants
// ============================================================================

// Dedicated, collision-free key prefixes.
// Using separate namespaces ("api-cache:" and "api-ver:") guarantees that cache keys
// and version-marker keys never collide, even when resource IDs contain colons (e.g.
// MAC addresses, UUIDs, composite paths, or IDs ending in ":ver").
constexpr const char *CACHE_KEY_PREFIX = "api-cache:";
constexpr const char *VER_KEY_PREFIX   = "api-ver:";

// Wire schema version tag. Ensures forward-compatibility and allows detecting
// incompatible cached entries if the serialization wire format evolves.
constexpr const char *WIRE_FMT_TAG     = "v1";

// Reconnection backoff interval (5 seconds).
// When a Redis command or connection fails, subsequent requests bypass Redis immediately
// for this duration without blocking on socket connect/read timeouts. This fail-fast
// mechanism protects worker thread pools from stalling during an active Redis outage.
constexpr int64_t RECONNECT_BACKOFF_MS = 5000;

// Bounded-staleness TTL policy window for normal API data (Specification §6.3.1).
// Cache entries are clamped to this window [10s, 60s]. If Redis invalidation fails
// after a PostgreSQL commit, stale entries expire quickly via this short TTL fallback.
constexpr int MIN_CACHE_TTL_SEC        = 10;
constexpr int MAX_CACHE_TTL_SEC        = 60;

// Version-marker retention policy (Specification §6.2, §6.3.1).
// Normal API data entries are stored with short TTLs [10s, 60s].
// Version markers ("api-ver:") are stored WITHOUT expiration (TTL == -1).
// Because OWPROV's DbTransaction defines DEFAULT_STATEMENT_TIMEOUT_MS = 0 (statement
// timeouts are disabled by default), the system does not enforce a global upper bound
// on in-flight database read durations. Therefore, version markers are kept strictly
// non-expiring to provide a mathematical guarantee against stale repopulation by arbitrarily
// delayed readers and writers.

} // anonymous namespace

// ============================================================================
// Key & Version Validation Helpers
// ============================================================================

// Validates that str is a strictly positive, decimal uint64 version string.
// Rejects: empty strings, strings > 20 digits, signs ('+', '-'), whitespace,
// non-digits, zero ("0", "00"), and values overflowing 64-bit unsigned integer.
bool IsValidVersion(std::string_view str) {
    if (str.empty() || str.size() > 20) return false;
    for (char c : str) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    uint64_t val = 0;
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), val);
    return (ec == std::errc{} && ptr == str.data() + str.size() && val > 0);
}

namespace {
// Validates the resource type portion of a key before canonicalization (e.g. "Inventory", "venue").
// Allows alphanumeric characters, underscores, hyphens, and dots up to 64 characters.
bool IsValidResourceType(std::string_view str) {
    if (str.empty() || str.size() > 64) return false;
    for (char c : str) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

// Validates that the resource type is already canonical (strictly lowercase alphanumeric,
// underscores, hyphens, and dots). Rejects uppercase characters to enforce one canonical
// key representation in Redis across all callers.
bool IsValidCanonicalResourceType(std::string_view str) {
    if (str.empty() || str.size() > 64) return false;
    for (char c : str) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

// Validates the resource identifier portion of a key (e.g. UUID, MAC, composite key).
// Allows printable non-whitespace ASCII characters up to 512 characters.
bool IsValidResourceId(std::string_view str) {
    if (str.empty() || str.size() > 512) return false;
    for (char c : str) {
        if (static_cast<unsigned char>(c) <= 32 || static_cast<unsigned char>(c) >= 127) return false;
    }
    return true;
}
} // anonymous namespace

// Checks whether Key conforms to the valid application cache namespace "api-cache:<type>:<id>".
// The resource type segment must be in canonical lowercase representation.
bool IsCacheableKey(std::string_view key) {
    constexpr std::string_view prefix = CACHE_KEY_PREFIX;
    if (key.size() <= prefix.size() || key.rfind(prefix, 0) != 0) return false;
    std::string_view rest = key.substr(prefix.size());
    auto sep = rest.find(':');
    if (sep == std::string_view::npos || sep == 0 || sep + 1 >= rest.size()) return false;
    return IsValidCanonicalResourceType(rest.substr(0, sep)) && IsValidResourceId(rest.substr(sep + 1));
}

namespace {

#ifdef USE_POCO_REDIS
// Clamps normal cache data TTL to the documented 10-60 second bounded-staleness window policy.
int ClampTtl(int requestedTtl, int configuredTtl) {
    int ttl = (requestedTtl > 0) ? requestedTtl : configuredTtl;
    if (ttl < MIN_CACHE_TTL_SEC) return MIN_CACHE_TTL_SEC;
    if (ttl > MAX_CACHE_TTL_SEC) return MAX_CACHE_TTL_SEC;
    return ttl;
}

// ============================================================================
// Atomic Lua Scripts for Version-Aware Cache Operations
// ============================================================================

// Shared Lua functions for validating and comparing 64-bit decimal version strings.
// Because Lua 5.1 numbers are double-precision floats (lossy above 2^53), version
// strings are compared using numeric length and character-by-character ordering,
// guaranteeing exact 64-bit unsigned comparison without precision loss.
#define LUA_UINT64_HELPERS \
"local function is_valid_uint64(v)\n" \
"    if type(v) ~= 'string' and type(v) ~= 'number' then return false end\n" \
"    local s = tostring(v)\n" \
"    if string.match(s, '^%d+$') == nil then return false end\n" \
"    local stripped = string.match(s, '^0*(%d+)$')\n" \
"    if not stripped or stripped == '0' or #stripped > 20 then return false end\n" \
"    if #stripped == 20 and stripped > '18446744073709551615' then return false end\n" \
"    return true\n" \
"end\n" \
"local function cmp_uint64(s1, s2)\n" \
"    local n1 = string.match(tostring(s1), '^0*(%d+)$') or '0'\n" \
"    local n2 = string.match(tostring(s2), '^0*(%d+)$') or '0'\n" \
"    if #n1 ~= #n2 then return (#n1 > #n2) and 1 or -1 end\n" \
"    if n1 > n2 then return 1 end\n" \
"    if n1 < n2 then return -1 end\n" \
"    return 0\n" \
"end\n"

// Atomic Lua script for version-aware cache invalidation (Specification §6.2, §6.3).
// Atomically deletes a stale cached entry and records the committed version marker.
// All reads, comparisons, deletion, and marker update happen in a single EVAL call,
// preventing races between concurrent OWPROV instances sharing the same Redis.
//
// Arguments:
//   KEYS[1] : Cache key (e.g. api-cache:inventory:123)
//   KEYS[2] : Version marker key (e.g. api-ver:inventory:123)
//   ARGV[1] : Committed version decimal string
//
// Return codes:
//   0 : Validation failure (invalid committed version).
//   1 : Success — either deleted the stale entry and recorded the marker, or safe no-op.
//   2 : Safe no-op — cached entry or marker is already newer; nothing was changed.
//       (Idempotent: repeated calls with the same version also return 1.)
//
// Rules:
//   1. Validate committed version — reject if not a strictly positive uint64.
//   2. Check existing committed marker:
//      - If existing marker > committed (delayed invalidation):
//        Marker is already newer; preserve marker unchanged (non-expiring, TTL == -1).
//        Inspect cached record: preserve if cached version >= marker (current or newer with marker);
//        delete if cached version < marker (genuinely stale).
//      - If committed >= existing marker (or no marker):
//        Advance marker to committed monotonically; stored without expiration (TTL == -1).
//        Inspect cached record: preserve if cached version > committed;
//        delete if cached version <= committed.
static const char *INVALIDATE_LUA = LUA_UINT64_HELPERS R"(
if not is_valid_uint64(ARGV[1]) then return 0 end
local committed = tostring(ARGV[1])

local marker = redis.call('GET', KEYS[2])
local marker_is_newer = false

if marker and is_valid_uint64(marker) then
    if cmp_uint64(committed, marker) < 0 then
        -- Existing committed marker is newer than incoming invalidation (delayed invalidation).
        marker_is_newer = true
    else
        -- Incoming committed version is newer or equal: update marker monotonically.
        redis.call('SET', KEYS[2], committed)
    end
else
    -- No previous marker: set marker to incoming committed version.
    redis.call('SET', KEYS[2], committed)
end

-- Inspect existing cached record if present.
local cur = redis.call('GET', KEYS[1])
if not cur then return 1 end

local ok, doc = pcall(cjson.decode, cur)
if ok and type(doc) == 'table' and is_valid_uint64(doc.version) then
    if marker_is_newer then
        -- An existing committed marker is newer than incoming invalidation version (committed < marker).
        -- Preserve any cached entry whose version is equal to or newer than that marker
        -- (e.g. cached V3 must NOT be deleted by a delayed V2 invalidation when marker is V3).
        if cmp_uint64(doc.version, marker) >= 0 then
            return 2
        end
    else
        -- Normal invalidation: preserve only genuinely newer entries (doc.version > committed).
        if cmp_uint64(doc.version, committed) > 0 then
            return 2
        end
    end
end

-- Entry is stale (doc.version < marker when marker is newer, or doc.version <= committed)
-- or unparseable: delete it.
redis.call('DEL', KEYS[1])
return 1
)";

// Atomic Lua script for version-aware cache population (Specification §6.2).
// Prevents slow reads from repopulating Redis with stale data when a newer committed version
// marker exists or an equal/newer entry is already cached.
//
// Arguments:
//   KEYS[1] : Cache key (e.g. api-cache:inventory:123)
//   KEYS[2] : Version marker key (e.g. api-ver:inventory:123)
//   ARGV[1] : Serialized JSON cache entry
//   ARGV[2] : Entry version decimal string
//   ARGV[3] : Key expiration TTL in seconds
//
// Rules:
//   1. Check committed version marker (KEYS[2]):
//      - Reject if incoming version < marker (< 0).
//      - Equal versions (incoming == marker) are allowed, permitting a reader that loaded
//        the newly committed version to populate Redis cleanly after invalidation.
//   2. Check existing cached entry (KEYS[1]):
//      - Reject if incoming version <= existing cached version (<= 0), preventing duplicate
//        or older overwrites.
//   3. Write entry with expiration TTL.
static const char *POPULATE_LUA = LUA_UINT64_HELPERS R"(
if not is_valid_uint64(ARGV[2]) then return 0 end
local new_ver = tostring(ARGV[2])

local marker = redis.call('GET', KEYS[2])
if marker and is_valid_uint64(marker) then
    if cmp_uint64(new_ver, marker) < 0 then return 0 end
end

local cur = redis.call('GET', KEYS[1])
if cur then
    local ok, doc = pcall(cjson.decode, cur)
    if ok and type(doc) == 'table' and is_valid_uint64(doc.version) then
        if cmp_uint64(new_ver, doc.version) <= 0 then return 0 end
    end
end

redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[3])
return 1
)";

// ============================================================================
// Wire Format Serialization & Deserialization
// ============================================================================

// Serializes a cache entry into a versioned JSON payload for Redis storage.
// Both data and version are stored as strings to avoid precision loss.
std::string SerializeEntry(const RedisCache::Entry &entry) {
    Poco::JSON::Object obj;
    obj.set("fmt", std::string(WIRE_FMT_TAG));
    obj.set("data", entry.data);
    obj.set("version", entry.version);
    std::ostringstream ss;
    Poco::JSON::Stringifier::stringify(obj, ss);
    return ss.str();
}

// Parses and validates a JSON wire string back into a RedisCache::Entry.
// Verifies schema version tag, explicit string types, and valid uint64 version.
std::optional<RedisCache::Entry> DeserializeEntry(const std::string &json) {
    if (json.empty()) return std::nullopt;
    try {
        Poco::JSON::Parser parser;
        auto obj = parser.parse(json).extract<Poco::JSON::Object::Ptr>();
        if (!obj || !obj->has("fmt") || !obj->get("fmt").isString()) return std::nullopt;
        if (obj->getValue<std::string>("fmt") != WIRE_FMT_TAG) return std::nullopt;
        if (!obj->has("data") || !obj->get("data").isString()) return std::nullopt;
        if (!obj->has("version") || !obj->get("version").isString()) return std::nullopt;

        std::string verStr = obj->getValue<std::string>("version");
        if (!IsValidVersion(verStr)) return std::nullopt;

        return RedisCache::Entry{obj->getValue<std::string>("data"), std::move(verStr)};
    } catch (...) {
        return std::nullopt;
    }
}

// Connects a Poco::Redis::Client, sets timeouts, and runs optional AUTH and SELECT.
// Passwords are never logged. Throws Poco::Exception on failure.
void ConnectRedisClient(Poco::Redis::Client &client,
                        const std::string &host, int port,
                        const std::string &password, int db,
                        int timeoutMs) {
    const Poco::Timespan connTimeout(static_cast<long>(timeoutMs) * 1000LL);
    client.connect(host, port, connTimeout);
    client.setReceiveTimeout(connTimeout);

    if (!password.empty()) {
        Poco::Redis::Array authCmd;
        authCmd << "AUTH" << password;
        if (client.execute<std::string>(authCmd) != "OK") {
            client.disconnect();
            throw Poco::Exception("Redis AUTH rejected");
        }
    }
    if (db != 0) {
        Poco::Redis::Array selectCmd;
        selectCmd << "SELECT" << std::to_string(db);
        if (client.execute<std::string>(selectCmd) != "OK") {
            client.disconnect();
            throw Poco::Exception(fmt::format("Redis SELECT {} rejected", db));
        }
    }
}
#endif

} // anonymous namespace

// ============================================================================
// Configuration & Key Construction
// ============================================================================

// Reads Redis cache configuration from microservice properties (openwifi.redis.*).
// Validates all settings (port, db, timeout, ttl, pool limits) and applies safe defaults.
RedisCacheConfig MakeRedisCacheConfig() {
    RedisCacheConfig cfg;
    Poco::Logger &log = Poco::Logger::get("RedisCache");
    try {
        if (!MicroServiceConfigGetBool("openwifi.redis.enable", true)) {
            poco_information(log, "Redis caching disabled: 'openwifi.redis.enable' is false.");
            return cfg;
        }
        std::string host = MicroServiceConfigGetString("openwifi.redis.host", "");
        if (host.empty()) {
            poco_information(log, "Redis caching disabled: 'openwifi.redis.host' is empty.");
            return cfg;
        }
        cfg.enabled = true;
        cfg.host = host;

        int port = static_cast<int>(MicroServiceConfigGetInt("openwifi.redis.port", 6379));
        cfg.port = (port > 0 && port <= 65535) ? port : 6379;

        cfg.password = MicroServiceConfigGetString("openwifi.redis.password", "");

        int db = static_cast<int>(MicroServiceConfigGetInt("openwifi.redis.db", 0));
        cfg.db = (db >= 0 && db <= 255) ? db : 0;

        int timeout = static_cast<int>(MicroServiceConfigGetInt("openwifi.redis.timeout", 2000));
        cfg.timeoutMs = (timeout >= 50 && timeout <= 60000) ? timeout : 2000;

        int ttl = static_cast<int>(MicroServiceConfigGetInt("openwifi.redis.cache.ttl", 60));
        cfg.cacheTtlSec = (ttl >= 10 && ttl <= 60) ? ttl : 60;

        int maxTotal = static_cast<int>(MicroServiceConfigGetInt("openwifi.redis.pool.maxtotal", 32));
        if (maxTotal >= 1 && maxTotal <= 256) cfg.maxTotalConnections = maxTotal;

        int maxIdle = static_cast<int>(MicroServiceConfigGetInt("openwifi.redis.pool.maxidle", 16));
        if (maxIdle >= 1 && maxIdle <= static_cast<int>(cfg.maxTotalConnections)) cfg.maxIdleConnections = maxIdle;

    } catch (const std::exception &e) {
        poco_warning(log, fmt::format("Error reading Redis configuration: {}. Redis cache disabled.", e.what()));
        cfg.enabled = false;
    } catch (...) {
        poco_warning(log, "Unknown error reading Redis configuration. Redis cache disabled.");
        cfg.enabled = false;
    }
    return cfg;
}

// Builds a deterministic cache key: "api-cache:<resource_type>:<resource_id>".
std::string MakeCacheKey(const std::string &ResourceType, const std::string &ResourceId) {
    if (!IsValidResourceType(ResourceType) || !IsValidResourceId(ResourceId)) return {};
    return fmt::format("{}{}:{}", CACHE_KEY_PREFIX, Poco::toLower(ResourceType), ResourceId);
}

// Builds a deterministic version-marker key: "api-ver:<resource_type>:<resource_id>".
std::string MakeVersionKey(const std::string &ResourceType, const std::string &ResourceId) {
    if (!IsValidResourceType(ResourceType) || !IsValidResourceId(ResourceId)) return {};
    return fmt::format("{}{}:{}", VER_KEY_PREFIX, Poco::toLower(ResourceType), ResourceId);
}

// Converts a cache key "api-cache:<type>:<id>" to its version-marker key "api-ver:<type>:<id>".
// Validates that CacheKey is a valid, canonical cache key before converting; returns empty string otherwise.
std::string MakeVersionKey(const std::string &CacheKey) {
    if (!IsCacheableKey(CacheKey)) return {};
    constexpr std::string_view prefix = CACHE_KEY_PREFIX;
    return fmt::format("{}{}", VER_KEY_PREFIX, CacheKey.substr(prefix.size()));
}

#ifdef USE_POCO_REDIS
// ============================================================================
// Connection Pooling & RAII Lease
// ============================================================================

// Connection pool managing reusable Poco::Redis::Client connections.
// Enforces a dual-bound strategy:
//   - MaxTotal: limits active + idle connections to prevent socket explosion under concurrent bursts.
//   - MaxIdle: limits warm idle connections kept in memory.
// When capacity is reached, callers wait briefly on a condition variable before safely falling back.
struct RedisConnectionPool {
    const RedisCacheConfig &Config;
    Poco::Logger           &Log;
    std::mutex              PoolMutex;
    std::condition_variable PoolCv;
    std::vector<std::unique_ptr<Poco::Redis::Client>> IdleClients;
    size_t                  MaxIdle;
    size_t                  MaxTotal;
    size_t                  ActiveCount = 0; // Number of connections currently leased or connecting

    RedisConnectionPool(const RedisCacheConfig &cfg, Poco::Logger &log)
        : Config(cfg), Log(log),
          MaxIdle(cfg.maxIdleConnections > 0 ? std::min(cfg.maxIdleConnections, cfg.maxTotalConnections) : 16),
          MaxTotal(cfg.maxTotalConnections > 0 ? cfg.maxTotalConnections : 32) {}

    ~RedisConnectionPool() {
        std::lock_guard G(PoolMutex);
        for (auto &client : IdleClients) {
            try { client->disconnect(); } catch (...) {}
        }
        IdleClients.clear();
    }

    // Borrows an idle client or creates a new connection up to MaxTotal.
    // If the pool is at capacity, waits up to 100ms for a slot or returns nullptr for safe fallback.
    std::unique_ptr<Poco::Redis::Client> Acquire() {
        std::unique_lock<std::mutex> lock(PoolMutex);

        // 1. Reuse existing idle client
        if (!IdleClients.empty()) {
            auto client = std::move(IdleClients.back());
            IdleClients.pop_back();
            ActiveCount++;
            return client;
        }

        // 2. Connect new client if below MaxTotal limit
        if (ActiveCount < MaxTotal) {
            ActiveCount++;
            lock.unlock(); // Connect outside mutex to avoid blocking other threads
            auto client = std::make_unique<Poco::Redis::Client>();
            try {
                ConnectRedisClient(*client, Config.host, Config.port,
                                   Config.password, Config.db, Config.timeoutMs);
                return client;
            } catch (...) {
                std::lock_guard<std::mutex> g(PoolMutex);
                if (ActiveCount > 0) ActiveCount--;
                PoolCv.notify_one();
                throw;
            }
        }

        // 3. Pool capacity reached: wait up to 100ms for an active connection to be returned
        constexpr auto ACQUIRE_TIMEOUT = std::chrono::milliseconds(100);
        bool acquired = PoolCv.wait_for(lock, ACQUIRE_TIMEOUT, [this]() {
            return !IdleClients.empty() || ActiveCount < MaxTotal;
        });

        if (acquired) {
            if (!IdleClients.empty()) {
                auto client = std::move(IdleClients.back());
                IdleClients.pop_back();
                ActiveCount++;
                return client;
            }
            if (ActiveCount < MaxTotal) {
                ActiveCount++;
                lock.unlock();
                auto client = std::make_unique<Poco::Redis::Client>();
                try {
                    ConnectRedisClient(*client, Config.host, Config.port,
                                       Config.password, Config.db, Config.timeoutMs);
                    return client;
                } catch (...) {
                    std::lock_guard<std::mutex> g(PoolMutex);
                    if (ActiveCount > 0) ActiveCount--;
                    PoolCv.notify_one();
                    throw;
                }
            }
        }

        // 4. Pool exhausted: log warning and return nullptr for safe fallback to database
        poco_warning(Log, fmt::format("RedisCache: Connection pool exhausted (max {} active connections). Falling back to direct database operations.", MaxTotal));
        return nullptr;
    }

    // Returns a healthy client to the pool, or disconnects and drops it if unhealthy/full.
    void Release(std::unique_ptr<Poco::Redis::Client> client, bool healthy) {
        if (!client) return;
        {
            std::lock_guard<std::mutex> lock(PoolMutex);
            if (ActiveCount > 0) ActiveCount--;
            if (healthy && IdleClients.size() < MaxIdle) {
                IdleClients.push_back(std::move(client));
                PoolCv.notify_one();
                return;
            }
            PoolCv.notify_one();
        }
        try { client->disconnect(); } catch (...) {}
    }
};

// RAII wrapper ensuring borrowed Redis clients are safely returned or cleaned up upon scope exit.
class PooledClientLease {
  public:
    PooledClientLease(RedisConnectionPool &pool, std::unique_ptr<Poco::Redis::Client> client)
        : Pool_(&pool), Client_(std::move(client)) {}

    ~PooledClientLease() {
        if (Pool_ && Client_) Pool_->Release(std::move(Client_), Healthy_);
    }

    PooledClientLease(const PooledClientLease &) = delete;
    PooledClientLease &operator=(const PooledClientLease &) = delete;
    PooledClientLease(PooledClientLease &&other) noexcept
        : Pool_(other.Pool_), Client_(std::move(other.Client_)), Healthy_(other.Healthy_) {
        other.Pool_ = nullptr;
    }
    PooledClientLease &operator=(PooledClientLease &&other) noexcept {
        if (this != &other) {
            if (Pool_ && Client_) Pool_->Release(std::move(Client_), Healthy_);
            Pool_ = other.Pool_;
            Client_ = std::move(other.Client_);
            Healthy_ = other.Healthy_;
            other.Pool_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] Poco::Redis::Client &Client() noexcept { return *Client_; }
    void MarkUnhealthy() noexcept { Healthy_ = false; }

  private:
    RedisConnectionPool *Pool_ = nullptr;
    std::unique_ptr<Poco::Redis::Client> Client_;
    bool Healthy_ = true;
};
#endif

// Private implementation structure (PIMPL) managing connection pool and command execution.
struct RedisCache::Impl {
#ifdef USE_POCO_REDIS
    RedisConnectionPool Pool;
    explicit Impl(const RedisCacheConfig &cfg, Poco::Logger &log) : Pool(cfg, log) {}

    // Acquires a client lease if Redis is enabled, not in backoff, and pool not exhausted.
    std::optional<PooledClientLease> AcquireLease(std::atomic<int64_t> &nextReconnect,
                                                  std::atomic<bool> &redisHealthy,
                                                  RedisCache &cache,
                                                  std::string *failureReason = nullptr) {
        if (!Pool.Config.enabled || Pool.Config.host.empty()) {
            if (failureReason) *failureReason = "Redis caching is disabled or host is unconfigured";
            return std::nullopt;
        }

        // Fail-fast check during reconnect backoff window
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowMs < nextReconnect.load(std::memory_order_relaxed)) {
            if (failureReason) *failureReason = "Redis connection is in reconnect backoff window";
            return std::nullopt;
        }

        try {
            auto client = Pool.Acquire();
            if (!client) {
                if (failureReason) *failureReason = "Redis connection pool exhausted (maxTotalConnections limit reached)";
                return std::nullopt;
            }
            redisHealthy.store(true, std::memory_order_relaxed);
            return PooledClientLease(Pool, std::move(client));
        } catch (const std::exception &e) {
            cache.OnRedisFailure("CONNECT", "", e.what());
            if (failureReason) *failureReason = e.what();
        } catch (...) {
            cache.OnRedisFailure("CONNECT", "", "Unknown connection error");
            if (failureReason) *failureReason = "Unknown connection error";
        }
        return std::nullopt;
    }

    // Central command execution helper: acquires lease, executes command lambda,
    // and unified error handling (backoff activation, lease marking, socket cleanup).
    template <typename Func>
    bool Execute(const std::string &op, const std::string &key,
                 std::atomic<int64_t> &nextReconnect,
                 std::atomic<bool> &redisHealthy,
                 RedisCache &cache,
                 Func &&fn,
                 std::string *failureReason = nullptr) {
        std::string acquireReason;
        auto lease = AcquireLease(nextReconnect, redisHealthy, cache, &acquireReason);
        if (!lease) {
            if (failureReason) *failureReason = std::move(acquireReason);
            return false;
        }
        try {
            fn(lease->Client());
            return true;
        } catch (const std::exception &e) {
            lease->MarkUnhealthy();
            cache.OnRedisFailure(op, key, e.what());
            if (failureReason) *failureReason = e.what();
        } catch (...) {
            lease->MarkUnhealthy();
            cache.OnRedisFailure(op, key, "Unknown exception");
            if (failureReason) *failureReason = "Unknown exception";
        }
        return false;
    }
#else
    explicit Impl(const RedisCacheConfig &, Poco::Logger &) {}
#endif
};

// ============================================================================
// RedisCache Public Interface Implementation
// ============================================================================

RedisCache::RedisCache(RedisCacheConfig Config, Poco::Logger &Logger)
    : Config_(std::move(Config)),
      Log_(Logger),
      Impl_(std::make_unique<Impl>(Config_, Log_)) {
    RedisHealthy_.store(false, std::memory_order_relaxed);
    NextReconnectTimeMs_.store(0, std::memory_order_relaxed);
}

RedisCache::~RedisCache() = default;

// Consolidated error handler for all Redis connection and command failures.
// Activates the 5-second backoff window and marks Redis unhealthy.
void RedisCache::OnRedisFailure(const std::string &Operation, const std::string &Key, const std::string &Reason) {
    if (Key.empty()) {
        poco_warning(Log_, fmt::format("RedisCache: {} failed: {}. Bypassing Redis for {}ms.", Operation, Reason, RECONNECT_BACKOFF_MS));
    } else {
        poco_warning(Log_, fmt::format("RedisCache: {} failed for key '{}': {}. Bypassing Redis for {}ms.", Operation, Key, Reason, RECONNECT_BACKOFF_MS));
    }
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    NextReconnectTimeMs_.store(nowMs + RECONNECT_BACKOFF_MS, std::memory_order_relaxed);
    RedisHealthy_.store(false, std::memory_order_relaxed);
}

// Cache-aside read operation with authoritative loader fallback.
// Workflow:
//   1. Check key cacheability. Invalid keys bypass Redis directly to the loader.
//   2. Attempt Redis GET. The client lease is released BEFORE executing loader(),
//      preventing connection exhaustion during database queries.
//   3. Execute authoritative loader if cache miss or Redis unavailable.
//   4. Atomically populate Redis using Set(). If population is rejected (e.g. newer
//      version marker committed concurrently), returns loaded data as Fallback.
RedisCache::Outcome RedisCache::Get(const std::string &Key, Loader loader) {
    // 1. Unsupported or invalid key: bypass Redis directly to authoritative loader
    if (!IsCacheableKey(Key)) {
        poco_debug(Log_, fmt::format("RedisCache: Key '{}' cannot be cached; bypassing Redis to loader.", Key));
        try {
            auto loaded = loader();
            return loaded ? Outcome{Result::Fallback, std::move(loaded), ""} : Outcome{Result::NotFound, std::nullopt, ""};
        } catch (const std::exception &e) {
            return {Result::Error, std::nullopt, e.what()};
        } catch (...) {
            return {Result::Error, std::nullopt, "Unknown loader exception"};
        }
    }

    // 2. Attempt Redis GET (connection lease is acquired and released strictly for the GET)
    std::optional<Entry> cachedEntry;
    bool redisAvailable = false;
#ifdef USE_POCO_REDIS
    redisAvailable = Impl_->Execute("GET", Key, NextReconnectTimeMs_, RedisHealthy_, *this,
        [&](Poco::Redis::Client &client) {
            Poco::Redis::Array GetCmd;
            GetCmd << "GET" << Key;
            auto reply = client.execute<Poco::Redis::BulkString>(GetCmd);
            if (!reply.isNull()) cachedEntry = DeserializeEntry(reply.value());
        });
    if (cachedEntry) {
        return {Result::Hit, std::move(cachedEntry), ""};
    }
#endif

    // 3. Cache Miss or Redis unavailable: invoke authoritative loader without holding Redis connections
    std::optional<Entry> loadedEntry;
    try {
        loadedEntry = loader();
    } catch (const std::exception &e) {
        poco_warning(Log_, fmt::format("RedisCache: Authoritative loader threw exception for key '{}': {}", Key, e.what()));
        return {Result::Error, std::nullopt, e.what()};
    } catch (...) {
        poco_warning(Log_, fmt::format("RedisCache: Authoritative loader threw unknown exception for key '{}'.", Key));
        return {Result::Error, std::nullopt, "Unknown loader exception"};
    }

    // Record absent in database (e.g. HTTP 404)
    if (!loadedEntry) {
        return {Result::NotFound, std::nullopt, ""};
    }

    // 4. Record loaded: populate Redis if available.
    // Set() atomically checks against committed version markers and existing entries via Lua.
    if (redisAvailable && Set(Key, *loadedEntry, Config_.cacheTtlSec)) {
        return {Result::Miss, std::move(loadedEntry), ""};
    }
    return {Result::Fallback, std::move(loadedEntry), ""};
}

// Direct cache read primitive. Returns entry on hit, or nullopt on miss/offline/error.
std::optional<RedisCache::Entry> RedisCache::Get(const std::string &Key) {
    if (!IsCacheableKey(Key)) return std::nullopt;
    std::optional<RedisCache::Entry> result;
#ifdef USE_POCO_REDIS
    Impl_->Execute("GET", Key, NextReconnectTimeMs_, RedisHealthy_, *this,
        [&](Poco::Redis::Client &client) {
            Poco::Redis::Array GetCmd;
            GetCmd << "GET" << Key;
            auto reply = client.execute<Poco::Redis::BulkString>(GetCmd);
            if (!reply.isNull()) result = DeserializeEntry(reply.value());
        });
#endif
    return result;
}

// Atomic version-aware cache population primitive.
// Populates entry with TTL [10, 60]s. Drops write if incoming version < committed marker
// or incoming version <= existing cached entry. Strictly validates version string.
bool RedisCache::Set(const std::string &Key, const Entry &entry, int TtlSec) {
    if (!IsCacheableKey(Key)) {
        poco_warning(Log_, fmt::format("RedisCache: Rejected Set() on invalid key: '{}'.", Key));
        return false;
    }
    if (!IsValidVersion(entry.version)) {
        poco_warning(Log_, fmt::format("RedisCache: Rejected Set() for key '{}': version '{}' must be a positive uint64 decimal.", Key, entry.version));
        return false;
    }

#ifdef USE_POCO_REDIS
    bool ok = false;
    Impl_->Execute("SET", Key, NextReconnectTimeMs_, RedisHealthy_, *this,
        [&](Poco::Redis::Client &client) {
            Poco::Redis::Array EvalCmd;
            EvalCmd << "EVAL"
                    << POPULATE_LUA
                    << "2"
                    << Key
                    << MakeVersionKey(Key)
                    << SerializeEntry(entry)
                    << entry.version
                    << std::to_string(ClampTtl(TtlSec, Config_.cacheTtlSec));
            ok = (client.execute<Poco::Int64>(EvalCmd) == 1);
        });
    return ok;
#else
    (void)entry; (void)TtlSec;
    return false;
#endif
}

// Atomic version-aware cache invalidation (Specification §6.2, §6.3).
// Atomically deletes the cached entry for Key if its version <= committedVersion, then
// records the committed version marker so future stale reads cannot repopulate Redis.
// Returns true on success or safe no-op; returns false only on Redis failure.
// Never throws; logs ERROR on failure per Specification §6.3.1.
bool RedisCache::Invalidate(const std::string &Key, const std::string &CommittedVersion) {
    if (!IsCacheableKey(Key)) {
        poco_warning(Log_, fmt::format("RedisCache: Rejected Invalidate() on invalid key: '{}'.", Key));
        return false;
    }
    if (!IsValidVersion(CommittedVersion)) {
        poco_warning(Log_, fmt::format("RedisCache: Rejected Invalidate() for key '{}': version '{}' must be a positive uint64 decimal.", Key, CommittedVersion));
        return false;
    }

#ifdef USE_POCO_REDIS
    bool ok = false;
    std::string failureReason;
    const std::string verKey = MakeVersionKey(Key);
    Impl_->Execute("INVALIDATE", Key, NextReconnectTimeMs_, RedisHealthy_, *this,
        [&](Poco::Redis::Client &client) {
            Poco::Redis::Array EvalCmd;
            EvalCmd << "EVAL"
                    << INVALIDATE_LUA
                    << "2"
                    << Key
                    << verKey
                    << CommittedVersion;
            auto result = client.execute<Poco::Int64>(EvalCmd);
            // 0 = validation failure, 1 = success, 2 = safe no-op (newer entry preserved).
            if (result == 1 || result == 2) {
                ok = true;
            } else {
                failureReason = "Redis Lua script rejected invalid version";
            }
        },
        &failureReason);
    if (!ok) {
        if (failureReason.empty()) failureReason = "Unknown Redis execution failure";
        poco_error(Log_, fmt::format(
            "RedisCache: Invalidation failed for key '{}' version '{}': {}. "
            "Stale entry may persist up to cache TTL ({}-{}s).",
            Key, CommittedVersion, failureReason, MIN_CACHE_TTL_SEC, MAX_CACHE_TTL_SEC));
        ++InvalidationFailures_;
        ++owprov_redis_invalidation_failures_total;
    }
    return ok;
#else
    (void)CommittedVersion;
    return false;
#endif
}

// Invalidation primitive. Deletes a key from Redis. Both 0 and 1 keys deleted count as success.
bool RedisCache::Delete(const std::string &Key) {
    if (!IsCacheableKey(Key)) return false;
#ifdef USE_POCO_REDIS
    bool ok = false;
    Impl_->Execute("DEL", Key, NextReconnectTimeMs_, RedisHealthy_, *this,
        [&](Poco::Redis::Client &client) {
            Poco::Redis::Array DelCmd;
            DelCmd << "DEL" << Key;
            ok = (client.execute<Poco::Int64>(DelCmd) >= 0);
        });
    return ok;
#else
    return false;
#endif
}

// Returns true if Redis connection is healthy and not in reconnect backoff.
bool RedisCache::IsRedisHealthy() const noexcept {
    return RedisHealthy_.load(std::memory_order_relaxed);
}

// Returns active configuration.
const RedisCacheConfig &RedisCache::Config() const noexcept {
    return Config_;
}

} // namespace OpenWifi
