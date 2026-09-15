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
- no API dependency on process-local AuthCache, SerialNumberCache, DeviceTypeCache, or similar in-memory caches
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
- instance id
- Kafka client id
- log/metric identity
- broadcast consumer group id, where broadcast Kafka behavior is required
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

### 5.1 Instance identity

Each OWPROV container must receive a unique instance id.

Example:

```text
OWPROV_INSTANCE_ID=owprov-1
OWPROV_INSTANCE_ID=owprov-2
OWPROV_INSTANCE_ID=owprov-3
```

This instance id must be used for:

```text
- log fields
- metrics labels
- Kafka client.id values
- service event metadata
- job owner identity
- readiness/shutdown messages
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

Kafka `client.id` must be unique per instance and per consumer role.

Example:

```text
owprov-1-work-consumer
owprov-1-broadcast-consumer
owprov-1-producer

owprov-2-work-consumer
owprov-2-broadcast-consumer
owprov-2-producer
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
5. Store the PostgreSQL result in Redis where caching is allowed.
6. Return the API result.
```

Implementation rules:

```text
1. Redis keys must be deterministic and shared across OWPROV instances.
2. Cache values must represent PostgreSQL-backed data or data derived from PostgreSQL-backed state.
3. Cache misses must reload from PostgreSQL.
4. Cache TTLs must be short and configurable per data type to ensure bounded staleness.
5. API behavior must not fall back to process-local cache state when Redis misses.
```

### 6.3 Write behavior and cache invalidation

Write paths must update PostgreSQL first and invalidate Redis after successful commit.

Write path:

```text
1. Validate request.
2. Start required PostgreSQL transaction.
3. Write PostgreSQL changes.
4. Commit PostgreSQL transaction.
5. Invalidate affected Redis cache keys.
6. Return API response.
```

Implementation rules:

```text
1. Redis must not be updated before PostgreSQL commit.
2. Failed PostgreSQL writes must not invalidate or overwrite Redis cache entries.
3. POST/PUT/DELETE handlers must identify affected Redis keys.
4. The preferred first implementation is cache invalidation, not direct Redis mutation.
5. The next read repopulates Redis from PostgreSQL on cache miss.
```

### 6.3.1 Invalidation failure handling and bounded staleness

When a write operation succeeds in PostgreSQL, OWPROV invalidates the corresponding Redis cache key(s). If the Redis invalidation call fails (e.g., due to temporary network partition, socket timeout, or Redis command error), OWPROV must handle the failure safely:

```text
PostgreSQL Commit Succeeded -> Redis Invalidation Failed:
  1. Return HTTP Success (200/201/204) to the client.
  2. Log ERROR with affected cache keys and failure reason.
  3. Increment invalidation failure metric.
  4. Stale cache entry expires quickly via short configurable TTL fallback.
  5. Subsequent read reloads fresh data from PostgreSQL.
```

Implementation rules:

```text
1. Successful DB writes must not return API errors if Redis invalidation fails. PostgreSQL has already durably committed the change; returning an HTTP error would mislead callers and risk dangerous duplicate non-idempotent retries.
2. All Redis cache entries must be written with a short, configurable TTL (e.g., a short safety window such as 10–60 seconds, configurable via openwifi.redis.cache.ttl or per domain) rather than long or indefinite durations.
3. The short TTL acts as a bounded staleness fallback: in the event of an individual invalidation failure, stale entries expire quickly on their own without requiring complex background retry queues or outbox processing.
4. Redis is a hard dependency for startup and readiness: if Redis is unreachable or offline, the instance must fail startup or fail readiness and refuse traffic until Redis connectivity is established. OWPROV must never fall back to process-local cache state.
5. Invalidation failures must emit ERROR logs with affected keys and increment owprov_redis_invalidation_failures_total for operator visibility.
```

### 6.4 Process-local cache usage

In multi-instance mode, OWPROV API paths must not use process-local caches for API decisions.

API handlers that currently depend on `AuthCache`, `SerialNumberCache`, `DeviceTypeCache`, or similar in-memory structures must be changed to use the shared cache-aside model defined in Sections 6.2 and 6.3.

If any of these cache classes remain in the codebase, they may be refactored to wrap Redis/PostgreSQL-backed behavior, but they must not keep process-local authoritative API decision state.

Required behavior:

```text
1. Authorization checks must not use process-local AuthCache as the decision source.
2. Serial-number existence, uniqueness, and search behavior must not use process-local SerialNumberCache as the decision source.
3. Device type validation must not use process-local DeviceTypeCache as the decision source.
4. API response decisions must use Redis shared cache or PostgreSQL-backed state.
5. Updating one OWPROV instance must not require updating another instance's local cache before the second instance can return the correct API result.
```

---

## 7. Authorization Specification

### 7.1 AuthCache behavior

In multi-instance mode, OWPROV authorization-related API checks must not use process-local `AuthCache` as the decision source.

Authorization checks may use Redis shared cache.

If required authorization data is not present in Redis, the API path must read from PostgreSQL-backed state.

`AuthCache` may remain only if it is refactored to use Redis/PostgreSQL-backed state and does not control authorization from process-local memory.

Required behavior:

```text
1. Authorization checks read from Redis shared cache where available.
2. Redis cache misses read from PostgreSQL-backed state.
3. Permission, role, policy, token, and management-scope changes are persisted in PostgreSQL-backed state where OWPROV owns the data.
4. After successful authorization-related writes, affected Redis authorization cache keys are invalidated.
5. Authorization results must not depend on which OWPROV instance receives the request.
```

### 7.2 Revocation behavior

Permission, role, policy, token, or management-scope changes must be visible to all OWPROV instances.

Required behavior:

```text
1. A permission change handled by owprov-1 must affect a later API request requiring that permission when the request is handled by owprov-2.
2. Revoked privileges must not remain accepted because owprov-2 has old process-local authorization state.
3. Token removal or revocation must update PostgreSQL-backed state where OWPROV owns the affected token state.
4. Affected Redis authorization cache keys must be invalidated after the committed change.
5. Authorization checks must not rely on process-local cache synchronization between OWPROV instances.
```

### 7.3 Implementation direction

For the first implementation, use Redis shared cache for cached authorization reads and PostgreSQL reload on cache miss.

Implementation rules:

```text
1. Identify API handlers that currently use AuthCache for permission, role, policy, token, or management-scope decisions.
2. Replace process-local AuthCache decision behavior with Redis shared cache reads.
3. On Redis miss, reload authorization data from PostgreSQL-backed state.
4. Persist permission, role, policy, token, and management-scope changes to PostgreSQL-backed state where OWPROV owns the data.
5. Invalidate affected Redis authorization keys after successful PostgreSQL commit.
6. Do not add a process-local AuthCache synchronization system between OWPROV instances.
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
group.id = prov-<instance-id>-broadcast
client.id = <instance-id>-broadcast-consumer
```

Initial topic assignment:

```text
service_events -> BroadcastConsumer
```

Required behavior:

```text
1. Each OWPROV instance has its own broadcast consumer group.

2. Every OWPROV instance receives every service_events message.

3. One instance consuming a service event must not prevent another instance from consuming the same service event.

4. Broadcast handlers must be safe to run independently on every instance.
```

---

### 12.4 service_events behavior

`service_events` must provide every OWPROV instance with service discovery data.

Required handler behavior:

```text
1. JOIN:
   update service discovery view for the announced service instance.

2. KEEP_ALIVE:
   refresh liveness timestamp for the announced service instance.

3. LEAVE:
   remove or mark stale only the announcing service instance.

4. EVENT_REMOVE_TOKEN:
   update PostgreSQL-backed token/auth state where OWPROV owns the affected token state, then invalidate affected Redis authorization cache keys after the committed change.
```

Local `Services_` state may remain process-local only if every OWPROV instance receives every required `service_events` message through BroadcastConsumer.

If reliable broadcast delivery is not implemented, service discovery must move to a shared registry source.

`EVENT_REMOVE_TOKEN` must not rely on local `AuthCache` invalidation as the authorization protection mechanism in multi-instance mode. Authorization cache invalidation must target Redis shared cache keys where OWPROV owns the affected authorization state.

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

### 13.2 Instance identity

Each OWPROV instance must have its own identity.

Instance-scoped values:

```text
- instance id
- Kafka client.id
- broadcast consumer group id
- logs and metrics labels
- job owner id
- service event instance metadata
```

### 13.3 Public endpoint

The public endpoint must point to the load balancer, not to one instance.

Example:

```text
https://owprov.example.com:16005
```

### 13.4 Private endpoint

For Docker Compose phase, the private endpoint should also point to an internal load-balanced OWPROV endpoint.

Example:

```text
https://owprov-internal:17005
```

Do not advertise `localhost` as the service endpoint in multi-instance mode.

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
user_id             -- User email who initiated the job
parameters          -- JSON array/object of parameters (e.g. venueId, revision)
status              -- pending, running, succeeded, failed
owner_instance_id   -- ID of OWPROV instance currently executing the job
lease_expires_at    -- Heartbeat lease expiration timestamp to detect worker crashes
attempt_count       -- Number of execution attempts
max_attempts        -- Maximum allowed execution attempts (e.g. 2)
result              -- JSON result details (e.g. success/failed device lists)
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
1. Validate the request.
2. Insert a row into the jobs table with status 'pending', attempt_count = 0, and user_id.
3. Return the jobId and initial 'pending' status to the caller immediately via HTTP.
4. A worker loop on an OWPROV instance atomically claims the pending job from PostgreSQL:

   UPDATE jobs
   SET
     status = 'running',
     owner_instance_id = :instance_id,
     lease_expires_at = :now + :lease_interval,
     attempt_count = attempt_count + 1,
     started_at = COALESCE(started_at, :now)
   WHERE id = :job_id
     AND status = 'pending'
   RETURNING *;

5. Only the instance that successfully receives the updated row executes the background job thread.
```

### 15.4 Lease renewal and heartbeat

While executing a job, the owning instance must maintain an active lease:

```text
1. Running jobs have an active lease_expires_at timestamp.
2. The executing worker periodically updates lease_expires_at (e.g., every 10 seconds with a 30-second lease window).
3. Upon task completion, the worker updates status to 'succeeded' or 'failed', persists result/error_message, sets completed_at, and triggers notification delivery (via Option A or Option B in Section 16).
```

### 15.5 Expired-owner recovery and retry rules

Surviving OWPROV instances periodically scan for crashed or abandoned jobs:

```text
1. Scan for jobs where status = 'running' AND lease_expires_at < :now.
2. If attempt_count < max_attempts:
   - Another instance atomically reclaims the job.
   - The reclaim event is logged with old owner, new owner, and attempt count.
   - The new owner resumes or restarts execution.
3. If attempt_count >= max_attempts:
   - The job is transitioned to terminal status 'failed'.
   - error_message is set to 'Worker crashed and maximum execution attempts exceeded'.
   - completed_at is recorded.
```

### 15.6 Device-side idempotency

Background tasks must record incremental progress or device-level execution states where possible. On crash recovery, operations are not blindly re-executed on devices that already completed the action.

### 15.7 Job query endpoint

Any OWPROV instance can serve job status queries from PostgreSQL:

```text
GET /api/v1/jobs/{id}
```

The UI or API client can query any instance to inspect job status, progress, and results directly from PostgreSQL without requiring affinity to the instance that created or executed the job.

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

The Kafka notification message should directly wrap OWPROV's existing `WebSocketNotification` payload:

```text
user: target user email (or empty string for broadcast to all users)
type_id: notification type id (e.g. 1000 = fw_upgrade, 2000 = config_update, 3000 = rebooter)
payload: JSON object containing { notification_id, type_id, content }
source_instance_id: instance identifier that generated the event
created_at: timestamp
```

Example Kafka JSON message:

```json
{
  "user": "user@example.com",
  "type_id": 3000,
  "source_instance_id": "owprov-2",
  "created_at": 1726400000,
  "payload": {
    "notification_id": 105,
    "type_id": 3000,
    "content": {
      "title": "Venue Reboot",
      "jobId": "a1b2c3d4-...",
      "details": "Job Completed: 50 rebooted, 0 failed.",
      "timeStamp": 1726400000,
      "success": ["001122334455"],
      "warning": []
    }
  }
}
```

When an OWPROV instance consumes this message:
- If `user` is non-empty: calls `UI_WebSocketClientServer()->SendToUser(user, type_id, payload_str)`
- If `user` is empty: calls `UI_WebSocketClientServer()->SendToAll(type_id, payload_str)`

### 16.4 Delivery behavior (Option A)

Required behavior:

```text
1. Instance generating the event publishes a notification event.

2. Every OWPROV instance receives the notification event.

3. Each instance checks whether it has matching local WebSocket clients.

4. The instance holding the socket delivers the event to the browser.

5. If no socket is connected, durable job/status state remains queryable through API where required.
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
- OWPROV_INSTANCE_ID
- Kafka client.id values
- broadcast consumer group id
- logs/metrics identity
- job owner identity
```

### 18.4 Health and readiness

The load balancer must route traffic only to ready OWPROV instances.

An instance is ready only when:

```text
- database startup coordination has completed;
- PostgreSQL is reachable;
- Redis is reachable and ready;
- Kafka required consumers/producers are ready;
- required runtime files are downloaded and validated;
- required service identity configuration is valid;
- the instance is not draining.
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
3. Instance joins Kafka consumers.
4. Instance completes PostgreSQL, Redis, Kafka, runtime file, and readiness checks.
5. Load balancer starts routing traffic.
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

### Phase 1: Documentation and review

```text
- requirements.md reviewed
- spec.md reviewed
```

### Phase 2: Database and API safety

```text
- PostgreSQL startup lock
- Redis shared cache-aside support
- PostgreSQL fallback on Redis cache miss
- cache invalidation after committed POST/PUT/DELETE operations
- AuthCache refactored away from process-local authorization decision state
- SerialNumberCache refactored away from process-local serial/inventory decision state
- DeviceTypeCache refactored away from process-local device type validation state
- serial/inventory DB enforcement
```

### Phase 3: Kafka behavior

```text
- relationship write protection
- GroupConsumer/BroadcastConsumer behavior
- service_events broadcast handling
- connection work-queue handling
- commit/retry behavior
- producer partitioning review
```

### Phase 4: Jobs and notifications

```text
- durable PostgreSQL job table
- job status query endpoint
- background job status/result persistence
- WebSocket notification bus or PostgreSQL job polling
- cross-instance notification/status delivery
```

### Phase 5: Runtime and deployment

```text
- runtime file validation
- Docker Compose multi-instance config
- readiness/drain behavior
- scale out/in validation
```
