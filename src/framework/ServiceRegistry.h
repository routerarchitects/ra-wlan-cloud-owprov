/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

//
// ServiceRegistry — Redis service-registry snapshot helpers for Phase 3 horizontal scaling.
//
// Shared Redis Contract:
//   - Key format:    service-registry:{service_type}:{instance_id}  (lowercase service_type)
//   - JSON payload fields:
//       * instanceId, type:                          Required, non-empty string (type lowercase)
//       * privateEndPoint, publicEndPoint, accessKey: Required, non-empty string
//       * lastUpdate:                                Required, positive integer (Unix epoch seconds > 0)
//       * version:                                   Optional string (defaults to empty string)
//   - Operations:    SET ... EX <ttl> on registration/keep-alive (default 60s); DEL on leave;
//                    SCAN 0 MATCH service-registry:* COUNT 100 + GET for bootstrap.
//   - Compatibility: Unknown JSON fields are ignored; key and payload service type and instance ID must match.
//
// Architectural rules:
//   - Redis is used ONLY for startup bootstrap snapshot seeding; Kafka remains authoritative.
//   - Operations are non-throwing and fail gracefully; passwords are never logged.
//   - Snapshot seeding runs after Kafka broadcast consumer starts, guarded by DepartedDuringBootstrap_.
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

/// Serialises a MicroServiceMeta to JSON for Redis storage according to the
/// shared Redis service-registration contract.
/// Only the fields needed to reconstruct a registry entry are stored.
/// Numeric legacy ID is intentionally omitted (Kafka wire format only).
[[nodiscard]] std::string Serialize(const Types::MicroServiceMeta &Meta);

/// Deserialises a MicroServiceMeta from a JSON string previously written by Serialize()
/// or by a peer service implementing the shared Redis service-registration contract.
/// Returns nullopt if the JSON is malformed, missing required fields, or has
/// an empty InstanceId, Type, PrivateEndPoint, PublicEndPoint, or AccessKey, or
/// if lastUpdate <= 0.
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
/// the value. Invalid, missing, or malformed entries are skipped without aborting.
/// If a Redis command/connection failure occurs during loading, the snapshot is marked
/// incomplete and loading stops gracefully.
///
/// Returns the collection of valid registry entries found up to any failure.
/// Returns an empty vector on initial connection failure (graceful degradation).
/// If IncompleteSnapshot is non-null, it is set to true if a Redis command or network
/// failure occurred (or false if the entire scan and all GETs completed cleanly).
[[nodiscard]] std::vector<Types::MicroServiceMeta>
LoadAll(const std::string &Host, int Port,
        const std::string &Password, int Db,
        int ConnectTimeoutMs,
        const std::atomic_bool *CancelSignal = nullptr,
        bool *IncompleteSnapshot = nullptr);

} // namespace OpenWifi::ServiceRegistry
