# OWPROV Horizontal Scaling Requirements

## 1. Purpose

This document defines the requirements OWPROV must satisfy before it can be safely run as multiple active instances in a Docker Compose deployment.

The purpose of this document is to agree on **what must be true** for OWPROV horizontal scaling before implementation starts.

Exact implementation details, class changes, Kafka registration APIs, database locking strategy and schema updates details will be defined later in `spec.md`.

---

## 2. Current Position

OWPROV can already be placed behind a load balancer at the HTTP/deployment layer.

However, being reachable through a load balancer is not the same as being safe for multi-instance operation.

Before OWPROV can be considered horizontally scalable, the service must not depend on process-local memory as an API decision source, per-process background jobs, per-process WebSocket state, unsafe Kafka delivery semantics, or unsafe concurrent database writes for correctness. Cached API reads must follow a shared Redis cache-aside model.

---

## 3. Target Multi-Instance Model

The target deployment model is:

```text
                +-------------------------+
API request --->|      Load balancer      |
                +-------------------------+
                    |        |        |
                    v        v        v
                owprov-1  owprov-2  owprov-3
                    |        |        |
                    +--------+--------+
                             |
              +--------------+--------+--------------+
              |                       |              |
              v                       v              v
          PostgreSQL                Redis          Kafka
     authoritative DB state     shared API cache   events / service discovery
```

**The expected behavior is**:

```text
1. More than one OWPROV instance can run at the same time.
2. Any incoming API request can be routed to any healthy OWPROV instance.
3. No user, device, job, or API request requires permanent routing to one specific OWPROV process.
4. Authoritative shared state is stored in PostgreSQL.
5. Cached API reads use Redis as the shared cache across OWPROV instances.
6. Kafka delivery behavior is defined per topic.
```

---

## 4. Scope

This requirements document covers OWPROV application behavior required for Docker Compose based horizontal scaling.

In scope:

```text
1. Multi-instance API correctness.
2. PostgreSQL source-of-truth behavior.
3. Shared Redis cache-aside behavior for cached API reads.
4. Removal of API dependency on stale process-local in-memory caches.
5. Cache invalidation after committed POST/PUT/DELETE operations.
6. Authorization correctness across replicas.
7. Kafka topic delivery semantics.
8. Database startup and schema initialization coordination.
9. Background job ownership and recovery.
10. Database write concurrency protection.
11. WebSocket and UI notification behavior across instances.
12. Runtime files and generated assets.
```
---


## 5. Global Requirements

### 5.1: Any healthy instance must be able to serve any API request

OWPROV must not require request stickiness for correctness.

**Required behavior**:

```text
1. A request for a device, venue, entity, subscriber, configuration, job, or user must not require routing to the instance that previously handled related work.
2. If a previous event was handled by owprov-1, a later read request routed to owprov-2 must still return correct shared state.
3. Load balancing across active OWPROV instances must not create correctness differences.
```

**Acceptance criteria**:

```text
1. Two OWPROV instances are running.
2. An operation is performed through one instance.
3. A read or follow-up operation through another instance observes correct shared state.
```

---
### 5.2: PostgreSQL must remain the source of truth, with Redis used for shared cached reads

OWPROV must use PostgreSQL as the authoritative source of truth for API data in multi-instance mode.

For API paths that require caching, OWPROV must use a shared Redis cache instead of process-local memory.

The required read model is:

```text
API read request
  -> check Redis shared cache
  -> if cache hit, use cached value
  -> if cache miss, read PostgreSQL
  -> populate Redis
  -> return result
```

The required write model is:

```text
POST / PUT / DELETE
  -> write PostgreSQL inside the required transaction boundary
  -> commit database change
  -> invalidate related Redis cache keys
  -> future reads reload fresh state from PostgreSQL on cache miss
```

Redis is a shared cache layer. PostgreSQL remains the permanent source of record.

**Required behavior**:

```text
1. Cached API read paths must use Redis as the shared cache across all OWPROV instances.
2. If a required record is not present in Redis, the API path must read the record from PostgreSQL.
3. PostgreSQL results loaded on cache miss may be written back to Redis using deterministic cache keys with a short, configurable TTL.
4. Write/update/delete APIs must persist changes to PostgreSQL before returning success.
5. After a successful PostgreSQL commit, OWPROV must invalidate all Redis cache keys affected by that write.
6. Redis must not be updated before the PostgreSQL transaction commits.
7. For normal data cache entries, if PostgreSQL commit succeeds but Redis invalidation fails, the API write must still return success; un-invalidated stale cache entries are bounded by the short TTL fallback, and invalidation errors are logged and monitored. Security-sensitive authorization cache entries follow the stricter invalidation policy in Section 6.2.
8. Redis is a cache layer, not a permanent source of record: if Redis is temporarily unavailable or unreachable, API reads must fall back directly to PostgreSQL or owsec authoritatively rather than failing readiness or serving stale process-local state. OWPROV operates without caching until Redis connectivity is restored.
9. API behavior must be based on committed PostgreSQL state and shared Redis cache state, not on which OWPROV instance receives the request.
10. Cache repopulation on read miss must not reintroduce stale data if a concurrent write committed in PostgreSQL: writers record the committed version/epoch in Redis alongside key invalidation (e.g. via an invalidation tombstone or version key), and readers attempting to repopulate on miss must check this marker and abort the write if their read version is older than the committed version.
```

**Acceptance criteria**:

```text
1. Data created/updated through owprov-1 is persisted in PostgreSQL.
2. Related Redis cache keys are invalidated after the PostgreSQL commit.
3. A later read through owprov-2 either reads fresh data from Redis or reloads it from PostgreSQL on cache miss.
4. If a Redis invalidation call fails after PostgreSQL commit, the API call returns success, an ERROR is logged, and stale Redis entries expire quickly via short TTL fallback.
5. If Redis is offline or unreachable, API reads continue to operate correctly by querying PostgreSQL and owsec directly without failing traffic acceptance or falling back to process-local caches.
6. Restarting one OWPROV instance does not change the data view of another instance.
7. API behavior is the same regardless of which OWPROV replica receives the request.
8. Verify that a slow read observing an older database snapshot cannot repopulate Redis with stale data if a concurrent write commits a newer version and invalidates the cache while the read was in-flight.
```

---

## 6. Shared Redis Cache Requirements

### 6.1: API behavior must not depend on process-local in-memory caches

OWPROV currently has process-local in-memory caches such as `AuthCache`, `AuthClient`, `SerialNumberCache`, `DeviceTypeCache`, or similar cache structures.

For multi-instance operation, these caches must not be used as process-local API decision sources.

API paths that require cached reads must follow the shared Redis cache-aside model defined in Section 5.2.

This section focuses on the cache-safety rule for existing process-local cache classes: they may remain only as wrappers around Redis shared caching or authoritative backend fallbacks, not as per-process authoritative API decision state.

**Required behavior:**

```text
1. API read paths must not rely on process-local AuthCache, AuthClient, SerialNumberCache, DeviceTypeCache, or similar in-memory caches as the decision source.
2. Cached API reads must follow the shared cache-aside model defined in Section 5.2.
3. API write/update/delete paths must persist required state to PostgreSQL.
4. After successful PostgreSQL commit, affected Redis cache keys must be invalidated.
5. Redis invalidation must not happen before the PostgreSQL transaction commits.
6. API behavior must not require local cache synchronization between OWPROV instances.
```

**Acceptance criteria:**

```text
1. API reads that previously used local caches are changed to follow the shared cache-aside model defined in Section 5.2.
2. Creating or updating data through owprov-1 does not require updating a process-local cache on owprov-2.
3. A read through owprov-2 observes fresh shared state without depending on owprov-2 process-local memory.
4. Restarting an OWPROV instance does not require rebuilding local API caches before API reads work correctly.
5. No API response depends on stale process-local cache state.
```

---

### 6.2: Authorization and authentication decisions must use Redis shared cache with authoritative backends

Authorization and authentication checks must not depend on which OWPROV instance receives the request, nor on process-local in-memory cache state. 

The architecture distinguishes two distinct categories of authorization and authentication state:
1. **OWPROV-Owned Authorization Data** (management roles, policies, entity scopes, permissions): Source of truth is OWPROV PostgreSQL. Cache misses reload from PostgreSQL. Invalidation occurs after PostgreSQL commit.
2. **Security-Service-Owned Authentication Data** (user session tokens `Authorization: Bearer <token>`, subscriber tokens validated via `AuthClient`): Source of truth is the OpenWiFi Security Microservice (`owsec`). Cache misses query `owsec` REST endpoints (`/api/v1/validateToken`, `/api/v1/validateSubToken`), NOT OWPROV PostgreSQL. When a token is revoked or a session is terminated, `owsec` directly invalidates (deletes) the corresponding shared Redis token-cache entry. Token invalidation does not depend on Kafka broadcast messages or cross-replica coordination.

**Required behavior:**

```text
1. Authorization and authentication API checks may read from Redis shared cache.
2. If OWPROV-owned authorization data is not present in Redis, the API path must read from PostgreSQL-backed state.
3. If token or API-key validation data is not present in Redis (cache miss), AuthClient must call the Security service (owsec) REST API, not PostgreSQL, and cache the validation result in Redis with a TTL that must never exceed the token's remaining lifetime.
4. Token revocation and session termination invalidations are the direct responsibility of owsec, which deletes the shared Redis token-cache entry. Token invalidation must not depend on Kafka offsets, consumer availability, or EVENT_REMOVE_TOKEN delivery. If owsec cannot delete the Redis token-cache key, owsec must record and retry the invalidation or otherwise prevent the revoked token from validating successfully.
5. Permission, role, policy, and management-scope changes owned by OWPROV must be persisted in PostgreSQL and invalidate affected Redis keys after commit.
6. Authorization and authentication behavior must not require process-local AuthCache or AuthClient cache synchronization between OWPROV instances.
7. Security-sensitive cache entries must follow a stricter invalidation policy than normal data cache entries:
   - For normal inventory, display, and metadata caches, Redis invalidation failure after a successful PostgreSQL commit may return success if the stale window is bounded by a short TTL.
   - For authorization-sensitive data, including permissions, roles, policies, token validation, and token revocation, Redis invalidation failure must not be treated as ordinary bounded staleness. The system must record a shared durable pending invalidation and retry with backoff until the stale authorization entry is removed.
   - The authoritative revision/epoch is maintained in PostgreSQL (or owsec). When caching in Redis, authorization entries validate against this active revision/epoch so that an older DB read cannot re-populate or re-authorize stale roles or permissions in the cache. If Redis is unavailable, authorization validates directly against PostgreSQL or owsec.
   - While a security-sensitive invalidation is pending or uncertain, OWPROV must not authorize requests only from the stale Redis entry. It must fall back to authoritative validation or fail closed.
```

**Acceptance criteria:**

```text
1. Start owprov-1 and owprov-2.
2. Change or revoke a user's management role/permission through owprov-1:
   - Confirm the change is persisted in PostgreSQL.
   - Confirm affected Redis authorization cache keys are invalidated.
   - Send an API request requiring that permission to owprov-2; confirm owprov-2 authorizes or rejects the request using Redis shared cache or PostgreSQL reload, not local AuthCache state.
3. Revoke a user session token or log out in owsec:
   - owsec directly deletes the shared Redis token-cache key.
   - A subsequent request with that token to any OWPROV instance results in a Redis cache miss, re-validates against owsec, and is rejected.
   - Invalidation correctness does not depend on Kafka consumer state, consumer group lag, or EVENT_REMOVE_TOKEN broadcast delivery.
4. Verify that cached token entries in Redis have a TTL bounded by the token's remaining lifetime, ensuring expired tokens cannot remain cached even if explicit revocation was not triggered.
5. Simulate Redis DEL failure after an OWPROV-owned permission or role revocation. Verify the database change commits, a shared durable security-sensitive invalidation retry is recorded, and no OWPROV replica authorizes using the stale permission cache entry.
6. Verify that an older database read for authorization data cannot overwrite or re-populate a newer committed permission or role change in Redis.
```

---

### 6.3: Serial number and inventory behavior must use Redis cache with PostgreSQL-enforced correctness

Serial number and inventory read/search behavior may use Redis shared cache.

Serial number uniqueness, duplicate prevention, and durable inventory correctness must still be enforced through PostgreSQL.

**Required behavior:**

```text
1. Serial number read/search paths may use Redis shared cache.
2. Redis cache misses must be loaded from PostgreSQL.
3. Serial number uniqueness checks must be enforced through PostgreSQL constraints, transactions, or safe database-level checks.
4. Duplicate prevention must not depend on local process memory or Redis alone.
5. Inventory write/update/delete paths must persist to PostgreSQL and invalidate affected Redis keys after commit.
6. Database constraints must be authoritative where uniqueness matters.
```

**Acceptance criteria:**

```text
1. A serial created through owprov-1 cannot be duplicated through owprov-2.
2. A serial removed through owprov-1 is not treated as valid by owprov-2 because of stale local memory.
3. Inventory search results are consistent across replicas for the same committed database state.
4. Redis cache misses reload serial/inventory data from PostgreSQL.
5. Redis keys affected by inventory writes are invalidated after PostgreSQL commit.
```


---

## 7. Database Requirements

### 7.1: Database startup and schema initialization must be single-owner coordinated

When multiple OWPROV instances start at the same time, database initialization and schema setup must not race.

**Required behavior:**

```text
1. Only one OWPROV instance may run the protected database startup/initialization section at a time.
2. Other instances must wait or fail readiness cleanly until the database is safe to use.
3. If the owner instance crashes before completing startup, another instance must be able to retry safely.
4. The coordination mechanism must be based on PostgreSQL/shared database ownership, not Kafka group leadership.
```

**Acceptance criteria:**

```text
1. Starting two or more OWPROV instances concurrently does not cause schema or initialization races.
2. Only one instance performs a schema transition at a time.
3. Other instances do not accept traffic before required database initialization is safe.
4. Startup failure is visible.
```

---

### 7.2: Concurrent database writes must not silently lose updates

OWPROV must protect correctness-sensitive read-modify-write paths.

Problem example:

```text
Initial Venue.devices = [DeviceX]

owprov-1 reads old state and adds DeviceA
owprov-2 reads old state and adds DeviceB

owprov-1 writes [DeviceX, DeviceA]
owprov-2 writes [DeviceX, DeviceB]

Final state can lose DeviceA.
```

**Required behavior:**

```text
1. Relationship updates must be concurrency-safe.
2. Check-then-write paths must be protected by database constraints, transactions, row locks, optimistic versions, atomic updates, or equivalent.
3. Multi-record operations must define transaction boundaries.
4. Retryable conflicts must be handled explicitly and not converted into false success.
```

**Acceptance criteria:**

```text
1. Two concurrent writes to the same relationship cannot silently lose either update.
2. Duplicate or conflicting writes are rejected, retried, or resolved according to explicit rules.
3. Database constraints, not local memory, are authoritative for uniqueness where correctness requires it.
```

---

## 8. Kafka Requirements

### 8.1: Kafka topic delivery semantics must be explicit

Every Kafka topic consumed by OWPROV must be assigned to the correct Kafka consumer type before multi-instance deployment is enabled.

OWPROV must support two consumer types:

```text
1. GroupConsumer:
   Used for work-queue topics.
   One replica processes each message in the shared service group.

2. BroadcastConsumer:
   Used for broadcast/fan-out topics.
   Every replica receives each message through its own instance-specific consumer group.
```

**Required behavior:**

```text
1. Each consumed Kafka topic must be assigned to either GroupConsumer or BroadcastConsumer.
2. "service_events" must be registered on BroadcastConsumer.
3. "connection" must be registered on GroupConsumer.
4. GroupConsumer must use the shared group.id so one message is processed by one instance.
5. BroadcastConsumer must use an incarnation-unique group.id generated per runtime process lifetime (e.g., group.id = prov-<incarnation-id>-broadcast using a runtime UUID or slot-qualified UUID) with auto.offset.reset = latest. Consumer group IDs must not be reused across overlapping container restarts, because Kafka would treat overlapping old and new replicas as members of the same consumer group and divide partitions between them instead of fanning out all messages to both. An instance must not use Kafka history replay from earliest as the service-discovery bootstrap mechanism; it may seed its local discovery view from the shared Redis service registry snapshot when available and must continue maintaining discovery state from live service_events.
6. Kafka client.id must also be unique per runtime instance for logs and observability (incorporating the runtime incarnation ID).
```

**Acceptance criteria:**

```text
1. "service_events" is assigned to BroadcastConsumer.
2. Every running instance receives service_events messages, including during rolling updates and overlapping container restarts.
3. "connection" is assigned to GroupConsumer.
4. Each "connection" message is processed by only one instance in the service group.
5. State written as a result of connection processing is stored in PostgreSQL and can be read by any OWPROV instance through the shared read model defined in Section 5.2.
6. No OWPROV-consumed Kafka topic is left without an explicitly assigned consumer type.
```

---

### 8.2: service_events must be delivered to every instance

`service_events` is a broadcast topic providing dynamic service discovery data across microservices.

**Required behavior:**

```text
1. Every running instance with an active BroadcastConsumer must receive live service join, keep-alive, and leave events published after it starts consuming.
2. Every instance must maintain a compatible service discovery view.
3. Service discovery registries across OpenWiFi microservices must track active replicas by instance identifier so that an individual replica's departure does not unregister a shared load-balanced service endpoint while other replicas remain active.
4. Token invalidation is handled directly by owsec deleting shared Redis token cache keys upon revocation, decoupling authentication correctness from Kafka broadcast delivery.
5. One instance consuming a service event must not prevent other instances from receiving it.
6. Service discovery state must not be initialized by replaying historical service_events from earliest. A newly started instance may seed its local discovery view from the shared Redis service registry snapshot when Redis is available, and must maintain its runtime discovery view from live service_events consumed from latest/current offset.
7. Redis service-registry snapshot unavailability must not fail service-discovery bootstrap by itself; however, the instance must not pass readiness until required upstream services are present in its local Services_ view, whether learned from Redis snapshot seeding or live JOIN/KEEP_ALIVE events.
```

**Acceptance criteria:**

```text
1. Start owprov-1 and owprov-2 behind a shared private endpoint.
2. Publish or trigger service_events.
3. Verify both instances receive and process the same service event.
4. Shutting down owprov-1 removes only owprov-1 from the peer discovery view without dropping the shared OWPROV service registration while owprov-2 is running.
5. Verify internal service lookup does not fail on one replica only because another replica consumed the event.
6. A newly started replica does not replay historical service_events from earliest and can seed its local discovery view from the shared Redis registry snapshot when available.
7. If the Redis registry snapshot is unavailable, verify the replica can still build its local discovery view from live JOIN and KEEP_ALIVE events consumed from service_events latest/current offset, and does not pass readiness until required upstream services are discovered.
```

---

### 8.3: connection events must be safe for work-queue processing

The `connection` topic may be processed as a work-queue topic only if OWPROV writes the durable result to PostgreSQL and later API calls use the shared read model defined in Section 5.2.

**Required behavior:**

```text
1. Each connection event should be processed by one instance in the OWPROV service group.
2. The processing instance must write required durable device/inventory state to PostgreSQL.
3. Later API calls for that device must be routable to any instance and must use the shared read model defined in Section 5.2.
4. No device should become permanently owned by the instance that processed its connection event.
5. Connection processing must be idempotent under retry, rebalance, or duplicate delivery.
```

**Acceptance criteria:**

```text
1. A connection event processed by owprov-1 is visible through API reads served by owprov-2.
2. Replaying the same connection event does not create duplicate inventory or conflicting state.
3. Rebalancing consumers does not create inconsistent device state.
```

---

### 8.4: Kafka producer partitioning must support scale-out where required

Kafka producer behavior must not prevent partition-based scale-out for scalable work topics.

**Required behavior:**

```text
1. Work topics that require ordering per device should use a stable message key such as serial number, MAC address, or device UUID.
2. Messages for the same device should preserve ordering when required.
3. Messages for different devices should be able to distribute across partitions.
4. Forcing all scalable topic messages to one partition must not be used unless explicitly justified.
```

**Acceptance criteria:**

```text
1. connection events for the same device are ordered through the same partition where ordering is required.
2. connection events for different devices can be distributed across multiple partitions.
3. Multiple OWPROV instances can actively consume work when the topic has enough partitions.
```

---

## 9. Service Identity Requirements

### 9.1: Service identity must distinguish logical service identity, slot identity, and runtime incarnation identity

Horizontal scaling requires a clear distinction between shared service identity, optional logical slot identity, and individual runtime incarnation identity.

**Required behavior:**

```text
1. The deployment must distinguish between:
   - Logical service identity: Shared public and internal endpoints representing the OWPROV cluster behind the load balancer.
   - Logical slot identifier (optional, non-normative): Administrative or orchestration slot labels (e.g., StatefulSet pod name or illustrative examples like "owprov-1", "owprov-2"). Implementations must not treat slot names as hardcoded or assume they are globally unique across overlapping deployment lifecycles.
   - Runtime incarnation identity: A collision-resistant unique identifier (UUID) generated per process/container execution lifecycle.
2. The runtime incarnation ID (or a composite including the slot identifier and runtime UUID) must serve as the unique instance identity for Kafka client IDs, broadcast consumer group IDs, job lease owners, and service_events ID.
3. BroadcastConsumer group.id must use the runtime incarnation ID (e.g., prov-<incarnation-id>-broadcast). Broadcast groups must not be statically reused across container restarts so that overlapping old and replacement replicas never share a broadcast consumer group or divide partitions during rolling restarts.
4. The service_events ID field must uniquely identify the individual runtime incarnation so two simultaneously running replicas (such as an old replica draining while a new replica initializes) never emit the same ID or collide in Redis service-registry keys.
5. The public OWPROV service endpoint must remain stable behind the load balancer.
6. If private per-instance endpoints are advertised, they must not collide and must be reachable by intended peers.
7. Internal API key/hash behavior must remain consistent where it depends on shared public endpoint configuration.
```

**Acceptance criteria:**

```text
1. Logs clearly identify which OWPROV instance and runtime incarnation produced each entry.
2. Kafka clients can be distinguished per running instance and incarnation.
3. Two simultaneously running replicas (including overlapping replicas during rolling restarts) emit distinct service_events IDs and maintain separate Redis service-registry keys.
4. Overlapping replicas during rolling restarts do not share Kafka broadcast consumer group IDs, ensuring both replicas receive all broadcast messages without partition division.
5. Service discovery does not accidentally collapse multiple replicas into ambiguous or conflicting records.
```

---

## 10. Runtime Downloaded File Requirements

### 10.1: Runtime downloaded files must remain consistent across OWPROV instances

OWPROV may download required files into its local data directory during startup or runtime.

In a multi-instance Docker Compose deployment, each OWPROV container may have its own local data directory. This is acceptable as long as every instance downloads the same required files from the same configured source.

A shared data directory is not required for this requirement.

**Required behavior:**

```text
1. Each OWPROV instance may download required files into its own local data directory.
2. All OWPROV instances must use the same configured download source for these files.
3. Downloaded files must not be manually changed differently on different OWPROV instances.
4. If a required file cannot be downloaded or validated, that OWPROV instance must not become ready to serve dependent API behavior.
5. A newly started OWPROV instance must be able to download the required files independently without needing files copied from another OWPROV instance.
```

Acceptance criteria:

```text
1. Start owprov-1 and owprov-2 with the same download source configuration.
2. Both instances download the required files into their own local data directories.
3. The downloaded files used by both instances are equivalent.
4. Requests depending on those files behave the same regardless of which OWPROV instance receives the request.
5. If one instance fails to download a required file, it does not become ready for API behavior that depends on that file.
```

---

## 11. Conditional Rate Limiting Requirements

### 11.1: Rate limiting must be multi-instance safe when enabled

Rate limiting is not a default requirement for every OWPROV API. This requirement applies only to APIs where rate limiting is enabled or where rate limiting is used as a security, abuse-prevention, or user/API protection mechanism.

A process-local rate limiter gives each OWPROV instance independent counters. In a multi-instance deployment, this means the effective limit can change when traffic is spread across multiple OWPROV replicas.

**Required behavior:**

```text
1. APIs with rate limiting enabled must clearly define whether the limit is local per-process protection or global user/API protection.
2. Local per-process rate limiting may be used only for local overload protection.
3. Global user/API rate limiting must not rely only on per-process OWPROV memory.
4. If global rate limiting is required, it must be enforced at the load balancer layer.
```

**Acceptance criteria:**

```text
1. APIs without rate limiting enabled are not blocked by this requirement.
2. Any API with rate limiting enabled documents whether the limit is local or global.
3. If an API requires global rate limiting, the effective limit does not change when OWPROV instance count changes.
4. If the limit is only local overload protection, the documentation clearly says it is not a global user/API limit.
```

---

## 12. Background Job Requirements

### 12.1: Background jobs must have durable ownership, recovery, and idempotency

Background jobs must not be owned only by the OWPROV process that received the REST request.

Required behavior:

```text
1. Long-running actions must create durable job state in PostgreSQL.
2. Job state must include job id, job type, parameters, owner_user_id (for access control), owner_email, status, lease_generation (monotonically increasing fencing token), lease_expires_at, progress, and result details.
3. Instances must claim and reclaim pending or expired jobs through an atomic shared-state operation that increments lease_generation.
4. Only one instance may own a job at a time.
5. All heartbeat, progress, and terminal status updates must be fenced using the active lease_generation and owner_instance_id; if the lease was stolen during a worker pause/stall, updates must fail (0 rows modified) and the stale worker must immediately abort execution.
6. Device operations (reboots, firmware upgrades, config pushes) must use operation/job idempotency keys (operation_id) and explicit per-device progress state transitions (pending -> dispatching -> dispatched/acknowledged -> completed or failed). owgw and downstream device-facing services must durably de-duplicate commands by operation_id across retries and restarts so stalled workers or recovered jobs do not re-execute commands on already-completed devices.
7. A crashed or stopped owner must not lose the job permanently; another instance may reclaim an expired job up to max_attempts.
8. Kafka may be used to wake workers, but Kafka group leadership must not be the source of truth for job ownership.
9. Job status query endpoint GET /api/v1/jobs/{id} must restrict access to the matching owner_user_id or callers with explicit admin/support permission for the job's scope, and job result payloads must contain only necessary fields without secrets or configuration bodies.
```

Acceptance criteria:

```text
1. A job created through owprov-1 can be queried through owprov-2.
2. If owprov-1 stops while owning a job, owprov-2 can observe and reclaim the job using an incremented lease_generation fencing token.
3. If owprov-1 pauses and resumes after its lease has been reclaimed by owprov-2, owprov-1 cannot overwrite job state or issue duplicate device commands.
4. A job is not blindly executed twice during restart, rebalance, or retry, and in-flight device operations progress through explicit states (pending -> dispatching -> dispatched/acknowledged -> completed).
5. Job progress and terminal state survive process restart.
6. A job status read via GET /api/v1/jobs/{id} allows access only to the matching owner_user_id or a caller with authorized admin/support access to the job's resource scope, returning minimized result summaries without secrets.
```

---

## 13. WebSocket And Notification Requirements

### 13.1: UI notifications and job status must be deliverable across different instances

A browser or UI client may be connected to one OWPROV instance while an API action or background job runs on another instance.

Required behavior:

```text
1. Progress and completion for work executed on owprov-2 must be deliverable to a client connected to owprov-1 through either cross-instance WebSocket fan-out or durable job-status polling.
2. WebSocket connection locality must not cause required notifications or job status updates to be silently lost.
3. Sticky WebSocket routing may help connection stability but must not be the only correctness mechanism.
4. Notification and status delivery expectations must be documented for each important event type.
5. Cross-instance notification fan-out must include an ownership envelope (owner_user_id, owner_email, job_id, notification_type), and receiving instances must filter and deliver only to local WebSockets matching the authenticated owner identity.
```

Acceptance criteria:

```text
1. Connect a UI client to owprov-1.
2. Trigger a job or action through owprov-2.
3. Verify required progress/completion information is available to the client through either cross-instance WebSocket fan-out or durable job-status polling.
4. Cross-instance notification broadcast does not deliver user- or job-specific notifications to clients with non-matching owner identities.
```

---


## 14. Docker Compose Deployment Requirements

### 14.1: Docker Compose deployment must support active-active OWPROV safely

The target deployment environment for this phase is Docker Compose.

Required behavior:

```text
1. Multiple OWPROV containers must be able to run at the same time.
2. All OWPROV instances must use the same PostgreSQL database.
3. All OWPROV instances must use the same Redis shared cache.
4. All OWPROV instances must use the same Kafka cluster.
5. Each OWPROV instance must have unique instance identity where required.
6. The public OWPROV endpoint must be load-balanced through the Nginx load balancer.
7. Health/readiness behavior must prevent instances from receiving traffic until core dependencies (PostgreSQL, Kafka, and required upstream microservices) are ready.
8. Shutdown must stop accepting new traffic before terminating long-running work where possible.
9. Instance-specific environment values must not conflict across replicas.
```

Acceptance criteria:

```text
1. At least two OWPROV instances run in Docker Compose.
2. Requests can be routed to either instance.
3. The same API read request returns consistent shared-state results from either instance.
4. An instance does not become ready for traffic until required upstream dependency services are discovered.
5. Restarting one instance does not corrupt shared state or lose durable work.
```

---

## 15. High-Level Acceptance Criteria

OWPROV horizontal scaling is acceptable when:

```text
1. Multiple OWPROV instances can run at the same time in Docker Compose.
2. Any healthy OWPROV instance can serve API requests without sticky routing.
3. PostgreSQL remains the source of truth for API data.
4. Cached API reads use the shared Redis cache-aside model.
5. API write/update/delete operations commit to PostgreSQL and invalidate affected Redis keys after commit.
6. API behavior does not depend on process-local AuthCache, SerialNumberCache, DeviceTypeCache, or similar in-memory caches.
7. Kafka topics consumed by OWPROV are registered on the correct consumer type.
8. service_events is received by every running OWPROV instance with an active BroadcastConsumer.
9. connection messages are processed by only one OWPROV instance in the service group.
10. Database startup and schema initialization do not race between instances.
11. Concurrent database writes do not silently lose updates.
12. Background jobs can be observed and recovered through durable state.
13. Runtime-downloaded files remain consistent across instances.
14. Conditional rate limiting behavior is defined only for APIs where rate limiting is enabled.
```
---

## 16. Review Checklist

Before `spec.md` or implementation begins, reviewers should confirm:

```text
1. The requirements correctly describe Docker Compose multi-instance OWPROV.
2. Kubernetes/Helm topics are intentionally excluded.
3. PostgreSQL source-of-truth behavior is accepted.
4. Redis shared cache-aside behavior is accepted for cached API reads.
5. Cache invalidation after committed POST/PUT/DELETE operations is accepted.
6. Removing correctness dependency on process-local caches is accepted.
7. Kafka Group vs Broadcast semantics are accepted as a requirement.
8. Background job durable ownership is accepted as a requirement.
9. Database startup coordination is accepted as a requirement.
10. Database write concurrency protection is accepted as a requirement.
11. WebSocket/notification cross-instance behavior is accepted as a requirement.
12. Runtime file handling is accepted as a requirement.

```
## 17. Non-Goals

This document does not define:

1. Exact code changes, class changes, method names, or implementation order.
2. Exact database schema, migration SQL, or locking implementation.
3. Exact KafkaManager class changes, Kafka registration APIs, or consumer internals.
4. Exact background job table schema, worker implementation, or retry algorithm.
5. Exact WebSocket fanout mechanism or notification transport.
6. Exact Docker Compose YAML, Nginx load balancer configuration, or port mappings.
7. Kubernetes, Helm, or Kubernetes-specific deployment behavior.
8. Request stickiness between the Nginx load balancer and OWPROV instances.
9. Replacing PostgreSQL as the source of truth for OWPROV API data.
10. Using Redis as the durable source of truth instead of PostgreSQL.
11. A shared data directory for runtime-downloaded files, as long as each instance independently downloads and validates equivalent files.
12. Enabling rate limiting for every OWPROV API.
13. Performance tuning, benchmarking targets, or capacity planning.
14. Public API behavior changes unless later required by `spec.md`.



