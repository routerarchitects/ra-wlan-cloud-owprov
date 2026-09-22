# OWPROV Horizontal Scaling Specification

## 1. Overview

This document defines the implementation specification for running OWPROV as multiple active instances in a Docker Compose deployment.

This specification is based on `requirements.md`. The requirements document defines what OWPROV must satisfy before horizontal scaling is allowed. This specification defines how the first implementation should satisfy those requirements.

The goal is to make OWPROV safe to run as:

```text
Load balancer
  -> owprov-1
  -> owprov-2
  -> owprov-3

Shared dependencies:
  -> PostgreSQL
  -> Redis
  -> Kafka
```

OWPROV instances must behave as one logical OWPROV service while each instance remains individually identifiable for logs, Kafka clients, service events, and shutdown behavior.

---

## 2. Design Goals

The horizontal scaling implementation must provide:

```text
- active-active OWPROV instances
- no REST/API request stickiness requirement
- shared Redis cache-aside model with PostgreSQL as the source of truth
- no API dependency on process-local AuthCache, AuthClient (Cache_/ApiKeyCache_), SerialNumberCache, DeviceTypeCache, or similar in-memory caches
- cache invalidation after committed POST/PUT/DELETE operations
- explicit Kafka delivery behavior per topic
- safe database startup coordination
- safe database writes under concurrent access
- durable background job ownership and recovery
- cross-instance WebSocket/UI notification delivery
- consistent runtime-downloaded file behavior
- Docker Compose based deployment support
```

The implementation must keep the first phase focused on Docker Compose. Kubernetes and Helm behavior are intentionally outside this phase.

---

## 3. Target Runtime Model

The target Docker Compose deployment is:

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
              +--------------+-----------+--------------+
              |                          |              |
              v                          v              v
          PostgreSQL                   Redis          Kafka
     authoritative API data     shared API cache   events and async delivery
```

Each OWPROV instance must use the same:

```text
- PostgreSQL database
- Redis shared cache
- Kafka cluster
- shared public OWPROV endpoint
- shared service configuration where logical service identity is required
```

Each OWPROV instance must have a unique:

```text
- runtime incarnation id (UUID, with optional logical slot identifier)
- Kafka client id (incorporating runtime incarnation id)
- log/metric identity
- broadcast consumer group id per runtime incarnation, where broadcast Kafka behavior is required
```

---

## 4. Permanent Boundaries

This implementation must not introduce the following assumptions:

```text
- API request stickiness is required for normal API behavior.
- A user, device, job, or WebSocket permanently belongs to one OWPROV instance.
- Process-local memory is used as the source for API read, validation, authorization, search, or response decisions. Cached API reads must follow the shared cache-aside model defined in Section 6.
- Process-local background jobs are the only source of job state.
- Local disk from one OWPROV container is required by another OWPROV container.
- Kafka consumer group leadership is used as the owner of database startup or background jobs.
- Kubernetes or Helm behavior is required for the Docker Compose phase.
```

---

## 5. Configuration Specification

### 5.1 Instance identity: logical slot ID and runtime incarnation ID

To support horizontal scaling, rolling updates, and active-active replicas without collision, OWPROV distinguishes between:

1. **Logical slot / host identifier** (`OWPROV_SLOT_ID` or optional configured prefix, e.g. `owprov-1`, `owprov-2`):
   An optional administrative or orchestration slot identifier (such as container hostname, Kubernetes pod name, or Compose service slot). The labels `owprov-1`, `owprov-2` used throughout this document are non-normative illustrative examples. Deployments and implementations must not assume these names are hardcoded, static, or globally unique across overlapping deployment lifecycles.

2. **Runtime incarnation ID** (UUID):
   A collision-resistant unique identifier (such as UUID v4) generated dynamically at process/container startup. It represents the specific runtime execution lifecycle of that process.

3. **Composite / Runtime Instance Identity**:
   For runtime mechanisms that require absolute process-level uniqueness—specifically Kafka BroadcastConsumer group IDs, Kafka client IDs, `service_events` registration IDs, Redis service registry keys, and background job lease fencing—the runtime incarnation ID (or a composite such as `<slot-id>-<incarnation-uuid>`) serves as the unique instance identity.

This separation guarantees that during rolling restarts or replacement deployments—where an old instance (`old owprov-1`) is draining while a replacement instance (`new owprov-1`) is starting up—the two replicas possess distinct runtime identities and do not collide in Kafka consumer groups or service discovery registries.

Scope of usage:

```text
- Logical Slot ID (if configured):
  - operator-facing log prefixes
  - deployment slot metrics labels
  - orchestration placement tracking

- Runtime Incarnation ID (or composite <slot-id>-<incarnation-uuid>):
  - Kafka BroadcastConsumer group.id (ensures true fan-out with no partition division during overlapping restarts)
  - Kafka client.id values
  - service_events ID field and Redis key (service-registry:{service_type}:{instance_id})
  - background job lease owner identity
  - readiness, lifecycle, and shutdown messages
```

### 5.2 Shared public endpoint

All OWPROV instances must use the same public OWPROV endpoint.

Example:

```text
openwifi.system.uri.public=https://owprov.example.com:16005
```

The public endpoint represents the logical OWPROV service, not one specific instance.

### 5.3 Private/internal endpoint

For the Docker Compose phase, OWPROV internal service-to-service traffic should use a shared internal load-balanced endpoint.

Example:

```text
openwifi.system.uri.private=https://owprov-internal:17005
```

If per-instance private endpoints are introduced later, they must be reachable by intended peer services and must not collide.

### 5.4 Kafka client identity

Kafka `client.id` must be unique per runtime instance incarnation and per consumer role so that overlapping replicas during restarts do not collide.

Example (incorporating runtime incarnation UUID):

```text
owprov-1-<incarnation-uuid>-work-consumer
owprov-1-<incarnation-uuid>-broadcast-consumer
owprov-1-<incarnation-uuid>-producer

owprov-2-<incarnation-uuid>-work-consumer
owprov-2-<incarnation-uuid>-broadcast-consumer
owprov-2-<incarnation-uuid>-producer
```

`client.id` is for observability and must not be used as a substitute for delivery semantics.

### 5.5 Redis shared cache configuration

All OWPROV instances must use the same Redis instance or Redis cluster for shared cached API reads.

Example:

```text
openwifi.redis.host=redis
openwifi.redis.port=6379
openwifi.redis.cache.ttl=60
```

Redis is used only as the shared cache layer; PostgreSQL remains the source of truth.

Required behavior:

```text
1. Every OWPROV instance uses the same Redis cache namespace.
2. Redis keys are deterministic and do not include process-local instance identity unless the data is intentionally instance-scoped.
3. Cache misses reload data from PostgreSQL.
4. POST/PUT/DELETE handlers invalidate affected Redis keys only after successful PostgreSQL commit.
5. Redis failure must not cause stale process-local cache fallback.
6. Cached entries must use a short, configurable TTL fallback to limit bounded staleness if invalidation fails.
7. Redis invalidation failures must be logged and monitored without failing committed DB writes.
```

---

## 6. API Data Access And Shared Cache Specification

### 6.1 PostgreSQL source of truth

PostgreSQL remains the authoritative source of truth for OWPROV API data in multi-instance mode.

Redis may be used for shared cached reads, but Redis must not become the durable source of OWPROV data.

Implementation rules:

```text
1. API write/update/delete handlers must persist required domain changes to PostgreSQL before returning success.
2. PostgreSQL transaction boundaries must be defined before related Redis cache invalidation happens.
3. A follow-up request routed to another OWPROV instance must observe the committed PostgreSQL state through Redis or PostgreSQL fallback.
4. API behavior must not depend on which OWPROV instance handled the previous request.
5. Restarting one OWPROV instance must not change the API data visible from another OWPROV instance.
```

### 6.2 Redis cache-aside read behavior

Cached API read paths must use Redis as a shared cache across OWPROV instances.

Read path:

```text
1. Build deterministic Redis cache key.
2. Read from Redis.
3. If Redis has the value, use it.
4. If Redis does not have the value, read from PostgreSQL.
5. Store the PostgreSQL result in Redis where caching is allowed, including its record version/generation (e.g. modified timestamp or revision epoch).
6. Return the API result.
```

Implementation rules:

```text
1. Redis keys must be deterministic and shared across OWPROV instances.
2. Cache values must represent PostgreSQL-backed data or data derived from PostgreSQL-backed state.
3. Cache misses must reload from PostgreSQL.
4. Cache TTLs must be short and configurable per data type to ensure bounded staleness.
5. API behavior must not fall back to process-local cache state when Redis misses.
6. Stale repopulation prevention: Cached entries must include a version/generation marker (e.g., record modified timestamp or revision epoch). When Redis is available, a writer that commits version V2 records this committed version in Redis (via an invalidation tombstone or version key); a reader with version V1 attempting to repopulate on cache miss checks this version marker and aborts the write if V1 < V2. For normal data caches, if Redis was unavailable during the commit so the version marker could not be written, stale repopulation upon recovery is explicitly acceptable and bounded by the configured TTL. For security-sensitive authorization data, the stronger guarantee is maintained: authorization data strictly derives from the authoritative PostgreSQL/owsec revision epoch and stale repopulation is never permitted.
```

### 6.3 Write behavior and cache invalidation

Write paths must update PostgreSQL first and invalidate Redis after successful commit.

Write path:

```text
1. Validate request.
2. Start required PostgreSQL transaction.
3. Write PostgreSQL changes (updating record version/generation).
4. Commit PostgreSQL transaction.
5. Invalidate affected Redis cache keys and record the committed version/epoch in Redis (e.g. via an invalidation tombstone or version marker) so older in-flight reads cannot repopulate stale data.
6. Return API response.
```

Implementation rules:

```text
1. Redis must not be updated before PostgreSQL commit.
2. Failed PostgreSQL writes must not invalidate or overwrite Redis cache entries.
3. POST/PUT/DELETE handlers must identify affected Redis keys.
4. The preferred first implementation is cache invalidation, not direct Redis mutation.
5. The next read repopulates Redis from PostgreSQL on cache miss.
6. Post-commit version tracking: When Redis is available, writers record the newly committed version/epoch in Redis alongside key invalidation (e.g. setting an invalidation tombstone or version marker with the committed timestamp/version); readers attempting to repopulate on miss compare their DB read's version against this marker in Redis and drop the write if their read is older than the committed version. Writes update the record version/generation in PostgreSQL before commit. For normal data caches across a Redis outage or invalidation failure, stale repopulation is explicitly acceptable for the duration of the configured TTL. For authorization data, the stronger guarantee is enforced via PostgreSQL/owsec revision epochs.
```

### 6.3.1 Invalidation failure handling and bounded staleness

For normal data cache entries (inventory, display, metadata), when a write operation succeeds in PostgreSQL, OWPROV invalidates the corresponding Redis cache key(s). If the Redis invalidation call fails (e.g., due to temporary network partition, socket timeout, or Redis command error), OWPROV handles the failure safely:

```text
PostgreSQL Commit Succeeded -> Redis Invalidation Failed (Normal Data):
  1. Return HTTP Success (200/201/204) to the client.
  2. Log ERROR with affected cache keys and failure reason.
  3. Increment invalidation failure metric.
  4. Stale cache entry expires quickly via short configurable TTL fallback.
  5. Subsequent read reloads fresh data from PostgreSQL.
```

Implementation rules:

```text
1. For normal data writes, successful DB writes must not return API errors if Redis invalidation fails. PostgreSQL has already durably committed the change; returning an HTTP error would mislead callers and risk dangerous duplicate non-idempotent retries.
2. Normal Redis cache entries must be written with a short, configurable TTL (e.g., a short safety window such as 10–60 seconds, configurable via openwifi.redis.cache.ttl or per domain) rather than long or indefinite durations.
3. The short TTL acts as a bounded staleness fallback: in the event of an individual invalidation failure, stale entries expire quickly on their own without requiring complex background retry queues or outbox processing.
4. If Redis is unreachable or temporarily offline, API reads must fall back to querying PostgreSQL or owsec directly rather than failing readiness or falling back to process-local cache state. The instance serves requests without caching until Redis connectivity is restored.
5. Invalidation failures must emit ERROR logs with affected keys and increment owprov_redis_invalidation_failures_total for operator visibility.
6. Stale repopulation across Redis outages: If a write commits in PostgreSQL while Redis is unreachable, the Redis invalidation and version marker cannot be recorded. For normal data (inventory, display, metadata), stale repopulation upon Redis recovery is explicitly acceptable and treated as bounded staleness expiring within the configured short TTL (10–60 seconds). For security-sensitive authorization data, the stronger guarantee is maintained: authorization state derives authoritatively from PostgreSQL/owsec revision epochs, preventing stale authorization data from ever being accepted or cached.
```

### 6.3.2 Security-sensitive cache invalidation

Redis cache usage is divided into two distinct cache policy classes:

1. Normal data cache
   - Used for inventory, display, metadata, and non-authorization data.
   - PostgreSQL remains the source of truth.
   - Readers do not perform double DB validation queries before setting cache; during normal operation, Redis version markers prevent stale repopulation.
   - After a successful PostgreSQL commit, if Redis invalidation fails or Redis was offline, stale repopulation upon recovery is explicitly acceptable and treated as bounded staleness expiring within the short TTL (`openwifi.redis.cache.ttl`, 10–60s).

2. Security-sensitive authorization cache
   - Used for permissions, roles, management policies, entity/venue scopes, user token validation, and token revocation state.
   - Distinct ownership boundaries:
     - **OWPROV-Owned Authorization Data** (roles, permissions, entity/venue scopes, management policies): OWPROV PostgreSQL is the source of truth and maintains an authoritative revision/epoch per authorization record.
     - **Security-Service-Owned Authentication Data** (bearer tokens, session validity, subscriber auth): `owsec` is the source of truth and maintains token revision, expiration, and revocation status.
   - Authoritative revision/epoch validation:
     - The authoritative revision/epoch is maintained in PostgreSQL (for OWPROV data) or `owsec` (for tokens).
     - **Before OWPROV trusts a cached authorization result in Redis**, it verifies that the cached revision/epoch matches the active revision/epoch.
     - **Before OWPROV writes authorization data into Redis on a cache miss**, it validates that the DB-read revision/epoch is still current against the authoritative source of truth.
     - Example (OWPROV Role Change): PostgreSQL has user role = `admin`, `auth_epoch = 5`. Instance `owprov-2` reads this. Concurrently, `owprov-1` changes user role to `viewer`, committing `auth_epoch = 6` to PostgreSQL. When `owprov-2` later attempts to cache or authorize using its read, it checks the active `auth_epoch` in PostgreSQL; seeing current epoch is 6, it identifies epoch 5 as stale, aborts the Redis write, and enforces `viewer` permissions.
     - Example (OWSEC Token Revocation): `owsec` marks token `ABC` valid with `token_epoch = 10`. When the user logs out or the token is revoked, `owsec` updates token state to revoked with `token_epoch = 11`. Any Redis entry with `token_epoch = 10` is identified as stale and rejected; OWPROV validates directly with `owsec` or fails closed.
   - Redis invalidation failure must not be treated as normal bounded staleness for authorization data.
   - For OWPROV-owned authorization data, OWPROV must create a shared durable pending invalidation record and retry with backoff until the affected cache entry is removed. For Security-service-owned token/session data, owsec must own the durable invalidation retry because owsec is the authoritative owner of token revocation and session state.
   - Request authorization must not rely only on a Redis entry known to be stale or affected by a pending invalidation.
   - If the cache state is uncertain or Redis is unavailable, OWPROV must use authoritative validation (PostgreSQL for OWPROV authorization state; Security service REST API for tokens) or fail closed. Stale authorization data is never permitted to be cached or trusted.

### 6.4 Process-local cache usage

In multi-instance mode, OWPROV API paths must not use process-local caches for authoritative API decisions.

API handlers that currently depend on `AuthCache`, `AuthClient`, `SerialNumberCache`, `DeviceTypeCache`, or similar in-memory structures must be changed to use the shared cache-aside model defined in Sections 6.2 and 6.3.

If any of these cache classes remain in the codebase, they may be refactored to wrap Redis shared caching or direct service/database fallbacks, but they must not keep process-local authoritative API decision state.

Required behavior:

```text
1. Authorization and role checks must not use process-local AuthCache as the decision source.
2. Token and API-key authentication must not use process-local AuthClient caches as the authoritative decision source across instances.
3. Serial-number existence, uniqueness, and search behavior must not use process-local SerialNumberCache as the decision source.
4. Device type validation must not use process-local DeviceTypeCache as the decision source.
5. API response decisions must use Redis shared cache or the appropriate authoritative backing source (OWPROV PostgreSQL for entity data; Security service REST API for token validation).
6. Updating one OWPROV instance must not require updating another instance's local cache before the second instance can return the correct API result.
```

---

## 7. Authorization & Authentication Specification

The architecture distinguishes between two separate categories of authorization and authentication state:
1. **OWPROV-Owned Authorization State**: Management roles, policies, entity scopes, and access permissions.
2. **Security-Service-Owned Authentication State (`AuthClient`)**: User session tokens (`Authorization: Bearer <token>`), subscriber tokens.

### 7.1 OWPROV-Owned Authorization (Management Roles, Policies, Scopes)

In multi-instance mode, OWPROV authorization-related API checks (roles, policies, management scopes) must not use process-local `AuthCache` as the decision source.

Authorization checks may use Redis shared cache.

If required authorization data is not present in Redis, the API path must read from PostgreSQL-backed state.

`AuthCache` may remain only if it is refactored to use Redis/PostgreSQL-backed state and does not control authorization from process-local memory.

Required behavior:

```text
1. Authorization checks read from Redis shared cache where available.
2. Redis cache misses read from PostgreSQL-backed state.
3. Permission, role, policy, and management-scope changes are persisted in PostgreSQL-backed state where OWPROV owns the data.
4. After successful authorization-related writes, affected Redis authorization cache keys are invalidated (DEL).
5. Authorization results must not depend on which OWPROV instance receives the request.
```

### 7.2 Security-Service-Owned Authentication (AuthClient: Token & API Key Validation)

OWPROV validates incoming user session tokens (`Authorization: Bearer <token>`), subscriber tokens through `AuthClient` (`src/framework/AuthClient.h`, `src/framework/AuthClient.cpp`).

In single-instance mode, `AuthClient` maintains process-local `ExpireLRUCache` instances.

In multi-instance mode:

```text
1. Shared Redis Token Cache: Validated token and metadata may be cached in Redis using hashed keys. The cached token TTL must never exceed the token's remaining lifetime (and is capped to a safe maximum, e.g., 20 minutes).
2. Cache Miss Target: On a Redis cache miss, AuthClient MUST call the OpenWiFi Security Service (owsec) REST endpoints (/api/v1/validateToken, /api/v1/validateSubToken), NOT OWPROV PostgreSQL. OWPROV PostgreSQL does not store Security-service session tokens or user API keys.
3. Direct owsec Invalidation: Token revocation and logout invalidation are the direct responsibility of owsec. When a token is revoked or a session is terminated, owsec directly deletes the shared Redis token-cache entry (DEL shared Redis token key). Token invalidation does not depend on Kafka offsets, consumer availability, or EVENT_REMOVE_TOKEN broadcast delivery across OWPROV replicas. If owsec cannot delete the Redis token-cache key, owsec must record and retry the invalidation or otherwise prevent the revoked token from validating successfully.
4. Expected Flow:
   Token validation:
   OWPROV -> Redis
             |
             +-- HIT  -> use cached validation
             |
             +-- MISS -> validate with owsec REST API -> cache result in Redis (TTL <= remaining lifetime)

   Token revocation/logout:
   owsec -> DEL shared Redis token key
5. Process-local AuthClient::Cache_ and ApiKeyCache_ must not act as authoritative decision sources across instances.
```

### 7.3 Revocation behavior

Permission, role, policy, and token/API-key changes must be visible to all OWPROV instances immediately.

Required behavior:

```text
1. A permission change handled by owprov-1 must affect a later API request requiring that permission when the request is handled by owprov-2.
2. Revoked privileges must not remain accepted because owprov-2 has old process-local authorization state.
3. Token revocation or logout is executed directly by owsec deleting the shared Redis token key. If owsec cannot delete the Redis token-cache key, owsec must record and retry the invalidation or otherwise prevent the revoked token from validating successfully; subsequent validation requests hitting Redis or falling back to owsec must not authorize from stale state. Cached token TTLs are strictly bounded by remaining token lifetime so stale entries cannot outlive the token validity period.
4. OWPROV-owned role/policy modifications must invalidate affected Redis authorization keys after the committed PostgreSQL write following the security-sensitive invalidation policy (Section 6.3.2).
5. Authorization and token checks must not rely on process-local cache synchronization between OWPROV instances.
6. If authorization cache state is uncertain or pending invalidation, OWPROV must validate authoritatively or fail closed.
```

### 7.4 Implementation direction

For the first implementation:

```text
1. Identify API handlers that currently use AuthCache for permission, role, policy, or management-scope decisions.
2. Replace process-local AuthCache decision behavior with Redis shared cache reads, falling back to PostgreSQL on miss.
3. Refactor AuthClient to check and populate Redis shared cache for token/API-key validation, falling back to the Security service REST API on miss, setting cache TTL <= remaining token lifetime.
4. Document that owsec is responsible for directly deleting the shared Redis token key upon token revocation or logout. Remove reliance on Kafka EVENT_REMOVE_TOKEN for token cache invalidation in OWPROV.
5. Persist OWPROV permission, role, policy, and management-scope changes to PostgreSQL-backed state, and invalidate affected Redis keys after successful PostgreSQL commit.
6. Do not add process-local AuthCache or AuthClient synchronization systems between OWPROV instances.
```

---

## 8. Serial Number And Inventory Specification

### 8.1 SerialNumberCache behavior

In multi-instance mode, OWPROV API paths must not use process-local `SerialNumberCache` as the decision source for serial-number existence, duplicate prevention, or inventory search behavior.

Serial number and inventory cached reads may use Redis shared cache.

Redis cache misses must reload from PostgreSQL.

Implementation rules:

```text
1. Serial-number read/search paths may use Redis shared cache.
2. Redis cache misses must query PostgreSQL.
3. Duplicate serial creation must be prevented by PostgreSQL constraints or transactional checks.
4. Inventory read/search behavior must return results from Redis shared cache or committed PostgreSQL state.
5. API handlers must not update process-local SerialNumberCache as part of making API state valid.
6. After inventory writes, affected Redis serial/inventory keys must be invalidated after PostgreSQL commit.
```

### 8.2 Required database enforcement

The implementation must audit the inventory table/index behavior and ensure serial-number uniqueness rules are enforced at the database layer where the product requires uniqueness.

Redis must not be the only protection for uniqueness.

Required behavior:

```text
1. A serial created through owprov-1 cannot be duplicated through owprov-2.
2. A serial removed through owprov-1 is not treated as valid by owprov-2 because of old process-local memory.
3. Inventory search results are based on Redis shared cache or the same committed PostgreSQL state across replicas.
4. Redis cache misses reload inventory data from PostgreSQL.
5. Redis keys affected by serial/inventory changes are invalidated after PostgreSQL commit.
```

---

## 9. Device Type Specification

### 9.1 DeviceTypeCache behavior

In multi-instance mode, OWPROV API paths must not use process-local `DeviceTypeCache` as the decision source for device type validation.

Device type validation may use Redis shared cache.

Redis cache misses must reload the accepted device type data from PostgreSQL-backed state.

For this phase, the preferred implementation is PostgreSQL-backed device type state with Redis shared cache in front of it.

### 9.2 PostgreSQL-backed device type state with Redis cache

The implementation must add or identify PostgreSQL-backed state that represents the accepted device type set or current accepted device type version.

This state may be populated from the existing firmware/service-registry/download source, but API validation must read through the shared cache-aside model instead of process-local `DeviceTypeCache`.

Required behavior:

```text
1. Every OWPROV instance validates device types against Redis shared cache or the same PostgreSQL-backed accepted device type state.
2. Device type updates are persisted before API validation depends on them.
3. Device type cache misses reload from PostgreSQL-backed state.
4. Device type changes invalidate affected Redis keys after PostgreSQL commit.
5. Device type validation does not require process-local cache synchronization between OWPROV instances.
6. A failed device type refresh is visible in logs or health/debug output.
7. If device type data is downloaded or fetched from another service, the fetched version/checksum must be stored or compared so all instances converge on the same version.
```

---

## 10. Database Startup Specification

### 10.1 Startup ownership

Database startup and schema initialization must be serialized across OWPROV instances.

Kafka group leadership must not be used for this.

The first implementation should use a PostgreSQL advisory lock around the protected database startup section.

### 10.2 Protected section

The protected database startup section includes:

```text
- database object creation
- schema creation/upgrade logic
- Create() calls
- system DB initialization
- consistency checks that modify shared database state
- startup initialization that must not race across processes
```

The advisory lock must not be held for the full OWPROV process lifetime.

### 10.3 Advisory lock behavior

Required behavior:

```text
1. Each OWPROV instance may connect to PostgreSQL.

2. Before running the protected database startup section, the instance must acquire the same fixed advisory lock.

3. Only the lock holder may run the protected section.

4. Other instances wait or fail readiness cleanly while the protected section is unavailable.

5. After the lock holder completes startup initialization, it releases the advisory lock.

6. If the lock holder crashes, PostgreSQL releases the lock when the database session ends.

7. Another instance may then acquire the lock and retry the idempotent startup path.
```

### 10.4 Pseudocode

```text
StorageService::Start():
  connect to PostgreSQL

  acquire advisory lock "owprov-db-startup"

  try:
    run existing database startup/init path
    run schema/object creation
    run consistency/startup initialization
  finally:
    release advisory lock

  continue normal service startup
```

### 10.5 Readiness behavior

An OWPROV instance must not become ready for traffic until:

```text
- database connection is established;
- database startup initialization has completed or been verified safe;
- required schema version is present;
- startup failure state is not active.
```

---

## 11. Database Write Concurrency Specification

### 11.1 Relationship updates

OWPROV must audit relationship-bearing records that use read-modify-write behavior.

Examples include:

```text
- venue device lists
- entity device lists
- map relationships
- configuration relationships
- inventory relationships
```

### 11.2 Required implementation pattern

For each relationship update path, use one of:

```text
- PostgreSQL transaction with SELECT ... FOR UPDATE on the parent row;
- optimistic version column with compare-and-update;
- atomic SQL update;
- normalized relationship table with unique constraints;
- another explicitly reviewed database-level protection.
```

### 11.3 First-phase rule

For first-phase implementation, protect existing parent-record vector updates with PostgreSQL transactions and row-level locking.

Example pattern:

```text
BEGIN;

SELECT parent_row
FROM parent_table
WHERE id = ?
FOR UPDATE;

modify relationship list;

UPDATE parent_table
SET relationship_field = ?, modified = ?
WHERE id = ?;

COMMIT;
```

### 11.4 Conflict handling

The implementation must not convert database conflicts into success.

Required behavior:

```text
1. Retryable conflicts are retried when safe.

2. Non-retryable conflicts return a clear error.

3. Partial multi-record updates are rolled back.

4. Logs include the affected entity type, record id, operation, and instance id.
```

---

## 12. Kafka Specification

### 12.1 Consumer semantics

OWPROV must support two Kafka delivery semantics:

```text
work-queue semantics:
  one OWPROV instance processes each message in the shared service group

broadcast semantics:
  every OWPROV instance receives each message
```

This specification uses two internal consumer roles:

```text
GroupConsumer:
  for work-queue topics

BroadcastConsumer:
  for broadcast/fan-out topics
```

The exact class names may change during implementation, but the delivery behavior must remain.

---

### 12.2 GroupConsumer

GroupConsumer is used when one message should be processed by one OWPROV instance.

Configuration pattern:

```text
group.id = prov
client.id = <instance-id>-work-consumer
```

Initial topic assignment:

```text
connection -> GroupConsumer
```

Required behavior:

```text
1. All OWPROV instances join the same work group.

2. Kafka assigns partitions among the OWPROV instances.

3. A single connection message is processed by one instance in the group.

4. Handler success is required before commit.

5. Handler failure must not be silently acknowledged unless explicitly documented.

6. Rebalance/retry duplicate delivery must be safe through idempotent processing and database constraints.
```

---

### 12.3 BroadcastConsumer

BroadcastConsumer is used when every OWPROV instance must receive each message.

Configuration pattern:

```text
group.id = prov-<INCARNATION_ID>-broadcast
client.id = <INCARNATION_ID>-broadcast-consumer
auto.offset.reset = latest
```

(Where `<INCARNATION_ID>` is the unique runtime incarnation UUID, optionally slot-qualified: `prov-<SLOT_ID>-<INCARNATION_ID>-broadcast`).

Initial topic assignment:

```text
service_events -> BroadcastConsumer
```

Required behavior:

```text
1. Independent Broadcast Consumer Group per Runtime Incarnation:
   Each OWPROV process creates its own broadcast consumer group keyed to its runtime incarnation ID (e.g. group.id = prov-<INCARNATION_ID>-broadcast or prov-<SLOT_ID>-<INCARNATION_ID>-broadcast). Broadcast consumer groups must NOT be statically reused across container restarts.

   Rationale: During rolling restarts or container replacements, the terminating replica (old owprov-1) and starting replica (new owprov-1) run concurrently during the draining/startup grace period. If both shared a static slot-based group.id (e.g. prov-owprov-1-broadcast), Kafka would treat them as members of the same consumer group and divide topic partitions between them. As a result, neither replica would receive all broadcast events, breaking the fan-out invariant. Using an incarnation-unique group.id guarantees that each running replica is the sole member of its consumer group and receives 100% of broadcast messages.

2. Ephemeral Consumer Group Lifecycle:
   Because new instances start with auto.offset.reset = latest and bootstrap discovery state from the shared Redis service-registry snapshot, broadcast consumer groups do not need to persist offsets across process lifetimes. Once an instance process terminates, Kafka's group coordinator automatically purges the inactive incarnation group after the standard offset retention window (offsets.retention.minutes).

3. Initial Cold Start & Bootstrap:
   BroadcastConsumer uses auto.offset.reset = latest. The instance must not replay historical service_events from earliest. It may seed its local Services_ view from the shared Redis service-registry snapshot when available, while live JOIN, KEEP_ALIVE, and LEAVE events remain the primary runtime discovery update path.

4. Fan-Out Delivery:
   Every running OWPROV instance with an active BroadcastConsumer receives live service_events published after it starts consuming.

5. Isolated Processing:
   One instance consuming a service event must not prevent another instance from consuming the same service event.

6. Handler Safety:
   Broadcast handlers must be safe to run independently and idempotently on every instance.
```

---

### 12.4 service_events behavior and instance-aware discovery

`service_events` provides every OWPROV instance and peer microservice with real-time incremental service discovery data.

In an active-active multi-instance deployment behind a load balancer, all OWPROV replicas advertise the shared load-balanced private endpoint (`https://owprov-internal:17005`) while emitting unique per-instance identifiers (`ID` field).

To ensure that an individual replica's shutdown or restart does not unregister the shared service endpoint while other replicas remain healthy, service discovery uses a Kafka-first live discovery model with an optional shared Redis bootstrap snapshot. Each instance keeps a local in-memory Services_ runtime view updated from live service_events. Redis stores a current-state snapshot that can seed Services_ for newly started instances when available:

Payload structure:

```text
EVENT:   JOIN, KEEP_ALIVE, LEAVE
ID:      <runtime-incarnation-id> (runtime UUID, optionally prefixed: <slot-id>-<incarnation-uuid>)
TYPE:    owprov (logical service type)
PRIVATE: https://owprov-internal:17005 (shared load-balanced private endpoint)
PUBLIC:  https://owprov.example.com:16005 (shared load-balanced public endpoint)
KEY:     <shared-service-key>
VRSN:    <daemon-version>
```

Required handler and registry behavior:

```text
1. Shared Redis Service Registry (Current State Snapshot):
   - Redis stores the current active service discovery state under keys formatted as:
     service-registry:{service_type}:{instance_id}
   - instance_id must use the unique runtime incarnation ID (or <slot-id>-<incarnation-uuid>). This ensures that when an old replica terminates during an overlapping rolling restart and issues LEAVE, it deletes only its own key and does not overwrite or remove the replacement replica's registration.
   - Each service instance self-registers its own current state; no single instance or leader owns the registry.
   - The shared Redis service registry is populated by service-discovery producers using the common service-registry contract when Redis registry support is available. Redis registry write failure must not prevent the producer from publishing live service_events.
   - Redis stores a current-state bootstrap snapshot only; it does not store raw service_events history and is not used as the normal request-path lookup source.
   - Self-registration records are written with a configurable TTL greater than the maximum keep-alive interval. Current services publish keep-alive every 5-10 seconds, so a TTL such as 30-60 seconds provides a bounded stale-service window while tolerating brief delays.
   - On clean shutdown (LEAVE), the exiting instance deletes its own Redis key.
   - If an instance terminates abnormally without sending LEAVE, its registration expires automatically via TTL.

2. Multi-Replica In-Memory Registry (Fast Runtime Lookups):
   - Each OWPROV instance maintains a local in-memory registry (Services_[Type][InstanceID] -> MicroServiceMeta) for fast request-path endpoint resolution.
   - Inter-service client resolution (e.g. GetServices(Type)) returns the active endpoint as long as at least one healthy replica is present in the registry.
   - Peer microservices must similarly track replicas by instance ID so that an individual replica's departure does not unregister the shared endpoint in peer services.
   - This is a common OpenWiFi service-discovery contract requirement: peer services or shared framework code that still key service records only by PrivateEndPoint must be upgraded to instance-keyed replica tracking.

3. JOIN & KEEP_ALIVE:
   - The announcing instance publishes JOIN and KEEP_ALIVE over Kafka service_events.
   - When Redis registry support is available, the announcing instance also refreshes its own Redis record (service-registry:{service_type}:{instance_id}) with updated metadata and reset TTL.
   - Peer instances receiving the Kafka event update the liveness timestamp and metadata in their local in-memory Services_ view.
   - Redis write failure must not prevent live service_events from being published.

4. LEAVE:
   - The departing instance publishes LEAVE over Kafka service_events.
   - When Redis registry support is available, the departing instance also removes its Redis key (service-registry:{service_type}:{instance_id}).
   - Peer instances receiving the Kafka LEAVE event remove that InstanceID from their local in-memory Services_ view.
   - If the instance terminates abnormally and cannot publish LEAVE, local stale-entry timeout and Redis TTL expiry must eventually remove the stale instance from discovery views.
   - The logical service endpoint remains active and routable as long as other replicas of that service type remain registered.
   - The logical service entry is removed from routing only when its last active replica leaves or times out.

5. Token Cache Invalidation Decoupled from service_events:
   - Service discovery events are strictly dedicated to microservice liveness and routing (JOIN, KEEP_ALIVE, LEAVE). Token revocation is handled directly by owsec deleting Redis keys, without routing invalidation events through Kafka service_events.

6. Startup & Bootstrap Lifecycle:
   - On startup, an OWPROV instance starts its BroadcastConsumer for service_events with its incarnation-unique group.id and auto.offset.reset = latest.
   - The instance builds and maintains its local Services_ runtime view from live JOIN, KEEP_ALIVE, and LEAVE events.
   - If configured, the instance attempts to seed its local Services_ view from the Redis service-registry snapshot. Redis service-registry snapshot unavailability must not fail service-discovery bootstrap by itself; however, the instance must not pass readiness until required upstream services are present in its local Services_ view, whether learned from Redis snapshot seeding or live JOIN/KEEP_ALIVE events.
   - Kafka history replay from earliest must not be used as the service-discovery bootstrap mechanism.
   - Runtime inter-service lookup uses the local Services_ view.
   - Local Services_ entries must expire or be removed when KEEP_ALIVE stops, LEAVE is received, or the entry is otherwise determined stale.
   - An instance is considered discovery-ready only after required upstream dependency microservices are present in its local Services_ view, whether learned from Redis snapshot seeding or live service_events.
```

Local `Services_` remains the primary runtime discovery view for OWPROV inter-service lookups.

Correctness must not depend on replaying historical Kafka service_events from earliest.

If BroadcastConsumer live delivery is unavailable, OWPROV may lose live discovery updates; Redis snapshot seeding can help new instances bootstrap, but it is not a replacement for live service_events during normal runtime.

Token revocation and session termination do not depend on Kafka `service_events` broadcast. `owsec` directly deletes the shared Redis token key upon revocation. Cached token entries must always have a TTL bounded by the token's remaining lifetime. On a Redis cache miss, OWPROV validates with `owsec` via REST, ensuring immediate revocation visibility without Kafka consumer lag or cross-instance broadcast dependencies.

---

### 12.5 connection behavior

`connection` must remain work-queue style.

Required behavior:

```text
1. Each connection message is processed by one OWPROV instance in the shared work group.

2. The processing instance writes required durable state to PostgreSQL.

3. Later API reads for that device can be served by any OWPROV instance through the shared cache-aside model.

4. The device does not become permanently owned by the processing instance.

5. Duplicate delivery after retry or rebalance must not create duplicate inventory or conflicting device state.
```

### 12.5.1 Idempotency

Connection processing must be idempotent by device identity.

The handler must use stable identifiers such as:

```text
- serial number
- device UUID
- MAC address, if applicable
```

Database writes must use one of:

```text
- upsert;
- unique constraint plus safe retry;
- transaction with row lock;
- explicit duplicate detection.
```

---

## 12.6 Kafka commit behavior

For state-changing handlers:

```text
1. Process message.
2. Persist required database changes.
3. Complete required side effects or record pending side effects durably.
4. Commit Kafka offset only after successful handling.
```

If a handler fails:

```text
- log the failure with topic, partition, offset, key, instance id, and error;
- do not commit unless the failure is explicitly non-retryable and recorded;
- avoid infinite retry loops without visibility.
```

A later `testcases.md` must include rebalance and retry scenarios.

---

## 12.7 Kafka producer partitioning

OWPROV-produced Kafka messages must not force scalable work topics to partition `0` unless explicitly justified.

Required behavior:

```text
1. Work topics that require ordering per device use a stable message key.

2. Messages for the same device should map to the same partition where ordering matters.

3. Messages for different devices should be able to distribute across partitions.

4. Producer code must allow key-based partitioning for scalable work topics.
```

---

## 13. Service Identity Specification

### 13.1 Logical service identity

The logical OWPROV service identity is shared across all instances.

Shared values:

```text
- service type
- public service URI
- internal API behavior tied to shared public URI
```

### 13.2 Instance identity: slot identity vs runtime incarnation identity

Each OWPROV instance must have its own identity clearly distinguished between logical slot naming and runtime incarnation:

1. **Logical slot identifier** (e.g., illustrative `owprov-1`, `owprov-2`):
   Optional administrative or deployment slot naming. Must not be hardcoded or assumed to be static across rolling restarts.

2. **Runtime incarnation identifier** (UUID):
   Generated at process startup, guaranteeing collision-free execution across overlapping replicas.

Instance-scoped values:

```text
- runtime incarnation id (UUID, optionally slot-prefixed)
- service_events ID (uses runtime incarnation id)
- Kafka client.id (incorporates runtime incarnation id)
- broadcast consumer group id (incorporates runtime incarnation id: prov-<incarnation-id>-broadcast)
- logs and metrics labels
- job lease owner id (uses runtime incarnation id)
- service event instance metadata
```

Implementation rules:

```text
1. The service_events ID field must uniquely identify the individual OWPROV runtime incarnation.
2. ID must use or incorporate the runtime incarnation ID (UUID). It must not represent only the shared logical OWPROV service identity or a bare slot name that collides during overlapping restarts.
3. Two simultaneously running OWPROV replicas (including old and replacement replicas during a rolling restart) must never emit the same service_events instance ID or share a broadcast consumer group.
```

### 13.3 Public endpoint

The public endpoint must point to the load balancer, not to one instance.

Example:

```text
https://owprov.example.com:16005
```

### 13.4 Private endpoint

For multi-instance deployments behind a load balancer, all OWPROV instances advertise a shared load-balanced private endpoint.

Example:

```text
https://owprov-internal:17005
```

Implementation rules:

```text
1. All OWPROV replicas publish the shared load-balanced private endpoint so peer microservices route inter-service API traffic through the load balancer.
2. Individual replicas are distinguished in service_events by their unique instance ID.
3. Peer microservice registries track replicas per instance ID (Section 12.4) so replica scale-in or restarts do not prematurely unregister the shared endpoint in peer services.
4. Do not advertise localhost or unroutable container IP addresses as the service endpoint in multi-instance mode.
```

### 13.5 Internal API key/hash behavior

If the internal API key/hash is derived from the shared public endpoint, all OWPROV instances must compute the same logical service key.

This is acceptable only when the key represents logical OWPROV service identity.

It must be documented that:

```text
- the key is service-scoped;
- the key is not a unique instance identity;
- instance identity is provided separately.
```

---

## 14. Runtime Downloaded File Specification

### 14.1 Local data directory

Each OWPROV container may have its own local data directory.

A shared data directory is not required for the Docker Compose phase.

### 14.2 Download source

All OWPROV instances must use the same configured download source for required runtime files.

Required behavior:

```text
1. Each instance downloads required files independently.

2. Files are not manually changed differently on different instances.

3. A newly started instance can become ready without copying files from another instance.

4. Requests depending on downloaded files behave the same on every instance.
```

### 14.3 Readiness behavior

If a required file cannot be downloaded or validated, the instance must not become ready for dependent API behavior.

Required logs:

```text
- file name
- configured source
- expected version/checksum, if available
- actual validation result
- instance id
```

---

## 15. Background Job Specification

### 15.1 Durable job model

Long-running jobs must be represented in PostgreSQL.

Examples:

```text
- venue configuration update (VenueConfigUpdater)
- firmware upgrade (VenueUpgrade)
- device reboot (VenueRebooter)
```

### 15.2 Job table

The implementation adds a durable job table in PostgreSQL.

Minimum fields:

```text
id                  -- UUID primary key
job_type            -- "VenueRebooter", "VenueUpgrade", "VenueConfigUpdater"
owner_user_id       -- Stable Security service user identifier for authorization
owner_email         -- User email for WebSocket delivery/display/audit
parameters          -- JSON array/object of parameters (e.g. venueId, revision)
status              -- pending, running, succeeded, failed
owner_instance_id   -- ID of OWPROV instance currently executing the job
lease_generation    -- Monotonically increasing fencing token (BIGINT) incremented on claim/reclaim
lease_expires_at    -- Heartbeat lease expiration timestamp to detect worker crashes
attempt_count       -- Number of execution attempts
max_attempts        -- Maximum allowed execution attempts (e.g. 2)
progress            -- JSON object tracking per-device execution progress and completion state
result              -- Summary JSON result details (minimized; excludes configuration bodies and secrets)
error_message       -- Error text if the job failed
created_at          -- Timestamp when job was submitted
started_at          -- Timestamp when execution began
completed_at        -- Timestamp when terminal state was reached
```

Allowed statuses:

```text
pending
running
succeeded
failed
```

### 15.3 Job creation and atomic claiming

When a REST request starts a long-running action:

```text
1. Validate the request and extract caller identity (UserInfo.id, UserInfo.email).
2. Insert a row into the jobs table with status 'pending', attempt_count = 0, lease_generation = 0, owner_user_id, and owner_email.
3. Return the jobId and initial 'pending' status to the caller immediately via HTTP.
4. A worker loop on an OWPROV instance atomically claims the pending job from PostgreSQL:

   UPDATE jobs
   SET
     status = 'running',
     owner_instance_id = :instance_id,
     lease_generation = lease_generation + 1,
     lease_expires_at = :now + :lease_interval,
     attempt_count = attempt_count + 1,
     started_at = COALESCE(started_at, :now)
   WHERE id = :job_id
     AND status = 'pending'
   RETURNING lease_generation, *;

5. Only the instance that successfully receives the updated row executes the background job thread, retaining the returned lease_generation in memory as its active fencing token.
```

### 15.4 Lease renewal, progress updates, and fenced heartbeats

While executing a job, the owning instance must maintain an active lease:

```text
1. Running jobs have an active lease_expires_at timestamp and lease_generation fencing token.
2. The executing worker periodically updates lease_expires_at and persists progress using fenced queries:

   UPDATE jobs
   SET lease_expires_at = :now + :lease_interval
   WHERE id = :job_id
     AND owner_instance_id = :instance_id
     AND lease_generation = :my_lease_generation;

3. If any heartbeat, progress update, or terminal update returns 0 rows modified (indicating the lease expired and was reclaimed by another worker with an incremented lease_generation), the current worker detects fence invalidation, immediately aborts execution, and halts further device operations.
4. Upon task completion, the worker atomically updates status to 'succeeded' or 'failed', persists result/error_message, sets completed_at using the same fenced predicate (WHERE id = :job_id AND owner_instance_id = :instance_id AND lease_generation = :my_lease_generation), and triggers cross-instance notification delivery (Section 16).
```

### 15.5 Expired-owner recovery and retry rules

Surviving OWPROV instances periodically scan for crashed or abandoned jobs:

```text
1. Scan for jobs where status = 'running' AND lease_expires_at < :now.
2. If attempt_count < max_attempts:
   - Another instance atomically reclaims the job and increments the fencing token:

     UPDATE jobs
     SET
       owner_instance_id = :new_instance_id,
       lease_generation = lease_generation + 1,
       lease_expires_at = :now + :lease_interval,
       attempt_count = attempt_count + 1
     WHERE id = :job_id
       AND status = 'running'
       AND lease_expires_at < :now
       AND attempt_count < max_attempts
     RETURNING lease_generation, *;

   - The reclaim event is logged with old owner, new owner, attempt count, and new lease_generation.
   - The new owner resumes execution using the incremented lease_generation fencing token.
3. If attempt_count >= max_attempts:
   - The job is transitioned to terminal status 'failed'.
   - error_message is set to 'Worker crashed and maximum execution attempts exceeded'.
   - completed_at is recorded.
```

### 15.6 Device-side idempotency and operation keys

```text
1. Fencing tokens prevent stale workers from mutating database job state, but external device operations (reboot, firmware upgrade, configuration push via SDK/GW) require operation-level idempotency.
2. Device commands dispatched to Gateway (owgw) must include an operation idempotency key / operation_id (e.g. "${job_id}-${device_serial_number}-${action}").
3. Each per-device operation must be tracked with an explicit state machine:
   pending -> dispatching -> dispatched/acknowledged -> completed or failed.
4. Execution sequence:
   - Before dispatch, the worker records the operation_id and marks the device operation as dispatching in the job progress tracking.
   - After owgw accepts or acknowledges the command, the operation moves to dispatched/acknowledged.
   - It is marked completed only after the defined completion condition for that action is satisfied (or failed if owgw rejects it).
   - If the worker crashes before acknowledgment, the recovering worker retries the same operation_id.
5. owgw and downstream device-facing services must durably de-duplicate commands by operation_id across retries and restarts, preventing repeated or conflicting commands on physical devices.
6. On crash recovery/reclaim, the newly claiming instance inspects the progress column and skips devices that have already reached completed status.
```

### 15.7 Job query endpoint and access control

Any OWPROV instance can serve job status queries from PostgreSQL:

```text
GET /api/v1/jobs/{id}
```

Access control and data minimization rules:

```text
1. The endpoint requires authentication.
2. Reads are allowed when the authenticated caller's Security user ID (UserInfo.id) matches job.owner_user_id.
3. Reads are also allowed when the caller has an explicit admin or support permission/role covering the job's resource scope (e.g. venue/entity).
4. Requests from callers who are neither the job owner nor authorized for the job's scope are rejected with 403 Forbidden.
5. Job result payloads must contain only fields necessary to report job status and outcome. They must exclude secrets and configuration bodies, and access to the result must follow the same owner/scope authorization rules as the job row.
```

---

## 16. WebSocket And Notification Specification

### 16.1 Problem model

A browser may have a WebSocket connected to `owprov-1`, while the action or job that generates a notification runs on `owprov-2`.

Therefore, local WebSocket maps are not enough for multi-instance notification delivery.

Progress and completion may be delivered via either real-time cross-instance notification push (Option A) or durable queryable job status polling (Option B).

### 16.2 Delivery options

#### Option A: Kafka-based notification bus (Real-time push)

The primary push-based implementation uses Kafka as the cross-instance notification bus.

Recommended topic:

```text
owprov.ui_notifications
```

Delivery semantics:

```text
broadcast/fan-out
```

Each OWPROV instance must consume notification events through BroadcastConsumer so it can deliver relevant notifications to its own local WebSocket clients.

#### Option B: Non-Kafka durable job status polling (REST pull alternative)

If Kafka is not used for cross-instance UI notification delivery:

```text
1. Background workers record job progress and completion exclusively in the shared PostgreSQL jobs table.
2. The UI client automatically polls the job status endpoint (e.g., GET /api/v1/jobs/{id}) in the background every 2–3 seconds until a terminal state is reached.
3. Any OWPROV instance can serve status queries directly from PostgreSQL.
4. No Kafka notification topic or cross-instance WebSocket fan-out is required for this approach.
```

### 16.3 Notification event format (Option A)

The Kafka notification message defines an explicit envelope containing owner identity, job reference, and payload:

```text
owner_user_id:      Security service user identifier (SecurityObjects::UserInfo.id) for authorization and audit
owner_email:        User email (SecurityObjects::UserInfo.email) for local socket lookup
job_id:             UUID of the associated job (or empty if not a job-related event)
notification_type:  Notification type ID (e.g. 1000 = fw_upgrade, 2000 = config_update, 3000 = rebooter)
source_instance_id: OWPROV instance identifier that generated the event
created_at:         Timestamp
payload:            JSON object containing { notification_id, type_id, content }
```

Example Kafka JSON message:

```json
{
  "owner_user_id": "99351e3e-4b2a-4f51-b0e6-a21234567890",
  "owner_email": "user@example.com",
  "job_id": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "notification_type": 3000,
  "source_instance_id": "owprov-2",
  "created_at": 1726400000,
  "payload": {
    "notification_id": 105,
    "type_id": 3000,
    "content": {
      "title": "Venue Reboot",
      "jobId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
      "details": "Job Completed: 50 rebooted, 0 failed.",
      "timeStamp": 1726400000
    }
  }
}
```

When an OWPROV instance consumes this message from the broadcast topic:
- It uses `owner_email` to find candidate connected WebSocket sessions on that local instance.
- It verifies that the candidate socket's authenticated user ID matches `owner_user_id` before delivering the frame.
- If no matching authenticated socket exists on that instance, the event is safely ignored and discarded by that replica.
- Broadcast fan-out consumers must never deliver user- or job-specific notifications to arbitrary or unauthenticated clients.

### 16.4 Delivery behavior (Option A)

Required behavior:

```text
1. The instance executing or finishing the action publishes a notification event to the broadcast topic with the complete ownership envelope (owner_user_id, owner_email, job_id, notification_type).

2. Every OWPROV instance receives the notification event via its BroadcastConsumer.

3. Each instance filters the event against its local WebSocket client map:
   - Uses owner_email to locate local socket sessions.
   - Verifies the socket's authenticated user ID matches owner_user_id.

4. Only the instance holding the matching authenticated socket delivers the event to the user's browser.

5. Other instances with no matching authenticated socket safely discard the message.

6. If the user has no active WebSocket connected to any instance, durable state remains queryable through GET /api/v1/jobs/{id}.
```

Sticky WebSocket routing may be used for connection stability, but it is not the notification delivery mechanism.

### 16.5 Solution comparison

| Attribute | Option A: Kafka Notification Bus (Push) | Option B: PostgreSQL Job Polling (Pull) |
| :--- | :--- | :--- |
| **Delivery Model** | Real-time WebSocket push | Automated background HTTP polling |
| **Kafka Changes** | Requires `owprov.ui_notifications` topic & consumer | Zero Kafka changes |
| **Frontend UI Impact** | Existing WebSocket listener unchanged | Requires polling loop in frontend UI |
| **Offline Client Resilience** | Requires durable status backing for replay | Naturally resilient via database state |

---

## 17. Rate Limiting Specification

### 17.1 Scope

Rate limiting is conditional.

This implementation does not require rate limiting for every OWPROV API.

### 17.2 Local limits

Process-local rate limiting may remain only for local overload protection.

It must be documented as:

```text
local per-instance protection
```

It must not be described as a global user/API limit.

### 17.3 Global limits

If an API requires global user/API rate limiting in Docker Compose phase, the first implementation should enforce it at the load balancer.

Required behavior:

```text
1. The effective limit does not multiply when OWPROV instance count increases.

2. The limit is documented outside OWPROV process-local memory.

3. Tests route traffic across multiple instances and verify the same effective limit.
```

---

## 18. Docker Compose Deployment Specification

### 18.1 Compose services

The Docker Compose deployment must include:

```text
load balancer
owprov-1
owprov-2
owprov-3
PostgreSQL
Redis
Kafka
```

The exact Compose YAML belongs in implementation, but the behavior must follow this specification.

### 18.2 Shared dependencies

All OWPROV instances must point to the same:

```text
- PostgreSQL database
- Redis shared cache
- Kafka cluster
- public service endpoint
- internal service endpoint, if internal load balancing is used
- runtime file download source
```

### 18.3 Unique per-instance values

Each OWPROV instance must have unique values for:

```text
- runtime incarnation ID (UUID) and logical slot ID (if configured)
- Kafka client.id values (incorporating runtime incarnation ID)
- broadcast consumer group id (incorporating runtime incarnation ID)
- logs/metrics identity
- job owner identity
```

### 18.4 Health and readiness

The load balancer must route traffic only to ready OWPROV instances.

An instance is ready only when:

```text
- database startup coordination has completed;
- PostgreSQL is reachable;
- Kafka required consumers/producers are ready;
- required upstream dependency microservices (such as security, gateway, and firmware services) have been discovered in the service discovery registry;
- required runtime files are downloaded and validated;
- required service identity configuration is valid;
- the instance is not draining.

(Note: Redis connectivity is checked for shared caching; if Redis is unavailable, the instance serves traffic authoritatively via PostgreSQL and owsec in non-cached mode).
```

### 18.5 Drain behavior

When an OWPROV instance receives shutdown:

```text
1. Mark instance as draining.

2. Readiness check starts failing.

3. Load balancer stops routing new requests to the instance.

4. Stop accepting new long-running jobs.

5. Stop or pause Kafka consumption safely.

6. Complete, release, or durably record in-flight work.

7. Close WebSocket connections gracefully where possible.

8. Exit only after shutdown handling completes or the configured grace period expires.
```

### 18.6 Scale out/in

Scale out:

```text
1. Start additional OWPROV instance.
2. Instance receives unique identity.
3. Instance starts Kafka consumers, including BroadcastConsumer for service_events.
4. Instance builds its local Services_ discovery view from live service_events.
5. If Redis service registry is available, instance may seed its local Services_ view from the Redis snapshot.
6. Instance completes PostgreSQL, Kafka, runtime file, and readiness checks.
7. When ready/routable, instance broadcasts JOIN over service_events and refreshes its Redis service-registry record if Redis is available.
8. Load balancer starts routing traffic after readiness succeeds.
```

Scale in:

```text
1. Mark selected instance draining.
2. Load balancer stops routing new requests.
3. Kafka work is stopped/rebalanced safely.
4. Job leases are completed or released.
5. WebSocket clients reconnect or receive graceful close.
6. Instance exits without losing durable state.
```

Database connection limits must be reviewed so adding instances does not exhaust PostgreSQL connections.

---

## 19. Initial Code Areas To Audit

The implementation should review and update these areas.

### 19.1 API/cache/auth/Redis

```text
src/framework/RESTAPI_Handler.h
src/framework/RESTAPI_Handler.cpp
src/framework/AuthClient.h
src/framework/AuthClient.cpp
src/RESTAPI/RESTAPI_managementRole_handler.cpp
src/RESTAPI/RESTAPI_managementPolicy_handler.cpp
src/RESTAPI/RESTAPI_inventory_handler.cpp
src/SerialNumberCache.h
src/SerialNumberCache.cpp
src/DeviceTypeCache.h
```

### 19.2 Database

```text
src/StorageService.cpp
src/storage/storage_inventory.cpp
src/framework/orm.h
src/RESTAPI/RESTAPI_db_helpers.h
src/RESTAPI/RESTAPI_map_handler.cpp
```

### 19.3 Kafka

```text
src/framework/KafkaManager.cpp
src/framework/KafkaManager.h
src/framework/EventBusManager.cpp
src/AutoDiscovery.cpp
src/framework/MicroService.cpp
```

### 19.4 Jobs

```text
src/JobController.h
src/JobController.cpp
src/RESTAPI/RESTAPI_venue_handler.cpp
src/Tasks/VenueRebooter.h
src/Tasks/VenueUpgrade.h
src/Tasks/VenueConfigUpdater.h
```

### 19.5 WebSocket/notifications

```text
src/framework/UI_WebSocketClientServer.h
src/framework/UI_WebSocketClientServer.cpp
src/framework/WebSocketLogger.h
src/UI_Prov_WebSocketNotifications.cpp
```

### 19.6 Runtime files

```text
src/Daemon.cpp
src/FileDownloader.cpp
src/RESTAPI/RESTAPI_asset_server.cpp
src/DeviceTypeCache.h
```

### 19.7 Deployment/config

```text
owprov.properties.tmpl
docker-compose files
runtime env files
```

---

## 20. Implementation Phases

### Phase 1 — Architecture and cross-service contracts

```text
- requirements.md / spec.md review
- identify cross-service dependencies with owsec, owgw and shared framework code
```

### Phase 2 — Multi-instance foundation and DB safety

```text
- runtime incarnation ID and optional slot ID
- shared logical service identity / shared API key / shared endpoints
- PostgreSQL startup advisory lock
- explicit DB transaction support
- row-level locking (SELECT ... FOR UPDATE)
- rollback handling
- unique constraints
- retryable conflict handling
- basic 2-instance development Compose topology
```

### Phase 3 — Kafka and service discovery

```text
- GroupConsumer / BroadcastConsumer separation
- incarnation-specific Kafka client/group identities
- Services_[Type][InstanceId] local registry
- Redis service-registry snapshot
- per-instance Redis self-registration
- JOIN / KEEP_ALIVE / LEAVE handling
- local last_seen tracking and independent stale-entry cleanup
- Redis TTL cleanup
- service-discovery bootstrap and eventual-convergence behavior
- Kafka commit/retry/idempotency rules
- key-based producer partitioning
```

### Phase 4 — Shared application state and caching

```text
- Redis cache-aside integration for API data
- post-commit cache invalidation
- SerialNumberCache migration
- DeviceTypeCache migration
- AuthCache migration
- AuthClient / owsec token-cache behavior
- authorization vs normal-data cache consistency policies
```

### Phase 5 — Durable async jobs and notifications

```text
- PostgreSQL jobs table
- atomic claim / reclaim
- lease_generation fencing
- device operation_id and progress states
- explicit owgw deduplication dependency
- job status API
- cross-instance notification delivery
```

### Phase 6 — Production deployment and validation

```text
- runtime file consistency
- load balancer/readiness behavior
- graceful drain
- scale-out / scale-in
- failure-mode testing for Redis, Kafka and PostgreSQL
```
