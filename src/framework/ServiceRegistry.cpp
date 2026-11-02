//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//

#include "framework/ServiceRegistry.h"

#include <string>
#include <vector>
#include <optional>

#include "Poco/Logger.h"
#include "Poco/JSON/Object.h"
#include "Poco/JSON/Parser.h"
#include "Poco/JSON/Stringifier.h"
#include "fmt/format.h"

#ifdef USE_POCO_REDIS
#include "Poco/Redis/Array.h"
#include "Poco/Redis/Client.h"
#include "Poco/Redis/Command.h"
#include "Poco/Redis/Exception.h"
#include "Poco/Redis/Type.h"
#include "Poco/Timespan.h"
#endif

#include "framework/OpenWifiTypes.h"
#include "framework/utils.h"

namespace OpenWifi::ServiceRegistry {

namespace {

// Redis key prefix for service-registry entries.
// All lookup and SCAN operations use this pattern.
constexpr const char *KEY_PREFIX = "service-registry:";

// JSON field names used in Redis-stored service metadata.
// These are stable identifiers: do not change them without a migration plan,
// as peer services sharing the same Redis snapshot contract depend on them.
constexpr const char *FIELD_INSTANCE_ID   = "instanceId";
constexpr const char *FIELD_TYPE          = "type";
constexpr const char *FIELD_PRIVATE_EP    = "privateEndPoint";
constexpr const char *FIELD_PUBLIC_EP     = "publicEndPoint";
constexpr const char *FIELD_ACCESS_KEY    = "accessKey";
constexpr const char *FIELD_VERSION       = "version";
constexpr const char *FIELD_LAST_UPDATE   = "lastUpdate";

// SCAN batch count hint. Redis treats this as advisory; the actual count
// per iteration may differ. A count of 100 keeps individual SCAN calls fast
// while reducing round-trips for registries with many entries.
constexpr int SCAN_COUNT = 100;

} // anonymous namespace

#include "Poco/String.h"

std::string MakeKey(const std::string &ServiceType, const std::string &InstanceId) {
    return fmt::format("{}{}:{}", KEY_PREFIX, Poco::toLower(ServiceType), InstanceId);
}

std::optional<std::pair<std::string, std::string>>
ParseKey(const std::string &RedisKey) {
    const std::string prefix = KEY_PREFIX;
    if (RedisKey.size() <= prefix.size() ||
        RedisKey.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }

    // Strip prefix: remainder is "{service_type}:{instance_id}"
    const std::string remainder = RedisKey.substr(prefix.size());
    const auto colonPos = remainder.find(':');
    if (colonPos == std::string::npos || colonPos == 0 ||
        colonPos + 1 >= remainder.size()) {
        return std::nullopt;
    }

    std::string serviceType = Poco::toLower(remainder.substr(0, colonPos));
    std::string instanceId  = remainder.substr(colonPos + 1);
    if (serviceType.empty() || instanceId.empty()) {
        return std::nullopt;
    }

    return std::make_pair(std::move(serviceType), std::move(instanceId));
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string Serialize(const Types::MicroServiceMeta &Meta) {
    Poco::JSON::Object Obj;
    Obj.set(FIELD_INSTANCE_ID, Meta.InstanceId);
    Obj.set(FIELD_TYPE,        Poco::toLower(Meta.Type));
    Obj.set(FIELD_PRIVATE_EP,  Meta.PrivateEndPoint);
    Obj.set(FIELD_PUBLIC_EP,   Meta.PublicEndPoint);
    Obj.set(FIELD_ACCESS_KEY,  Meta.AccessKey);
    Obj.set(FIELD_VERSION,     Meta.Version);
    Obj.set(FIELD_LAST_UPDATE, static_cast<Poco::Int64>(Meta.LastUpdate));

    std::ostringstream oss;
    Poco::JSON::Stringifier::stringify(Obj, oss);
    return oss.str();
}

std::optional<Types::MicroServiceMeta>
Deserialize(const std::string &Json) {
    if (Json.empty()) return std::nullopt;
    try {
        Poco::JSON::Parser Parser;
        auto Val = Parser.parse(Json);
        auto Obj = Val.extract<Poco::JSON::Object::Ptr>();
        if (!Obj) return std::nullopt;

        Types::MicroServiceMeta Meta;
        Meta.InstanceId     = Obj->optValue<std::string>(FIELD_INSTANCE_ID, "");
        Meta.Type           = Poco::toLower(Obj->optValue<std::string>(FIELD_TYPE, ""));
        Meta.PrivateEndPoint = Obj->optValue<std::string>(FIELD_PRIVATE_EP, "");
        Meta.PublicEndPoint  = Obj->optValue<std::string>(FIELD_PUBLIC_EP, "");
        Meta.AccessKey      = Obj->optValue<std::string>(FIELD_ACCESS_KEY, "");
        Meta.Version        = Obj->optValue<std::string>(FIELD_VERSION, "");
        int64_t rawLastUpdate = Obj->optValue<Poco::Int64>(FIELD_LAST_UPDATE, 0);
        if (rawLastUpdate <= 0) {
            return std::nullopt;
        }
        Meta.LastUpdate     = static_cast<uint64_t>(rawLastUpdate);

        // Reject entries missing fields required by Kafka discovery handler
        // (BusMessageReceived requires InstanceId, Type, PrivateEndPoint, PublicEndPoint, and AccessKey)
        if (Meta.InstanceId.empty() || Meta.Type.empty() ||
            Meta.PrivateEndPoint.empty() || Meta.PublicEndPoint.empty() ||
            Meta.AccessKey.empty()) {
            return std::nullopt;
        }

        return Meta;
    } catch (...) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Redis operations
// ---------------------------------------------------------------------------

#ifdef USE_POCO_REDIS

namespace {

// Connects a Poco::Redis::Client with a connect and receive timeout.
// Throws Poco::Exception or std::exception on failure.
void ConnectClient(Poco::Redis::Client &Client,
                   const std::string &Host, int Port,
                   const std::string &Password, int Db,
                   int ConnectTimeoutMs) {
    const Poco::Timespan ConnTimeout(ConnectTimeoutMs * 1000LL); // microseconds
    Client.connect(Host, Port, ConnTimeout);
    Client.setReceiveTimeout(ConnTimeout);

    // AUTH (only if password is non-empty)
    if (!Password.empty()) {
        Poco::Redis::Array AuthCmd;
        AuthCmd << "AUTH" << Password;
        auto Reply = Client.execute<std::string>(AuthCmd);
        if (Reply != "OK") {
            Client.disconnect();
            throw Poco::Exception("Redis AUTH rejected");
        }
    }

    // SELECT db (skip for db=0, which is the default)
    if (Db != 0) {
        Poco::Redis::Array SelectCmd;
        SelectCmd << "SELECT" << std::to_string(Db);
        auto Reply = Client.execute<std::string>(SelectCmd);
        if (Reply != "OK") {
            Client.disconnect();
            throw Poco::Exception(fmt::format("Redis SELECT {} rejected", Db));
        }
    }
}

} // anonymous namespace

bool WriteRegistration(const std::string &Host, int Port,
                       const std::string &Password, int Db,
                       int ConnectTimeoutMs,
                       const Types::MicroServiceMeta &Meta, int TtlSeconds) {
    Poco::Logger &Log = Poco::Logger::get("ServiceRegistry");
    try {
        Poco::Redis::Client Client;
        ConnectClient(Client, Host, Port, Password, Db, ConnectTimeoutMs);

        const std::string Key   = MakeKey(Meta.Type, Meta.InstanceId);
        const std::string Value = Serialize(Meta);

        // SET key value EX ttl
        Poco::Redis::Array SetCmd;
        SetCmd << "SET" << Key << Value << "EX" << std::to_string(TtlSeconds);
        auto Reply = Client.execute<std::string>(SetCmd);
        Client.disconnect();

        if (Reply != "OK") {
            poco_warning(Log, fmt::format(
                "ServiceRegistry: SET for instance '{}' returned unexpected reply.",
                Meta.InstanceId));
            return false;
        }
        return true;

    } catch (const Poco::Redis::RedisException &E) {
        poco_warning(Log, fmt::format(
            "ServiceRegistry: Redis error writing registration for instance '{}': {}",
            Meta.InstanceId, E.message()));
    } catch (const Poco::Exception &E) {
        poco_warning(Log, fmt::format(
            "ServiceRegistry: Poco exception writing registration for instance '{}': {}",
            Meta.InstanceId, E.displayText()));
    } catch (const std::exception &E) {
        poco_warning(Log, fmt::format(
            "ServiceRegistry: Exception writing registration for instance '{}': {}",
            Meta.InstanceId, E.what()));
    } catch (...) {
        poco_warning(Log, fmt::format(
            "ServiceRegistry: Unknown exception writing registration for instance '{}'.",
            Meta.InstanceId));
    }
    return false;
}

bool DeleteRegistration(const std::string &Host, int Port,
                        const std::string &Password, int Db,
                        int ConnectTimeoutMs,
                        const std::string &ServiceType,
                        const std::string &InstanceId) {
    Poco::Logger &Log = Poco::Logger::get("ServiceRegistry");
    try {
        Poco::Redis::Client Client;
        ConnectClient(Client, Host, Port, Password, Db, ConnectTimeoutMs);

        const std::string Key = MakeKey(ServiceType, InstanceId);
        Poco::Redis::Array DelCmd;
        DelCmd << "DEL" << Key;
        Client.execute<Poco::Int64>(DelCmd);
        Client.disconnect();
        return true;

    } catch (const Poco::Redis::RedisException &E) {
        poco_warning(Log, fmt::format(
            "ServiceRegistry: Redis error deleting registration for instance '{}': {}",
            InstanceId, E.message()));
    } catch (const Poco::Exception &E) {
        poco_warning(Log, fmt::format(
            "ServiceRegistry: Poco exception deleting registration for instance '{}': {}",
            InstanceId, E.displayText()));
    } catch (const std::exception &E) {
        poco_warning(Log, fmt::format(
            "ServiceRegistry: Exception deleting registration for instance '{}': {}",
            InstanceId, E.what()));
    } catch (...) {
        poco_warning(Log, fmt::format(
            "ServiceRegistry: Unknown exception deleting registration for instance '{}'.",
            InstanceId));
    }
    return false;
}

std::vector<Types::MicroServiceMeta>
LoadAll(const std::string &Host, int Port,
        const std::string &Password, int Db,
        int ConnectTimeoutMs,
        const std::atomic_bool *CancelSignal) {
    Poco::Logger &Log = Poco::Logger::get("ServiceRegistry");
    std::vector<Types::MicroServiceMeta> Results;

    if (CancelSignal && *CancelSignal) {
        poco_information(Log, "ServiceRegistry: Snapshot load skipped; cancellation signal is set.");
        return Results;
    }

    try {
        Poco::Redis::Client Client;
        ConnectClient(Client, Host, Port, Password, Db, ConnectTimeoutMs);

        // Iterative SCAN to enumerate all service-registry:* keys without
        // blocking the Redis server (avoids KEYS * on production data).
        std::string Cursor = "0";
        const std::string Pattern = fmt::format("{}*", KEY_PREFIX);

        do {
            if (CancelSignal && *CancelSignal) {
                poco_information(Log, "ServiceRegistry: Snapshot load cancelled by shutdown signal.");
                break;
            }

            // SCAN cursor MATCH service-registry:* COUNT 100
            Poco::Redis::Array ScanCmd;
            ScanCmd << "SCAN" << Cursor
                    << "MATCH" << Pattern
                    << "COUNT" << std::to_string(SCAN_COUNT);

            auto ScanReply = Client.execute<Poco::Redis::Array>(ScanCmd);

            // SCAN reply: [next_cursor, [key1, key2, ...]]
            if (ScanReply.size() < 2) break;

            auto NextCursorBs = ScanReply.get<Poco::Redis::BulkString>(0);
            if (NextCursorBs.isNull()) break;
            Cursor = NextCursorBs.value();

            auto KeyArray = ScanReply.get<Poco::Redis::Array>(1);
            if (KeyArray.isNull()) break;

            for (std::size_t i = 0; i < KeyArray.size(); ++i) {
                if (CancelSignal && *CancelSignal) {
                    poco_information(Log, "ServiceRegistry: Snapshot load cancelled by shutdown signal.");
                    break;
                }

                try {
                    auto KeyBs = KeyArray.get<Poco::Redis::BulkString>(i);
                    if (KeyBs.isNull()) continue;
                    const std::string Key = KeyBs.value();

                    // Validate key format
                    auto Parsed = ParseKey(Key);
                    if (!Parsed) {
                        poco_debug(Log, fmt::format(
                            "ServiceRegistry: SCAN returned unexpected key '{}', skipping.", Key));
                        continue;
                    }

                    // GET the JSON value
                    Poco::Redis::Array GetCmd;
                    GetCmd << "GET" << Key;
                    auto ValueBs = Client.execute<Poco::Redis::BulkString>(GetCmd);
                    if (ValueBs.isNull()) {
                        // Key expired between SCAN and GET — safe to skip
                        continue;
                    }

                    auto Meta = Deserialize(ValueBs.value());
                    if (!Meta) {
                        poco_debug(Log, fmt::format(
                            "ServiceRegistry: Skipping invalid or incomplete entry for key '{}'.", Key));
                        continue;
                    }

                    // Validate key-to-payload consistency:
                    // Key format is service-registry:{service_type}:{instance_id}.
                    if (Meta->Type != Parsed->first || Meta->InstanceId != Parsed->second) {
                        poco_warning(Log, fmt::format(
                            "ServiceRegistry: Key '{}' does not match payload content (payload type='{}', id='{}'), skipping.",
                            Key, Meta->Type, Meta->InstanceId));
                        continue;
                    }

                    Results.push_back(std::move(*Meta));

                } catch (const std::exception &E) {
                    poco_debug(Log, fmt::format(
                        "ServiceRegistry: Error processing SCAN key at index {}: {}", i, E.what()));
                } catch (...) {
                    poco_debug(Log, fmt::format(
                        "ServiceRegistry: Unknown error processing SCAN key at index {}.", i));
                }
            }

        } while (Cursor != "0");

        Client.disconnect();
        poco_information(Log, fmt::format(
            "ServiceRegistry: Bootstrap snapshot loaded {} entries from Redis.", Results.size()));

    } catch (const Poco::Redis::RedisException &E) {
        poco_warning(Log, fmt::format(
            "ServiceRegistry: Redis error during bootstrap snapshot load: {}. "
            "Service discovery will rely on live Kafka events only.", E.message()));
    } catch (const Poco::Exception &E) {
        poco_warning(Log, fmt::format(
            "ServiceRegistry: Poco exception during bootstrap snapshot load: {}. "
            "Service discovery will rely on live Kafka events only.", E.displayText()));
    } catch (const std::exception &E) {
        poco_warning(Log, fmt::format(
            "ServiceRegistry: Exception during bootstrap snapshot load: {}. "
            "Service discovery will rely on live Kafka events only.", E.what()));
    } catch (...) {
        poco_warning(Log, "ServiceRegistry: Unknown exception during bootstrap snapshot load. "
                         "Service discovery will rely on live Kafka events only.");
    }
    return Results;
}

#else // USE_POCO_REDIS not defined — Redis support compiled out

bool WriteRegistration(const std::string &, int, const std::string &, int, int,
                       const Types::MicroServiceMeta &, int) {
    return false;
}

bool DeleteRegistration(const std::string &, int, const std::string &, int, int,
                        const std::string &, const std::string &) {
    return false;
}

std::vector<Types::MicroServiceMeta>
LoadAll(const std::string &, int, const std::string &, int, int,
        const std::atomic_bool *) {
    return {};
}

#endif // USE_POCO_REDIS

} // namespace OpenWifi::ServiceRegistry
