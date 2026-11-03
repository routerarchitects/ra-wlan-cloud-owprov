/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

//
// ServiceRegistry — Redis service-registry snapshot for Phase 3 horizontal scaling.
//
// Responsibility:
//   - Format Redis keys:  service-registry:{service_type}:{instance_id}
//   - Serialize/deserialize MicroServiceMeta to/from JSON stored in Redis
//   - Provide stateless helpers: Write (SET EX), Delete (DEL), LoadAll (SCAN + GET)
//
// Architectural contract:
//   - Redis is used ONLY for bootstrap snapshot seeding. It is NOT consulted
//     during normal API request-path service lookups. The authoritative runtime
//     view is MicroService::Services_ (in-memory, updated via Kafka service_events).
//   - All Redis operations are non-throwing from the caller's perspective:
//     failures are caught, logged, and not re-thrown. A Redis failure must
//     not affect normal Kafka-based service-event publishing or local discovery.
//   - Redis credentials must NOT appear in logs. The password field, if any,
//     is passed only to the client library and never formatted into log messages.
//
// Bootstrap race correctness:
//   The caller (MicroService::BootstrapFromRedisSnapshot) loads this snapshot
//   AFTER the Kafka BroadcastConsumer is running. During snapshot loading, live
//   Kafka LEAVE events may concurrently remove instances from Services_. To
//   prevent stale snapshot data from reintroducing departed services, the caller
//   maintains a DepartedDuringBootstrap_ set (entries recorded by BusMessageReceived
//   on LEAVE while BootstrapInProgress_ is true). LoadAll returns raw entries;
//   the caller filters against this set under InfraMutex_.
//
// Thread safety:
//   These helpers are stateless. The Poco::Redis::Client is constructed on each
//   operation (connect, execute, disconnect) to avoid cross-thread sharing of
//   a single live connection. This is intentional: Redis operations happen only
//   at startup bootstrap and on JOIN/KEEP_ALIVE/LEAVE events, not in tight loops.
//   Each call creates a short-lived connection — acceptable given the low call
//   frequency (once per keep-alive interval, typically 5-10 seconds).
//
//   A long-lived shared connection would require its own mutex and reconnect logic.
//   The short-lived approach is simpler, more failure-tolerant, and appropriate
//   for this use case.
//

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "framework/OpenWifiTypes.h"

namespace OpenWifi::ServiceRegistry {

/// Returns the Redis key for a service instance.
/// Format: service-registry:{service_type}:{instance_id}
/// Keys use lowercase service type to match BusMessageReceived normalisation.
[[nodiscard]] std::string MakeKey(const std::string &ServiceType,
                                  const std::string &InstanceId);

/// Extracts (service_type, instance_id) from a Redis key of the form
/// service-registry:{service_type}:{instance_id}.
/// Returns nullopt if the key does not match the expected prefix/format.
[[nodiscard]] std::optional<std::pair<std::string, std::string>>
ParseKey(const std::string &RedisKey);

/// Serialises a MicroServiceMeta to JSON for Redis storage.
/// Only the fields needed to reconstruct a registry entry are stored.
/// Numeric legacy ID is intentionally omitted (Kafka wire format only).
[[nodiscard]] std::string Serialize(const Types::MicroServiceMeta &Meta);

/// Deserialises a MicroServiceMeta from a JSON string previously written by Serialize().
/// Returns nullopt if the JSON is malformed, missing required fields, or has
/// an empty InstanceId, Type, or PrivateEndPoint.
[[nodiscard]] std::optional<Types::MicroServiceMeta>
Deserialize(const std::string &Json);

/// Writes a service registration to Redis with the given TTL (seconds).
/// Equivalent to: SET service-registry:{type}:{id} <json> EX <ttl>
///
/// Returns true on success, false on any connection or command failure.
/// Logs failures at warning level; never throws to the caller.
///
/// Redis password is passed directly to the client and is never logged.
bool WriteRegistration(const std::string &Host, int Port,
                       const std::string &Password, int Db,
                       int ConnectTimeoutMs,
                       const Types::MicroServiceMeta &Meta, int TtlSeconds);

/// Removes a service registration from Redis.
/// Equivalent to: DEL service-registry:{type}:{id}
///
/// Returns true on success or if the key was absent, false on connection failure.
/// Logs failures at warning level; never throws to the caller.
bool DeleteRegistration(const std::string &Host, int Port,
                        const std::string &Password, int Db,
                        int ConnectTimeoutMs,
                        const std::string &ServiceType,
                        const std::string &InstanceId);

/// Loads all service-registry:* entries from Redis in a single SCAN pass.
/// For each key matching "service-registry:*", performs a GET and deserialises
/// the value. Invalid, missing, or malformed entries are silently skipped.
///
/// Returns the collection of valid registry entries found.
/// Returns an empty vector on connection failure (graceful degradation).
/// Logs connection failures at warning level and entry-level errors at debug level.
[[nodiscard]] std::vector<Types::MicroServiceMeta>
LoadAll(const std::string &Host, int Port,
        const std::string &Password, int Db,
        int ConnectTimeoutMs,
        const std::atomic_bool *CancelSignal = nullptr);

} // namespace OpenWifi::ServiceRegistry
