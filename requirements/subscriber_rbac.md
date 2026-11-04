# Subscriber RBAC Requirements

**Repository:** `ra-wlan-cloud-owprov`  
**Issue:** #90  
**Status:** Proposed  

---

## 1. Objective

This document defines the functional, security, and operational requirements for Subscriber Role-Based Access Control (RBAC) and self-service authorization in the OpenWifi Provisioning Service (`owprov`).

The primary objective is to enable authenticated subscribers to securely execute explicitly permitted self-care operations within their own authorized identity and resource ownership scope, while ensuring that every subscriber request is authenticated, capability-checked, ownership-scoped where a target resource identifier exists, and strictly constrained to authorized operations and field allowlists.

This specification establishes a dedicated, fail-closed subscriber authorization pipeline evaluated at the `RESTAPIHandler::RoleIsAuthorized()` choke point prior to administrative role resolution, leaving existing administrative RBAC policies and workflows completely intact.

---

## 2. Background

The introduction of the enterprise RBAC engine in `owprov` caused all authenticated subscriber self-service API requests to be rejected with `HTTP 403 ACCESS_DENIED`.

The administrative authorization engine resolves permissions by querying `ManagementRole` records from `RolesDB` and matching them against policies in `PolicyDB`. Subscribers are consumer identities authenticated by the OpenWifi Security Service (`owsec`); they do not possess `ManagementRole` assignments. Consequently, when subscriber requests reach `RESTAPIHandler::RoleIsAuthorized()`, the administrative evaluation path fails to match any role and denies the request, even when the subscriber attempts legitimate self-care operations on their own claimed devices or profile.

The required operational model for subscribers differs fundamentally from administrative users:
1. Administrative users possess configurable, hierarchical grants scoped to `Entity` and `Venue` organizational trees.
2. Subscribers possess uniform, platform-invariant self-care capabilities strictly bounded to their authenticated user identity and directly owned resources.

To resolve this issue without polluting administrative role storage with synthetic management roles, `owprov` requires a dedicated subscriber authorization path that enforces static, exact-match capabilities and direct resource ownership scoping (`resource.subscriberId == CallerId`).

---

## 3. Scope

### 3.1 In Scope

The requirements in this document apply strictly to subscriber self-service authorization for three active resource categories:
1. **`Subscriber`**: Profile inspection, hardware MAC address updates, and account self-deletion.
2. **`SubscriberDevice`**: Collection retrieval, individual device inspection, device claiming on sentinel path `/0`, defensive device inspection, and device unclaiming/deletion.
3. **`Venue`**: Venue inspection and descriptive metadata updates.

In-scope functional and security requirements comprise:
- Deriving authoritative subscriber identity strictly from validated Bearer tokens (`UserInfo_.userinfo.id`).
- Evaluating a dedicated subscriber authorization branch at `RoleIsAuthorized()` prior to administrative role lookups.
- Enforcing static, in-code, exact-match subscriber capabilities with zero method aliasing.
- Scoping collection queries strictly to the authenticated subscriber (`WHERE subscriberId = CallerId`) with no operator-wide fallback.
- Verifying direct resource ownership (`TargetResource.subscriberId == CallerId` or `venue.subscriber == CallerId`) whenever an endpoint targets a specific resource identifier.
- Enforcing server-derived ownership on device creation, requiring the sentinel path `/api/v1/subscriberDevice/0`.
- Enforcing strict subscriber mutation boundaries and administrative property immutability.
- Enforcing direct resource ownership across all subscriber-accessible device, profile, and venue resources.
- Enforcing the architectural separation between unauthenticated internal registration (Flow A) and authenticated subscriber self-service (Flow B).
- Enforcing semantic differentiation between authorization rejections (`HTTP 403 ACCESS_DENIED`) and request content/schema violations (`HTTP 400 Bad Request`).
- Enforcing fail-closed behavior across all endpoints, paths, operations, query parameters, and ambiguous contexts.
- Enforcing direct resource ownership invariants across all subscriber operations.
- Preserving administrative RBAC behavior (`FindAllUserRoles()`, `PolicyAllows()`, `ManagementRole`, `PolicyDB`) without regression.

### 3.2 Implementation Priority Framework

To resolve the regression in production without destabilizing existing cross-service subscriber flows, the upcoming implementation PR for Issue #90 will deliver and verify functionality across three prioritized tiers:

#### Tier 1: Active Production Subscriber Path (P0 — Mandatory Delivery)
The active production call path consists of the exact endpoints invoked by UserPortal (`owsub`) during live Mobile App operations. Making these endpoints fully authorized, functionally verified, and covered by end-to-end integration tests is the primary objective of the implementation PR:
1. `GET /api/v1/subscriberDevice`: Collection retrieval used by UserPortal to locate the root gateway (`deviceGroup == "olg"`) for Wi-Fi actions, topology, and parental controls. Must tolerate upstream query parameters (`operatorId`).
2. `GET /api/v1/subscriberDevice/{mac}`: Individual device lookup during device claim verification.
3. `POST /api/v1/subscriberDevice/0`: Sentinel creation path for onboarding root gateways and mesh extenders.
4. `DELETE /api/v1/subscriberDevice/{mac}`: Device unclaim and inventory disassociation.
5. `DELETE /api/v1/subscriber/{id}`: Subscriber account self-deletion via UserPortal proxy.

#### Tier 2: Defensive & Administrative Endpoints (P1 — Capability Retention)
Endpoints defined in `owprov`'s OpenAPI specification that are not invoked by UserPortal's active Mobile App flow today (e.g. profile inspection/update). Standalone device mutation (`PUT /api/v1/subscriberDevice/{id}`) is explicitly classified as an administrative-only endpoint (denied fail-closed for subscribers).

#### Tier 3: Internal Microservice Principals (Track 1 / Flow A)
Endpoints invoked by UserPortal or other ecosystem microservices using trusted internal service credentials (`Internal_ == true`, valid `X-API-KEY` with verified `X-INTERNAL-NAME`), such as `POST /api/v1/subscriber` (signup) and internal venue/location lookups. All internal service calls bypass the subscriber authorization pipeline entirely and proceed directly to existing internal microservice handler execution.

### 3.3 UserPortal (owsub) Call Chain Mapping

In production, subscribers interact exclusively with UserPortal (`owsub`) via the Mobile App. UserPortal acts as a Backend-For-Frontend (BFF), orchestrating calls to `owprov`, `owgw`, and `owsec`.

| Mobile App Feature | UserPortal (`owsub`) API | Downstream `owprov` Target | Auth Type | Priority Tier |
|---|---|---|:---:|:---:|
| **Signup (Flow A)** | `POST /api/v1/subscriber` | `POST /api/v1/subscriber` | Internal (`X-API-KEY`) *(Bypasses RBAC)* | Tier 3 |
| **Claim Device** | `POST /api/v1/subscriber/devices/{mac}` | 1. `GET /api/v1/subscriberDevice`<br/>2. `GET /api/v1/subscriberDevice/{mac}`<br/>3. `POST /api/v1/subscriberDevice/0` | Bearer Token | **Tier 1 (P0)** |
| **Unclaim Device** | `DELETE /api/v1/subscriber/devices/{mac}` | `DELETE /api/v1/subscriberDevice/{mac}` | Bearer Token | **Tier 1 (P0)** |
| **Wi-Fi Settings** | `POST /api/v1/action?action=configure` | `GET /api/v1/subscriberDevice` *(resolves gateway serial; pushes config to `owgw`)* | Bearer Token | **Tier 1 (P0)** |
| **Hardware Action**| `POST /api/v1/action?action={cmd}` | `GET /api/v1/subscriberDevice` *(verifies ownership; dispatches to `owgw`)* | Bearer Token | **Tier 1 (P0)** |
| **Delete Account** | `DELETE /api/v1/subscriber` | `DELETE /api/v1/subscriber/{id}` | Bearer Token | **Tier 1 (P0)** |
| **Location / Venue** | `GET/POST/PUT/DELETE /api/v1/subscriber/location` | `GET/PUT /api/v1/venue`<br/>`GET/PUT/DELETE /api/v1/location` | Internal (`X-API-KEY`) *(Bypasses RBAC)* | Tier 3 |
| **Topology View** | `GET /api/v1/topology` | `GET /api/v1/subscriberDevice`<br/>`GET /api/v1/venue/{id}` | Bearer Token / Internal *(Bypasses RBAC)* | **Tier 1 (P0)** |
| **Parental Controls**| `POST /api/v1/groups`, `POST /api/v1/schedules` | `GET /api/v1/subscriberDevice` *(resolves gateway serial; pushes firewall to `owgw`)* | Bearer Token | **Tier 1 (P0)** |
| **Device Mutation**| *(Not called by Mobile App today)* | `PUT /api/v1/subscriberDevice/{id}` | Bearer Token | **Denied** *(Admin-only)* |

> [!IMPORTANT]
> **Production Call Path Invariants:**
> 1. **Internal Service RBAC Bypass:** All inter-service calls arriving on the internal router with valid internal credentials (`Internal_ == true` with `MicroServiceIsValidAPIKEY(*Request)`) bypass subscriber RBAC entirely and proceed to existing internal microservice handler execution. This bypass does not expand or alter existing service routing or privileges.
> 2. **Subscriber Wi-Fi Configuration Flow:** The existing UserPortal flow routes device Wi-Fi configuration mutations through `POST /api/v1/action?action=configure` directly to `owgw`. Direct invocation of `PUT /api/v1/subscriberDevice/{id}` by subscribers on `owprov` is denied fail-closed (`HTTP 403 ACCESS_DENIED`).

### 3.4 Out of Scope

The following areas are explicitly excluded from the scope of Issue #90:
- **Standalone Configuration Endpoints**: Direct subscriber access to `/api/v1/configuration*` is administrative-only and denied fail-closed.
- **Administrative RBAC Redesign**: Modifications to administrative role structures, policy inheritance, entity hierarchy, or staff permissions.
- **Hardware Proof-of-Possession**: Physical hardware entitlement, challenge-response verification, or supply-chain validation during device claiming.
- **Database / ORM Overhaul**: General ORM refactoring, database schema migrations, composite index additions, query restructuring, or caching layers.
- **Concurrency and TOCTOU Hardening**: Race-condition and TOCTOU hardening are intentionally deferred to a follow-up issue. This specification defines authorization and ownership invariants only and does not require transaction/concurrency redesign as part of Issue #90.
- **Cross-Service Reconciliation**: Distributed event synchronization, background state reconciliation between `owprov` and `owgw`, or message bus delivery semantics.
- **General Security Features**: Global API rate limiting, IP reputation filtering, token refresh protocol redesign, or audit logging overhaul.

---

## 4. Authorization Requirements

Subscriber self-service requests MUST be authorized strictly according to the multi-stage pipeline defined below.

```text
Bearer Token Authentication (owsec)
        |
        v
Authenticated Subscriber Identity Resolution
(CallerId = UserInfo_.userinfo.id)
        |
        v
Dedicated Subscriber Branch Selection
(Evaluated in RoleIsAuthorized() before Administrative RBAC)
        |
        v
Static Exact-Match Capability Check
(Zero Method Aliasing)
        |
        v
Method / Path / Operation Guard
        |
        v
Resource Ownership Check
(Enforced if request specifies an explicit target resource ID)
        |
        v
Mutation Boundary Authorization
        |
        v
Existing Business Operation Execution
```

Authorization MUST fail closed at the earliest failing stage. A request MUST NOT proceed to execution unless every applicable stage succeeds.

### 4.1 Subscriber Identity

1. **Authoritative Source**: The system MUST derive the authenticated subscriber identity exclusively from the validated Bearer token payload via `UserInfo_.userinfo.id` (designated as `CallerId`).
2. **Context Immutability & Provenance**: `CallerId` and `userRole` MUST originate exclusively from trusted authentication processing of the validated cryptographic token context (JWT issued and signed by `owsec`). They MUST remain immutable for the entire lifecycle of the HTTP request and MUST NEVER be derived from, influenced by, or overridden by incoming request headers, query parameters, or request body fields.
3. **No Synthetic Role Injection**: The system MUST NOT synthesize `ManagementRole` objects or insert artificial subscriber roles into `FindAllUserRoles()`.
4. **Mandatory Identity Context**: If `UserInfo_.userinfo.id` is null, empty, or unresolvable, the system MUST terminate authorization immediately and return `HTTP 403 ACCESS_DENIED`.
5. **No Ambient Identity**: An unauthenticated or anonymous request MUST NOT be evaluated under the subscriber authorization pipeline.
6. **Principal Classification Gate**: Incoming requests evaluated at `RoleIsAuthorized()` MUST be classified into one of four distinct principal tracks:
   - **Track 1 (Internal Microservice Principal)**: Established exclusively when a request arrives on the internal router interface (`Internal_ == true`, bound to trusted internal network infrastructure) accompanied by valid inter-service credentials verified via `MicroServiceIsValidAPIKEY(*Request)` with a verified `X-INTERNAL-NAME` header matching a registered ecosystem service (such as `owsec`, `UserPortal`, or `owgw`). Internal service calls MUST bypass the subscriber authorization pipeline and proceed directly to existing internal-service handler execution and routing in `owprov`. If an internal request fails API key or service identity validation, it MUST fail closed immediately (`HTTP 401 Unauthorized` / `HTTP 403 Forbidden`) and MUST NEVER fall through to subscriber (Track 2) or administrative (Track 3) authorization pipelines.
   - **Track 2 (Authenticated Subscriber Principal)**: When a request presents a valid Bearer token and the authentication context explicitly classifies the caller as a subscriber principal (`UserInfo_.userinfo.userRole == SecurityObjects::USER_ROLE::SUBSCRIBER`), the request MUST enter the dedicated subscriber authorization pipeline. A non-empty `UserInfo_.userinfo.id` alone MUST NOT classify the caller as a subscriber.
   - **Track 3 (Administrative Principal)**: Administrative principals (`ROOT`, `Admin`, `CSR`, `Installer`, `NOC`, `Accounting`) MUST NOT enter the subscriber pipeline and MUST evaluate strictly under administrative RBAC.
   - **Track 4 (Unrecognized / Unknown External Principal)**: Requests that do not qualify as Track 1, Track 2, or Track 3 MUST fail closed immediately with `HTTP 403 ACCESS_DENIED`.

### 4.2 Subscriber Capability

1. **Static In-Code Policy**: Subscriber authorization policy is a static in-code policy and MUST NOT depend on administrative `RolesDB` or `PolicyDB` resolution.
2. **Dedicated and Explicit**: The subscriber policy MUST be dedicated to subscriber self-service authorization, explicit, and fail-closed.
3. **No Management Role Dependency**: Subscriber policy MUST NOT be derived from `ManagementRole` records, MUST NOT require `FindAllUserRoles()`, and MUST NOT introduce synthetic management roles into administrative storage.
4. **Evaluation Precedence & Principal Gating**: When `RoleIsAuthorized()` evaluates a request, the subscriber authorization branch MUST be entered exclusively when principal classification confirms `userRole == SUBSCRIBER`. Administrative principals MUST continue directly to administrative RBAC resolution, preventing administrative users from being restricted by subscriber policies.
5. **Zero Method Aliasing**: The system MUST enforce exact-match HTTP method authorization.
   - An explicit grant for `GET` MUST NOT authorize `POST`, `PUT`, or `DELETE`.
   - An explicit grant for `PUT` MUST NOT authorize `POST` or `DELETE`.
   - Method-aliasing semantics in `PolicyAllows()` (such as `UPDATE` satisfying `CREATE`) MUST NOT be applied to subscribers.
6. **Capability Matrix**: The system MUST enforce the following exact-match capability matrix:

| Resource | `GET` | `POST` | `PUT` | `DELETE` | Capability Rule |
|---|:---:|:---:|:---:|:---:|---|
| **`subscriberDevice`** | **Allowed** | **Allowed** | **Denied** | **Allowed** | Device inspection, sentinel creation (`/0`), and unclaiming. `PUT` is denied fail-closed (`HTTP 403 ACCESS_DENIED`); subscriber Wi-Fi configuration mutations route via UserPortal directly to `owgw`. |
| **`subscriber`** | **Allowed** | **N/A (Flow A)** | **Allowed** | **Allowed** | Profile inspection, MAC update, and bare self-deletion. `POST` is an unauthenticated registration flow outside subscriber RBAC. |
| **`venue`** | **Allowed** | **Denied** | **Allowed** | **Denied** | Venue inspection and metadata update only. Creation and deletion are administrative. |
| **`configuration`** | **Denied** | **Denied** | **Denied** | **Denied** | Administrative-only resource. All subscriber methods denied fail-closed. |
| *(Any Other Resource)* | **Denied** | **Denied** | **Denied** | **Denied** | Unknown or unspecified resources fail closed immediately. |

### 4.3 Resource Ownership

The system MUST distinguish between endpoints with and without explicit target resource identifiers.

#### 4.3.1 Case A — Endpoints Without Target Resource ID
When an authorized endpoint does not contain a target resource ID in its URI path (e.g., `GET /api/v1/subscriber`, `PUT /api/v1/subscriber`, bare `DELETE /api/v1/subscriber`, and `GET /api/v1/subscriberDevice`):
1. The system MUST bind the operational scope directly and implicitly to `CallerId = UserInfo_.userinfo.id`.
2. The system MUST NOT require a client-supplied `subscriberId` merely to establish identity. If `subscriberId` is supplied, the common Section 4.6 consistency rule applies.

#### 4.3.2 Case B — Endpoints With Target Resource ID
When an authorized endpoint contains an explicit target resource ID in its URI path (e.g., `/api/v1/subscriberDevice/{id}` or `/api/v1/venue/{id}`):
1. The target resource ID MUST be treated strictly as an object locator, NOT an authorization assertion. For `subscriberDevice` endpoints, `:resourceId` MAY be either a canonical UUID or a valid hardware serial number, matching `LoadDeviceFromBinding()` resolution.
2. **Single-Pass Loading, In-Memory Ownership Assertion, and Uniform 404 Masking**:
   - The system MUST load the target resource by its identifier (`:resourceId`, via UUID or hardware serial number for devices) in a single pass.
   - **Uniform 404 Masking for Existence-Oracle Prevention**:
     - To prevent authenticated subscribers from enumerating, scanning, or probing valid device UUIDs, hardware serial numbers, or venue IDs across the platform, the system MUST return a uniform `HTTP 404 Not Found` whenever a target resource is inaccessible to the caller:
       a) If the target resource does not exist in the database $\longrightarrow$ `HTTP 404 Not Found`.
       b) If the target resource exists in the database but is owned by another subscriber or unassigned (`Device.subscriberId != CallerId` or `Venue.subscriber != CallerId`) $\longrightarrow$ `HTTP 404 Not Found`.
     - **Scope Boundary**: This uniform masking eliminates the primary direct HTTP existence oracle for protected resource identifiers. Potential indirect micro-architectural side channels (e.g. slight latency/timing variations or downstream network diagnostics) are outside the scope of Issue #90.
     - **Account Self-Deletion Identity Exception**: In contrast to object-locator endpoints, the parameterized account deletion path (`DELETE /api/v1/subscriber/{id}`) represents a self-identity assertion rather than an object lookup. If an authenticated subscriber invokes `DELETE /api/v1/subscriber/{id}` with an `{id}` that does not match `CallerId`, the system MUST return `HTTP 403 ACCESS_DENIED` (Consistent Identity Violation per Section 5.1.5), rather than `404 Not Found`.
     - **Internal Diagnostic Logging**: The system MUST retain distinct internal logging reasons (e.g., `AUTH_DEBUG: Denied - Foreign resource ownership` vs `AUTH_DEBUG: Denied - Resource not found in database`) for operational telemetry, audit trails, and troubleshooting, while ensuring the external HTTP response remains identical (`HTTP 404 Not Found`).
   - For authorized requests (`subscriberId == CallerId`), the pre-loaded entity MUST be passed/memoized within the request context so downstream business handlers do not reload the same resource from the database.
3. **Point-of-Mutation Ownership Check**:
   - For mutations (`PUT`, `DELETE`), direct resource ownership MUST be verified from the loaded record prior to executing the database modification.
   - Low-level database concurrency primitives, row-level locking, and distributed multi-statement atomic conditional SQL queries (e.g., `WHERE id = :resourceId AND subscriberId = :CallerId`) are explicitly deferred to the follow-up concurrency issue ("Issue #90-Followup: Concurrency and TOCTOU DB-level serialization").
4. Resource ownership verification MUST be evaluated as an individual direct resource ownership check (`resource.subscriberId == CallerId` or `venue.subscriber == CallerId`); it MUST NOT be delegated to tenant or operator checks.

### 4.4 Collection Scoping and Bounded Retrieval

1. **Unconditional Query Constraint**: For subscriber collection requests (`GET /api/v1/subscriberDevice`), the system MUST constrain the underlying database query strictly to:
   ```sql
   WHERE subscriberId = :CallerId
   ```
2. **Zero Operator Fallback**: If the query returns zero records, or if request query parameters are omitted, the query MUST NOT broaden or fall back to operator-wide, entity-wide, or venue-wide visibility.
3. **Consistent Scoping and Parameter Tolerance**:
   - **Client-Supplied `subscriberId`**: If `subscriberId` is omitted, the collection query MUST automatically inject `WHERE subscriberId = :CallerId`. If `subscriberId` is explicitly supplied in the query string, the consistent `subscriberId` rule (Section 4.6.3) MUST be enforced: if `subscriberId == CallerId`, the query proceeds; if `subscriberId != CallerId`, the request MUST be rejected immediately with `HTTP 403 ACCESS_DENIED`. The server MUST NOT silently ignore or rewrite a mismatched `subscriberId`.
   - **Upstream Parameter Tolerance & Semantic Inertia (`operatorId`)**: Context parameters routinely supplied by upstream clients (such as `operatorId` passed by UserPortal in `GET /api/v1/subscriberDevice?operatorId=...`) MUST be tolerated with semantic inertia. For subscriber principals, the presence of `operatorId` MUST NOT trigger operator existence validation (`OperatorDB.Exists()`), MUST NOT dispatch to operator-level collection logic (`ListHandlerForOperator`), MUST NOT fail the request, and MUST NOT loosen or alter the query predicate from `WHERE subscriberId = :CallerId`.
4. **Query Limits and Result Bounds**: Collection requests MUST preserve existing query limit and pagination behavior implemented in the underlying handler and database query (`DB.GetRecords(Offset, Limit, ...)`), operating strictly within the mandatory `WHERE subscriberId = :CallerId` predicate without imposing artificial limits that could truncate or alter existing upstream caller workflows.
5. **Scoping Invariant Across Pagination**: Pagination (`limit`, `offset`), filtering, and sorting parameters MUST operate strictly within the `subscriberId = :CallerId` predicate. Client parameters MUST NOT be permitted to loosen or bypass the subscriber ownership boundary. Implementations MUST apply pagination clauses directly in the database engine query; fetching unconstrained result sets into application memory followed by in-memory filtering is strictly prohibited.

### 4.5 Fail-Closed Authorization

1. **Evaluation Order**: The subscriber authorization pipeline MUST be evaluated in `RESTAPIHandler::RoleIsAuthorized()` BEFORE `FindAllUserRoles()`, BEFORE administrative `ManagementRole` database queries, and BEFORE staff/operator global GET shortcuts, conditioned strictly upon subscriber principal classification (`userRole == SUBSCRIBER`).
2. **Authorization Rejection Precedence**: If a subscriber request fails authorization conditions—including capability checks, unauthorized method/path operations, missing or unresolvable identity context, or attempts to modify unauthorized properties—the request MUST fail closed with `HTTP 403 ACCESS_DENIED` (with inaccessible foreign resources on target resource endpoints uniformly masked to `HTTP 404 Not Found` per Section 4.3.2).
3. **Invalid Request Precedence**: If an authorized subscriber request contains malformed request syntax, unparseable JSON, or malformed data types, the request MUST fail closed with `HTTP 400 Bad Request` in accordance with Section 9.
4. **No Cross-Pipeline Fallthrough**: A rejected or unmatched subscriber request MUST NOT fall through to administrative RBAC evaluation. Conversely, administrative principals accessing subscriber-capable endpoints (e.g., `GET /api/v1/subscriberDevice` or `DELETE /api/v1/subscriber/{id}`) MUST NOT enter the subscriber authorization pipeline and MUST evaluate strictly under administrative RBAC.
5. **Path and Operation Boundary**:
   - When a requested path or operation is syntactically recognized but represents an action or path variant the subscriber is not permitted to invoke (such as `POST /api/v1/venue`, `DELETE /api/v1/venue/{id}`, or `POST /api/v1/subscriberDevice/{non-zero-id}`), authorization MUST fail closed with `HTTP 403 ACCESS_DENIED`.
   - When the request structure or path parameter itself is malformed or cannot be interpreted as a valid request representation by the parsing layer, the request MUST fail closed with `HTTP 400 Bad Request`.

### 4.6 Client-Supplied Identity and Scope

1. **Zero Authority**: Client-supplied identifiers—including `subscriberId`, `operatorId`, `tenantId`, `entityId`, and `email`—have zero authority to establish or expand subscriber authorization scope.
2. **Authoritative Subscriber Identity**: The Bearer-token-derived `CallerId` (`UserInfo_.userinfo.id`) remains the authoritative subscriber identity for all subscriber self-service operations.
3. **Consistent `subscriberId` Rule**: Wherever a subscriber endpoint accepts or processes an optional client-supplied `subscriberId`:
   - **If `subscriberId` is supplied**: It MUST equal `CallerId`. If `subscriberId != CallerId`, the request MUST be rejected with `HTTP 403 ACCESS_DENIED`. If `subscriberId == CallerId`, the operation proceeds.
   - **If `subscriberId` is not supplied**: The system MUST use `CallerId` derived from the validated Bearer token, and the operation proceeds.
   - **Scope Invariant**: A client-supplied `subscriberId` does not create or expand authorization scope; when supplied, it acts solely as an additional consistency check against `CallerId`.
   - **Optional Presence**: This rule MUST NOT be interpreted as requiring `subscriberId` to be present in client requests.
4. **Creation Ownership Enforcement**: On device creation (`POST /api/v1/subscriberDevice/0`), the persisted ownership MUST be assigned directly from the authenticated caller:
   ```cpp
   newDevice.subscriberId = CallerId;
   ```
   A client MUST NOT be able to choose, assign, or transfer ownership to any other subscriber. If the creation payload includes a `subscriberId`, the consistent `subscriberId` rule above applies (`subscriberId != CallerId` returns `HTTP 403 ACCESS_DENIED`).
5. **Elimination of Tenant and Operator Logic**: The subscriber authorization path MUST NOT evaluate `CallerTenant`, tenant equality checks, multi-tenant derivation logic, entity hierarchy checks, or operator boundaries. Subscriber authorization is strictly bounded by `CallerId` and direct resource ownership.
6. **Client Parameter Tolerance & Semantic Inertia**: Upstream consumer gateways and portals (such as UserPortal) routinely submit context parameters such as `operatorId` in collection queries and creation payloads (`POST /api/v1/subscriberDevice/0`). The subscriber authorization layer MUST tolerate client-supplied `operatorId` with complete semantic inertia (it confers zero authorization authority, does not trigger operator verification checks, and does not alter ownership assignment or query predicates), without failing or rejecting the request.

---

## 5. Endpoint Requirements

The system MUST enforce subscriber authorization across all endpoints in accordance with the specifications below.

| Method | Endpoint | Subscriber Access | Required Scope & Authorization Rule |
|---|---|:---:|---|
| `POST` | `/api/v1/subscriber` | **N/A (Flow A)** | Unauthenticated registration flow via internal service authentication (`X-API-KEY`); outside subscriber RBAC capability set. |
| `GET` | `/api/v1/subscriber` | **Allow** | Scoped strictly to `CallerId`; prohibits `email`, `macAddress`, `listOnly`; subscriber-safe projection. |
| `PUT` | `/api/v1/subscriber` | **Allow** | Scoped strictly to `CallerId`; permitted operation `updateMac` only; verifies MAC availability. |
| `DELETE` | `/api/v1/subscriber` | **Allow** | Bare path; scoped to `CallerId`; requires active claimed device count == 0. |
| `DELETE` | `/api/v1/subscriber/{id}` | **Allow (Consistent Identity)** | Parameterized path invoked by UserPortal proxy; permitted when `{id} == CallerId` and active claimed device count == 0. If `{id} != CallerId`, returns `403 ACCESS_DENIED`. |
| `GET` | `/api/v1/subscriberDevice` | **Allow** | Collection scoped strictly to `WHERE subscriberId = CallerId`; no operator fallback. |
| `GET` | `/api/v1/subscriberDevice/{id}` | **Allow** | Verifies target device ownership (`device.subscriberId == CallerId`); uniform `404` on missing or foreign device. |
| `POST` | `/api/v1/subscriberDevice/0` | **Allow** | Sentinel path `/0` only; forces `newDevice.subscriberId = CallerId`; verifies claim eligibility. |
| `PUT` | `/api/v1/subscriberDevice/{id}` | **Denied** | Administrative-only endpoint; direct subscriber device/configuration mutation on `owprov` is denied fail-closed (`HTTP 403 ACCESS_DENIED`). Subscriber Wi-Fi configuration mutations route via UserPortal directly to `owgw`. |
| `DELETE` | `/api/v1/subscriberDevice/{id}` | **Allow** | Verifies device ownership (uniform `404` on missing/foreign) + executes unclaim workflow. |
| `GET` | `/api/v1/venue/{id}` | **Allow** | Verifies target venue ownership (`venue.subscriber == CallerId`); uniform `404` on missing or foreign venue. |
| `PUT` | `/api/v1/venue/{id}` | **Allow** | Verifies venue ownership (uniform `404` on missing/foreign) + positive allowlist. |
| `POST` | `/api/v1/venue` | **Denied** | Administrative-only endpoint; rejected fail-closed with `403 ACCESS_DENIED`. |
| `DELETE` | `/api/v1/venue/{id}` | **Denied** | Administrative-only endpoint; rejected fail-closed with `403 ACCESS_DENIED`. |
| `*` | `/api/v1/configuration*` | **Denied** | Standalone Configuration API is administrative-only; all methods return `403 ACCESS_DENIED`. |

---

### 5.1 Subscriber

#### 5.1.1 POST `/api/v1/subscriber`
1. `POST /api/v1/subscriber` is an **unauthenticated-user registration flow** (Flow A) executed via UserPortal with internal service authentication (`X-API-KEY`).
2. It is **not a subscriber self-service capability** and is outside the subscriber RBAC capability set entirely. It MUST NOT be evaluated under or enter the subscriber Bearer-token authorization pipeline.
3. Direct external invocation without valid trusted internal service authentication MUST NOT be permitted. The existing internal service authentication layer and signup workflow remain responsible for this operation and determine the appropriate HTTP status for missing or invalid credentials.
4. The system MUST NOT require subscriber identity, Bearer-token validation, `CallerId` ownership assertions, `subscriberId == CallerId` comparisons, or subscriber role lookups for this flow.

#### 5.1.2 GET `/api/v1/subscriber`
1. The system MUST authorize authenticated subscribers to retrieve their own subscriber profile.
2. **Prohibited Query Parameters**: To prevent account enumeration and cross-account data exposure, the system MUST prohibit the following query parameters for subscribers:
   - `email`
   - `macAddress`
   - `listOnly`
   If any of these prohibited parameters are present in a subscriber request, the system MUST reject the request fail-closed with `HTTP 403 ACCESS_DENIED`.
3. **`signupUUID` Parameter Handling**: If the client provides `?signupUUID=<id>`:
   - The system MUST load the corresponding `SignupEntry` (`SE`) from `SignupDB`.
   - If the record does not exist, or if `SE.userId != CallerId`, the system MUST return `HTTP 404 Not Found` (uniformly masked to prevent signup UUID existence probing, with internal logging of the diagnostic reason).
4. **Default Identity Resolution**: When no query parameters are present, the system MUST resolve the profile strictly via trusted caller identity: `SignupDB.GetRecord("userId", CallerId, SE)`.
5. **Subscriber-Safe Response Projection**: The system MUST filter the raw `SignupEntry` before returning the JSON payload, enforcing strict data confidentiality:
   - **Permitted Fields**: `id`, `userId`, `email`, `status`, `statusCode`, `macAddress`, `serialNumber`, `deviceID`, `registrationId`, `operatorId`, `submitted`, `completed`, `created`, `modified`.
   - **Redacted Fields**: Internal operator metadata—including `notes`, `tags`, and internal error traces—MUST be stripped from the response.

#### 5.1.3 PUT `/api/v1/subscriber`
The `PUT /api/v1/subscriber` endpoint serves two distinct operational lifecycles differentiated by the authenticated principal track:

1. **Subscriber Self-Service Lifecycle (`userRole == SUBSCRIBER`):**
   - The system MUST authorize authenticated subscribers to update their profile strictly when all the following conditions are satisfied:
     - The request specifies query parameter `operation=updateMac`.
     - The caller owns the signup record (`SE.userId == CallerId`).
   - **Operation Guard**: For subscriber self-service, `operation=updateMac` is the ONLY permitted operation. Any request specifying `operation=emailVerified`, an empty operation, or any arbitrary operation string MUST be rejected fail-closed with `HTTP 403 ACCESS_DENIED`.

2. **Internal Service Signup Lifecycle (Track 1 Internal Service):**
   - The registration workflow requires completing email verification via internal microservice calls.
   - When invoked by `owsec` via trusted internal service authentication (`Internal_ == true`, valid `X-API-KEY`, and `X-INTERNAL-NAME`), the operation `operation=emailVerified` accompanied by `signupUUID` MUST be permitted and dispatched to the internal verification handler to mark the subscriber email as verified.

3. **MAC Normalization, Latency, and Concurrency (Self-Service):**
   - The system MUST normalize the candidate MAC address into canonical lowercase 12-character hexadecimal format (via `Utils::NormalizeMac()`) before validation, uniqueness evaluation, or database lookup.
   - The system MUST verify that the normalized MAC address is unassigned across `SubscriberDeviceDB`, `InventoryDB`, and `SignupDB`.
   - **Explicit Failure Semantics for `operation=updateMac`**:
     | Condition | HTTP Status | Internal Diagnostic Code | Description |
     |---|---|---|---|
     | Missing `mac` parameter | `HTTP 400 Bad Request` | `MissingOrInvalidParameters` | Query parameter `mac` was not supplied. |
     | Malformed MAC syntax | `HTTP 400 Bad Request` | `ConfigBlockInvalid` | Value fails hex/length validation or `Utils::NormalizeMac()`. |
     | MAC already assigned | `HTTP 400 Bad Request` | `MACAddressExists` / `RecordNotUpdated` | Address already exists in `SubscriberDeviceDB`, `InventoryDB`, or `SignupDB`. |
     | Valid, unassigned MAC | `HTTP 200 OK` | `Success` | Profile record updated with normalized MAC address. |
   - Uniqueness checks SHOULD evaluate indexed tables first (short-circuiting immediately upon collision) to minimize multi-table latency.
   - Existing business validation checks MAC availability across tables at request time and rejects detected collisions; database-level transactional concurrency and race-free serialization are deferred to Issue #90-Followup.

#### 5.1.4 DELETE `/api/v1/subscriber` (Bare Path)
1. The system MUST authorize self-deletion on the bare URI path `/api/v1/subscriber` without path parameters.
2. The deletion operation MUST target `CallerId` directly.
3. **Multi-Venue & Active Device Aggregation Check**: The system MUST verify that no active claimed devices or populated venues remain associated with `CallerId` before permitting deletion:
   - `InventoryDB.Count("subscriber", CallerId) == 0`
   - `SubscriberDeviceDB.Count("subscriberId", CallerId) == 0`
   - **Venue Aggregation Procedure**: The system MUST query all venues associated with the subscriber (`VenueDB.GetRecords(0, 100, venues, " subscriber='" + CallerId + "' ")`). For every retrieved venue, the system MUST verify that its device collection is empty (`venue.devices.empty()`).
4. **Conditional Deletion**:
   - If active claimed devices remain associated with the subscriber in `InventoryDB`, `SubscriberDeviceDB`, or **any** owned venue contains devices (`!venue.devices.empty()`), the system MUST reject the request with `HTTP 400 Bad Request` (`StillInUse`).
   - If all owned venues contain zero devices (or if the subscriber owns zero venues) and device counts in `InventoryDB` and `SubscriberDeviceDB` are zero, the system MUST delete all subscriber-owned venues in `VenueDB`, and subsequently delete the subscriber record in `SignupDB`.

#### 5.1.5 DELETE `/api/v1/subscriber/{id}` (Parameterized Path)
1. **UserPortal Integration Context**: UserPortal (`owsub`) proxies subscriber account deletion by invoking `DELETE /api/v1/subscriber/{UserInfo_.userinfo.id}` using the authenticated subscriber's Bearer token.
2. **Subscriber Consistent Identity Rule**: When invoked by an authenticated subscriber principal (`userRole == SUBSCRIBER`):
   - The path parameter `{id}` MUST match the authenticated caller identity (`{id} == CallerId`).
   - If `{id} != CallerId`, the request MUST be rejected fail-closed with `HTTP 403 ACCESS_DENIED`.
   - If `{id} == CallerId`, authorization succeeds and proceeds to the active device aggregation check.
3. **Multi-Venue & Active Device Aggregation Check**:
   - The system executes the identical multi-venue aggregation check: `InventoryDB.Count("subscriber", CallerId) == 0 && SubscriberDeviceDB.Count("subscriberId", CallerId) == 0`, and iterates all venues matching `subscriber == CallerId` to ensure every venue satisfies `venue.devices.empty()`.
   - If active claimed devices remain associated with `CallerId` or any owned venue has active devices, the system MUST reject the request with `HTTP 400 Bad Request` (`StillInUse`).
   - If active claimed device count is zero across all sources, the system MUST delete all subscriber-associated venues, followed by deleting the subscriber record.
4. **Administrative Principal Evaluation**: When invoked by an administrative principal (`ROOT`, `Admin`, `CSR`), the endpoint bypasses the subscriber consistent identity rule and evaluates strictly through administrative RBAC, authorizing deletion of subscriber records according to administrative policy.

---

### 5.2 SubscriberDevice

#### 5.2.1 GET `/api/v1/subscriberDevice` (Collection)
1. The system MUST authorize authenticated subscribers to retrieve their claimed devices.
2. The database query MUST be constrained unconditionally to `WHERE subscriberId = CallerId`.
3. The response MUST contain only devices belonging to `CallerId`.
4. The system MUST NOT fall back to operator-wide visibility under any circumstances.

#### 5.2.2 GET `/api/v1/subscriberDevice/{id}` (Individual)
1. The system MUST load the target device from `SubscriberDeviceDB` using `{id}` (resolving by internal UUID or hardware serial number).
2. If the device does not exist, or if the device exists but does not belong to the caller (`device.subscriberId != CallerId` or unassigned), the system MUST return `HTTP 404 Not Found` in accordance with the Section 4.3.2 uniform masking rule, preventing hardware serial number and UUID existence enumeration.
3. The server MUST record the distinct authorization failure reason internally for audit and diagnostic purposes.

#### 5.2.3 POST `/api/v1/subscriberDevice/0` (Claiming & Creation)
1. **Mandatory Sentinel Path**: The system MUST permit subscriber device creation exclusively on the sentinel path `/api/v1/subscriberDevice/0`.
2. **Rejection of Arbitrary Paths**: Any request targeting alternate, syntactically recognized paths (such as `/new`, empty ID, an existing device UUID, or a MAC address) MUST be rejected fail-closed with `HTTP 403 ACCESS_DENIED`. Genuinely malformed request syntax or unparseable paths MUST be rejected with `HTTP 400 Bad Request`.
3. **Server-Derived Ownership and `subscriberId` Rule**: The system MUST derive device ownership strictly from the authenticated caller:
   ```cpp
   newDevice.subscriberId = CallerId;
   ```
   If the client provides an optional `subscriberId` in the request body, the consistent `subscriberId` rule (Section 4.6) MUST be enforced:
   - If `subscriberId == CallerId`, the creation operation proceeds.
   - If `subscriberId != CallerId`, the request MUST be rejected with `HTTP 403 ACCESS_DENIED`.
   - If `subscriberId` is omitted, the system relies on `CallerId`.
   Under no circumstances may client input establish, override, or expand ownership to any identity other than `CallerId`.
4. **Device Claim Eligibility & Scope Boundary**:
   - The system MUST enforce existing claim validations:
     - Serial number MUST NOT already exist in `SubscriberDeviceDB`.
     - Serial number MUST satisfy syntax validation (`ValidSerialNumber`).
     - `deviceGroup` MUST be specified and MUST be `"ap"` or `"olg"`.
     - A subscriber MUST NOT own more than one active OLG gateway.
   - **Hardware Entitlement Scope Boundary**: This RBAC specification enforces strict ownership isolation post-assignment and guarantees an authenticated subscriber cannot view, claim, or mutate devices already assigned to another subscriber. Upstream physical hardware entitlement and proof-of-possession verification (e.g. physical QR code scanning on the router label, optical barcode verification, or hardware push-button pairing challenges) are the operational trust boundary of upstream provisioning clients (Mobile App / installer flow) and are outside the scope of Issue #90.
5. **Server-Side Configuration Generation on Claim**: On device claim (`POST /api/v1/subscriberDevice/0`), `owprov` server-side automatically creates a dedicated, subscriber-owned configuration record (`cfg.subscriber = CallerId`, `cfg.subscriberOnly = true`) via `CreateConfigurationRecord()`. Any client-supplied `deviceConfiguration` UUID or reference to an existing or administrative configuration template is strictly ignored or rejected; client input can never associate a claimed subscriber device with a pre-existing, shared, or administrative configuration.

#### 5.2.4 PUT `/api/v1/subscriberDevice/{id}` (Administrative Only)
1. **Subscriber Denial**: Direct device and configuration mutation on `/api/v1/subscriberDevice/{id}` is strictly an administrative endpoint. Any attempt by an authenticated subscriber principal (`userRole == SUBSCRIBER`) to invoke `PUT /api/v1/subscriberDevice/{id}` MUST be rejected fail-closed with `HTTP 403 ACCESS_DENIED`.
2. **Production Architectural Flow Invariant**: In production, subscribers interact exclusively through UserPortal (`owsub`), which dispatches Wi-Fi configuration mutations directly to `owgw` via `POST /api/v1/action?action=configure`. Neither the live Mobile App nor UserPortal invokes `PUT /api/v1/subscriberDevice/{id}` for subscriber operations; therefore, direct subscriber invocation on `owprov` is completely prohibited.
3. **Administrative Evaluation**: Administrative principals (`ROOT`, `Admin`, `CSR`) invoking this endpoint evaluate strictly under administrative RBAC.

#### 5.2.5 DELETE `/api/v1/subscriberDevice/{id}` (Unclaiming & Removal)
1. The system MUST load the target device (resolving by UUID or hardware serial number) and verify ownership: `device.subscriberId == CallerId`. If the device does not exist, or if `device.subscriberId != CallerId`, the system MUST return `HTTP 404 Not Found` in accordance with the uniform masking rule (Section 4.3.2).
2. Upon successful ownership verification, the system MUST execute the existing device unclaim and inventory disassociation workflow.

---

### 5.3 Venue

#### 5.3.1 GET `/api/v1/venue/{id}`
1. The system MUST load the venue record from `VenueDB` using `{id}`.
2. If the venue does not exist, or if `venue.subscriber != CallerId`, the system MUST return `HTTP 404 Not Found` in accordance with the Section 4.3.2 uniform masking rule, preventing venue existence enumeration.

> [!NOTE]
> **UserPortal Integration Context**: When UserPortal queries venues or locations during topology synthesis via internal microservice credentials (`X-API-KEY`), the request evaluates under Track 1 (Internal Microservice Principal). Direct subscriber Bearer token requests targeting `GET /api/v1/venue/{id}` evaluate under Track 2 and require strict direct ownership verification (`venue.subscriber == CallerId`).

#### 5.3.2 PUT `/api/v1/venue/{id}`
1. The system MUST load the venue record and assert `venue.subscriber == CallerId`. If the venue does not exist, or if `venue.subscriber != CallerId`, the system MUST return `HTTP 404 Not Found` in accordance with the Section 4.3.2 uniform masking rule.
2. The system MUST enforce the venue metadata allowlist defined in Section 6.2.

#### 5.3.3 POST `/api/v1/venue` and DELETE `/api/v1/venue/{id}`
1. Venue creation and venue deletion MUST NOT be permitted as subscriber capabilities.
2. The system MUST reject any subscriber request attempting `POST /api/v1/venue` or `DELETE /api/v1/venue/{id}` fail-closed with `HTTP 403 ACCESS_DENIED`.

---

### 5.4 Configuration (Administrative Boundary)

1. **Administrative Exclusivity**: Standalone configuration endpoints under `/api/v1/configuration*` are purely administrative APIs.
2. **Fail-Closed Denial**: Any direct or indirect invocation of `GET`, `POST`, `PUT`, or `DELETE` on `/api/v1/configuration*` using a subscriber Bearer token MUST be rejected fail-closed with `HTTP 403 ACCESS_DENIED`.
3. **Encapsulation Independence**: Internal configuration records generated or modified during authorized `SubscriberDevice` operations MUST NOT grant subscriber access to the standalone Configuration API.

---

## 6. Mutation Requirements

Authorization to access or own a resource DOES NOT grant unrestricted permission to modify unauthorized properties. The system MUST enforce the following mutation boundaries:

### 6.1 Subscriber Profile Mutations

For `PUT /api/v1/subscriber` (Subscriber Self-Service):
1. **Permitted Operation**: `operation=updateMac` is the ONLY permitted subscriber operation.
2. **Permitted Fields**: The request payload MUST contain only the new hardware `macAddress`.
3. **Prohibited Operations**: Any subscriber request attempting other operations—including `emailVerified`, operator notes, billing tags, or status updates—MUST be rejected with `HTTP 403 ACCESS_DENIED`. (As defined in Section 5.1.3, `operation=emailVerified` is an internal microservice operation executed under Track 1 inter-service authentication).

### 6.2 Venue Metadata Mutations

For `PUT /api/v1/venue/{id}`:
1. **Permitted Venue Fields**:
   - The authorization engine MUST restrict subscriber venue updates strictly to descriptive user metadata:
     - `name`: User-facing venue name (persisted to `info.name`).
     - `description`: User-facing venue description (persisted to `info.description`).
     - `location`: Linked location UUID string (referencing the subscriber's location record in `LocationDB`).
2. **Linked Location Ownership Verification**:
   - If the update payload includes a `location` UUID, the system MUST verify that the referenced location record exists in `LocationDB` and directly belongs to the caller (`locationRecord.subscriber == CallerId`).
   - If the referenced location does not exist, or if `locationRecord.subscriber != CallerId`, the system MUST reject the request with `HTTP 404 Not Found` in accordance with uniform existence masking (Section 4.3.2), preventing cross-subscriber resource association and IDOR probing.
3. **Field Enforcement**:
   - Any attempt by a subscriber to modify administrative or infrastructure venue properties (including `subscriber`, `entity`, `parent`, `children`, `devices`, `topology`, `managementPolicy`, `deviceRules`, `contacts`, `configurations`, `deviceConfiguration`) is unauthorized and MUST be rejected fail-closed with `HTTP 403 ACCESS_DENIED`.

---

## 7. Registration Requirements

The system MUST maintain a strict architectural distinction between the unauthenticated user registration flow (Flow A) and authenticated subscriber self-service (Flow B).

```text
Flow A: Unauthenticated User Registration (Internal Service Flow)
=================================================================
Unauthenticated User
        ↓
    UserPortal
        ↓ Internal Service Authentication (X-API-KEY)
owprov POST /api/v1/subscriber
        ↓
Existing Signup Workflow (SignupDB, VenueDB, owsec /signup)


Flow B: Authenticated Subscriber Self-Care (Subscriber RBAC)
============================================================
Authenticated Subscriber
        ↓ Bearer Token (JWT from owsec)
UserInfo_.userinfo.id (CallerId)
        ↓
owprov RoleIsAuthorized() -> Dedicated Subscriber Authorization Pipeline
        ↓
Subscriber-Scoped Handler & Business Operation
```

1. **Flow A — Unauthenticated Registration Flow**: `POST /api/v1/subscriber` is an unauthenticated-user registration flow executed by UserPortal using trusted internal service authentication (`X-API-KEY` with `X-INTERNAL-NAME`).
2. **Outside Subscriber RBAC**: It is NOT a subscriber self-service capability and is outside the subscriber RBAC capability set entirely. It MUST NOT enter or be evaluated under the subscriber Bearer-token authorization branch.
3. **No Subscriber RBAC Requirements**: The system MUST NOT require subscriber identity, Bearer-token validation, `CallerId` ownership assertions, `subscriberId == CallerId` comparisons, or subscriber role lookups for Flow A.
4. **Internal Service Authentication Responsibility & Fail-Closed Isolation**: Direct external invocation without valid trusted internal service authentication MUST NOT be permitted. Track 1 Internal Microservice Principal classification at `RoleIsAuthorized()` requires the mandatory conjunction of:
   - Request arrival strictly on the internal router interface (`Internal_ == true`, bound to internal listening ports).
   - Valid inter-service credentials verified via `MicroServiceIsValidAPIKEY(*Request)`.
   - Verified `X-INTERNAL-NAME` header matching a registered ecosystem microservice (`owsec`, `UserPortal`, or `owgw`).
   Requests satisfying this three-element conjunction bypass subscriber RBAC checks to allow registration (`POST /api/v1/subscriber`) and email verification (`PUT /api/v1/subscriber?operation=emailVerified`) to proceed to existing service handlers. If an incoming internal request fails API key verification or supplies an unverified/mismatched service name, it MUST fail closed immediately (`HTTP 401 Unauthorized` / `HTTP 403 Forbidden`) and MUST NEVER fall through to subscriber (Track 2) or administrative (Track 3) authorization pipelines. Granular inter-service ACL hardening is deferred to Issue #90-Followup (Non-Goal 17).

---

## 8. Administrative RBAC Compatibility

1. **Zero Impact on Administrative Roles**: The implementation of subscriber authorization MUST NOT modify the behavior, semantics, or data structures of:
   - `FindAllUserRoles()`
   - Global `PolicyAllows()`
   - `RolesDB`
   - `PolicyDB`
2. **No Synthetic Role Records**: The system MUST NOT insert synthetic subscriber management roles into `RolesDB` or `PolicyDB`.
3. **Execution Precedence & Principal Separation**: The dedicated subscriber authorization branch MUST be evaluated in `RoleIsAuthorized()` BEFORE administrative RBAC checks, but MUST be gated strictly on subscriber principal classification (`UserInfo_.userinfo.userRole == SecurityObjects::USER_ROLE::SUBSCRIBER`). This prevents subscriber tokens from matching administrative bypass rules while ensuring administrative identities accessing subscriber endpoints are evaluated under administrative RBAC without being misclassified or restricted as subscribers.
4. **Preservation of Administrative Hierarchy**: Non-subscriber actors (`ROOT`, `Admin`, `CSR`, `Installer`, `NOC`, `Accounting`) MUST continue to be authorized through existing hierarchical `ManagementRole` and `PolicyDB` rules without alteration.

---

## 9. HTTP Error Requirements

The system MUST enforce an operational semantic separation between authorization failures (`HTTP 403 ACCESS_DENIED`) and request validation failures (`HTTP 400 Bad Request`).

### 9.1 Operational Error Boundary

#### HTTP 403 — Authorization Failure
> **Rule:** The request is structurally understood, but the caller is not permitted to perform the requested operation or modify unauthorized properties.

Enforced for:
- Identity mismatch: A client-supplied `subscriberId` or path identifier does not match the authenticated caller (e.g., `{ "subscriberId": "someone-else" }` or `DELETE /api/v1/subscriber/{foreign_id}` $\longrightarrow$ `HTTP 403 ACCESS_DENIED`).
- Unauthorized property modification: Attempts by a subscriber to mutate administrative, entity, or infrastructure properties outside permitted metadata.
- Unauthorized endpoints or operations: Invocations of administrative endpoints (e.g., `POST /api/v1/venue`, `DELETE /api/v1/venue/{id}`, or `PUT /api/v1/subscriberDevice/{id}`).
- Standalone configuration access: Invocations of `/api/v1/configuration*`.
- Unsupported subscriber operations: Profile mutations outside `operation=updateMac`.
- Syntactically recognized unauthorized paths (e.g., `POST /api/v1/subscriberDevice/123`).
- Prohibited query parameters: Use of `?email=...`, `?macAddress=...`, or `?listOnly=true` on profile GET.
- Missing, empty, or unresolvable subscriber identity context.

#### HTTP 404 — Not Found & Resource Existence Masking
> **Rule:** The target resource does not exist, or exists but belongs to another subscriber or entity.

Enforced for:
- Non-existent target resource identifiers: Requested device UUID, serial number, or venue ID does not exist in the database $\longrightarrow$ `HTTP 404 Not Found`.
- Uniform foreign resource masking: Eliminates the primary direct HTTP existence oracle for device serial numbers, device UUIDs, or venue IDs; any access to a foreign or unassigned resource via target resource endpoints (`/api/v1/subscriberDevice/{id}`, `/api/v1/venue/{id}`, or `?signupUUID=<id>`), or submitting a foreign location reference on `PUT /api/v1/venue/{id}`, returns `HTTP 404 Not Found`, identical to a non-existent resource. (Note: In contrast, parameterized `DELETE /api/v1/subscriber/{foreign_id}` returns `HTTP 403 ACCESS_DENIED` as an explicit identity-assertion violation rather than 404). The distinct diagnostic reason (foreign ownership vs. record not found) is recorded strictly in internal server logs for debugging and audit trails.

#### HTTP 400 — Request Validation Failure
> **Rule:** The operation is permitted for the caller, but the request representation itself is invalid, unsupported, or violates domain schema constraints.

Enforced for:
- Malformed request syntax or unparseable JSON payload $\longrightarrow$ `HTTP 400 Bad Request` (`InvalidJSONDocument`).
- Malformed path parameters or invalid structural representation.
- Invalid data types, formats, or values outside allowable ranges (e.g., invalid MAC format, invalid serial syntax).
- MAC collision on profile update: Attempting to assign an already-registered MAC address via `PUT /api/v1/subscriber?operation=updateMac` $\longrightarrow$ `HTTP 400 Bad Request` (`MACAddressExists` / `RecordNotUpdated`).
- Active claimed devices remaining during subscriber self-deletion (`StillInUse`).

---

## 10. Security Requirements

1. **Cross-Subscriber Isolation & Existence Masking**: The system MUST strictly enforce isolation between subscribers. A subscriber MUST NOT be able to view, claim, mutate, or delete resources belonging to any other subscriber. To eliminate the primary direct HTTP existence oracle for device serial numbers, device UUIDs, and venue IDs across the platform, endpoints targeting specific resources MUST return a uniform `HTTP 404 Not Found` for both non-existent and foreign-owned resources, while logging distinct diagnostic reasons internally. Indirect micro-architectural timing or latency side-channels are outside the scope of Issue #90.
2. **Server-Derived Ownership and Identity**: Ownership of newly claimed or created resources MUST be assigned server-side from `CallerId`. If a client supplies an optional `subscriberId`, it MUST match `CallerId` (`HTTP 403 ACCESS_DENIED` on mismatch); client-supplied ownership values have zero authority to establish or expand caller authorization scope.
3. **No Operator / Tenant Dependency**: The subscriber authorization path MUST NOT evaluate or depend upon `operatorId`, `CallerTenant`, or tenant equality checks.
4. **Strict Collection Scoping**: Device collection queries MUST be scoped strictly to `WHERE subscriberId = CallerId` with zero possibility of falling back to operator-wide visibility.
5. **Exact-Match Capabilities**: The system MUST enforce exact-match HTTP method capabilities with zero method aliasing.
6. **Administrative & Infrastructure Protection**: Administrative, management, entity, and infrastructure properties are protected against subscriber mutation, and any attempt to modify unauthorized properties MUST be rejected with `HTTP 403 ACCESS_DENIED`.
7. **Fail-Closed Default & Uniform 404 Masking**: Authorization MUST fail closed with `HTTP 403 ACCESS_DENIED` for any unauthorized capability, unsupported subscriber operation, unauthorized path variant, identity mismatch (`subscriberId != CallerId`), or missing/incomplete identity context. Whenever a target resource is requested (via URI path or resource identifier) and is either non-existent or foreign-owned, the system MUST uniformly return `HTTP 404 Not Found` to eliminate existence oracles, recording the distinct diagnostic reason internally in server logs.
8. **Subscriber-Safe Response Projection**: The system MUST project subscriber profile responses to strip internal operator notes, classification tags, and internal error traces.
9. **Preservation of Administrative RBAC**: Administrative roles, policies, and privileges MUST remain unaffected by subscriber authorization logic.
10. **Business Logic Separation**: Authorization logic determines whether an operation is permitted; existing business logic handlers remain responsible for operational integrity checks (e.g., serial syntax, MAC uniqueness).
11. **Concurrency and TOCTOU Scope Boundary**: Authorization and direct resource ownership invariants are asserted from loaded records at request time. Advanced database-level concurrency serialization (e.g., distributed locking, multi-statement row locks, atomic conditional CAS queries) are intentionally deferred to a follow-up issue ("Issue #90-Followup: Concurrency and TOCTOU DB-level serialization").

---

## 11. Acceptance Criteria

### 11.1 Subscriber Profile
- **AC-SUB-1**: An authenticated subscriber calling bare `GET /api/v1/subscriber` successfully retrieves their own profile (`userId == CallerId`).
- **AC-SUB-2**: The response payload for `GET /api/v1/subscriber` contains only subscriber-safe fields; internal operator metadata (`notes`, `tags`, internal error traces) is redacted.
- **AC-SUB-3**: A subscriber calling `GET /api/v1/subscriber` with query parameter `?email=...`, `?macAddress=...`, or `?listOnly=true` receives `HTTP 403 ACCESS_DENIED`.
- **AC-SUB-4**: A subscriber calling `GET /api/v1/subscriber?signupUUID=<own_id>` successfully retrieves their profile; calling with `?signupUUID=<foreign_id>` receives `HTTP 404 Not Found` (uniform existence masking).
- **AC-SUB-5**: An authenticated subscriber calling `PUT /api/v1/subscriber?operation=updateMac` with a valid, unassigned MAC successfully updates their profile.
- **AC-SUB-6**: An authenticated subscriber calling `PUT /api/v1/subscriber` with an operation other than `updateMac` (or an empty operation) receives `HTTP 403 ACCESS_DENIED`. Calling `PUT /api/v1/subscriber?operation=updateMac` with an already-assigned MAC address receives `HTTP 400 Bad Request` (`MACAddressExists` / `RecordNotUpdated`).
- **AC-SUB-7**: An authenticated subscriber calling bare `DELETE /api/v1/subscriber` with zero active claimed devices across all owned venues successfully deletes their profile.
- **AC-SUB-8**: An authenticated subscriber calling bare `DELETE /api/v1/subscriber` with active claimed devices remaining in any owned venue receives `HTTP 400 Bad Request` (`StillInUse`).
- **AC-SUB-9**: An authenticated subscriber calling parameterized `DELETE /api/v1/subscriber/{id}` where `{id} == CallerId` with zero active claimed devices across all owned venues successfully deletes their profile and associated empty venues; if active claimed devices remain associated with the subscriber or any owned venue, receives `HTTP 400 Bad Request` (`StillInUse`); calling where `{id} != CallerId` receives `HTTP 403 ACCESS_DENIED`.

### 11.2 Subscriber Devices
- **AC-DEV-1**: An authenticated subscriber calling `GET /api/v1/subscriberDevice` receives only devices where `subscriberId == CallerId`.
- **AC-DEV-2**: An authenticated subscriber calling `GET /api/v1/subscriberDevice/{id}` (via UUID or hardware serial number) for an owned device receives the device payload; calling for a foreign device receives `HTTP 404 Not Found` (uniform existence masking).
- **AC-DEV-3**: An authenticated subscriber calling `POST /api/v1/subscriberDevice/0` with eligible claim data successfully claims the device, with ownership assigned to `CallerId`.
- **AC-DEV-4**: An authenticated subscriber calling `POST /api/v1/subscriberDevice/{non_zero_id}` receives `HTTP 403 ACCESS_DENIED`.
- **AC-DEV-5**: A client-supplied `subscriberId` in the body of `POST /api/v1/subscriberDevice/0` that does not match `CallerId` is rejected with `HTTP 403 ACCESS_DENIED`; if absent or matching `CallerId`, the device is claimed with `newDevice.subscriberId = CallerId`.
- **AC-DEV-6**: Any authenticated subscriber calling `PUT /api/v1/subscriberDevice/{id}` receives `HTTP 403 ACCESS_DENIED` (administrative-only endpoint; subscriber Wi-Fi configuration mutations route via UserPortal directly to `owgw`).
- **AC-DEV-7**: An authenticated subscriber calling `DELETE /api/v1/subscriberDevice/{id}` (via UUID or hardware serial number) on an owned device successfully unclaims the device; calling on a foreign or non-existent device receives `HTTP 404 Not Found` (uniform existence masking).
- **AC-DEV-8**: On device claim via `POST /api/v1/subscriberDevice/0`, client input supplying an existing `deviceConfiguration` UUID or referencing an administrative configuration template is ignored or rejected; `owprov` automatically generates a dedicated subscriber configuration record (`cfg.subscriber = CallerId`, `cfg.subscriberOnly = true`).

### 11.3 Venues
- **AC-VEN-1**: An authenticated subscriber calling `GET /api/v1/venue/{id}` on an owned venue (`venue.subscriber == CallerId`) receives the venue record; calling on a foreign venue receives `HTTP 404 Not Found` (uniform existence masking).
- **AC-VEN-2**: An authenticated subscriber calling `PUT /api/v1/venue/{id}` on an owned venue with permitted fields (`name`, `description`, `location`) successfully updates the venue.
- **AC-VEN-3**: An authenticated subscriber calling `PUT /api/v1/venue/{id}` referencing a foreign or non-existent location UUID receives `HTTP 404 Not Found` (uniform existence masking).
- **AC-VEN-4**: An authenticated subscriber calling `PUT /api/v1/venue/{id}` attempting to modify `subscriber`, `entity`, or administrative settings receives `HTTP 403 ACCESS_DENIED`.
- **AC-VEN-5**: An authenticated subscriber calling `POST /api/v1/venue` receives `HTTP 403 ACCESS_DENIED`.
- **AC-VEN-6**: An authenticated subscriber calling `DELETE /api/v1/venue/{id}` receives `HTTP 403 ACCESS_DENIED`.

### 11.4 Standalone Configuration Boundary
- **AC-CFG-1**: Any direct subscriber request targeting `/api/v1/configuration*` (`GET`, `POST`, `PUT`, `DELETE`) receives `HTTP 403 ACCESS_DENIED`.

### 11.5 Registration Flow
- **AC-REG-1**: A direct external request to `POST /api/v1/subscriber` without valid internal service credentials (`X-API-KEY`) is rejected by the authentication/authorization layer.
- **AC-REG-2**: An internal service call from UserPortal with valid `X-API-KEY` to `POST /api/v1/subscriber` succeeds and initiates the signup workflow.
- **AC-REG-3**: An incoming request on the internal router interface with an invalid API key, missing internal name, or mismatched service identity receives `HTTP 401 Unauthorized` / `HTTP 403 Forbidden` and fails closed immediately without falling through to Track 2 or Track 3.

### 11.6 Administrative RBAC Preservation
- **AC-ADM-1**: Administrative requests (`ROOT`, `Admin`, `CSR`, `Installer`) continue to evaluate through hierarchical `ManagementRole` and `PolicyDB` rules without deviation or regression.
- **AC-ADM-2**: An authenticated administrative user accessing subscriber-capable endpoints (such as `GET /api/v1/subscriberDevice` or `DELETE /api/v1/subscriber/{id}`) is not classified as a subscriber and evaluates exclusively through administrative RBAC.

---

## 12. Testing Requirements

Test suites MUST provide comprehensive verification across all authorization boundaries without redundant test cases.

> [!IMPORTANT]
> **Test Suite Prioritization:** Automated test execution for the upcoming implementation PR MUST prioritize the **Tier 1 Active Production Path** (`GET /api/v1/subscriberDevice`, `POST /api/v1/subscriberDevice/0`, `DELETE /api/v1/subscriberDevice/{mac}`, `GET /api/v1/subscriberDevice/{mac}`, and `DELETE /api/v1/subscriber/{id}`) to guarantee that live Mobile App operations through UserPortal succeed with zero regressions. End-to-end verification of Tier 2 non-path endpoints is secondary.

```text
+-------------------------------------------------------------------------+
| Test Suite Structure & Boundary Verification Matrix                     |
+-------------------------------------------------------------------------+
| 1. Subscriber Identity & Context:                                       |
|    - Verify valid Bearer JWT resolves to UserInfo_.userinfo.id.         |
|    - Verify missing or unresolvable identity fails closed with 403.     |
|    - Verify client-supplied subscriberId must match CallerId (403 on    |
|      mismatch); if omitted, CallerId is used.                           |
|    - Verify CallerId/userRole cannot be overridden by headers or body.  |
|                                                                         |
| 2. Exact-Match Capability Enforcement:                                  |
|    - Verify zero method aliasing (PUT does not authorize POST/DELETE).  |
|    - Verify unauthorized resources (configuration, etc.) fail with 403. |
|    - Verify direct PUT /subscriberDevice/{id} returns 403 for subs.     |
|                                                                         |
| 3. Collection Scoping:                                                  |
|    - Verify GET /subscriberDevice filters strictly to caller's devices. |
|    - Verify zero operator-wide fallback on empty or foreign filters.    |
|    - Verify existing pagination limit/offset preserved within caller.   |
|                                                                         |
| 4. Resource Ownership & Isolation:                                      |
|    - Verify owned device/venue GET/DELETE succeeds.                     |
|    - Verify foreign resource access returns 404 (existence masking).    |
|    - Verify PUT /venue/{id} referencing foreign location returns 404.   |
|    - Verify distinct internal diagnostic reason logged for audit.       |
|                                                                         |
| 5. Sentinel Creation Enforcement:                                       |
|    - Verify POST /subscriberDevice/0 claims device for caller.          |
|    - Verify non-sentinel paths (/123, /new) fail closed with 403.       |
|    - Verify claimed config has cfg.subscriber == CallerId and           |
|      cfg.subscriberOnly == true (never references admin config).        |
|    - Verify client-supplied deviceConfiguration UUID is rejected/ignored|
|                                                                         |
| 6. Profile & Venue Boundaries:                                          |
|    - Verify GET /subscriber response redaction (notes/tags stripped).   |
|    - Verify prohibited query parameters on GET /subscriber return 403.  |
|    - Verify PUT /subscriber permits only operation=updateMac for subs.  |
|    - Verify duplicate MAC on updateMac returns 400 Bad Request.         |
|    - Verify bare DELETE /subscriber enforces multi-venue device check.  |
|    - Verify parameterized DELETE /subscriber/{id} succeeds when        |
|      {id} == CallerId (zero devices across all venues), returns 400 when|
|      devices remain in any venue, and returns 403 when {id} != CallerId.|
|    - Verify venue POST and DELETE return 403.                           |
|    - Verify venue PUT restricts modification to metadata (name, desc).  |
|                                                                         |
| 7. Registration vs. Self-Service Separation:                            |
|    - Verify POST /subscriber requires internal X-API-KEY/X-INTERNAL-NAME|
|    - Verify internal calls bypass subscriber RBAC via Track 1.          |
|    - Verify invalid API key / internal name fails closed (401/403) with |
|      zero fall-through to Track 2 (subscriber) or Track 3 (admin).      |
|    - Verify internal Track 1 calls preserve existing handler routing.   |
|    - Verify PUT /subscriber?operation=emailVerified allowed for Track 1 |
|      and rejected with 403 for external subscriber Bearer tokens.       |
|                                                                         |
| 8. Administrative RBAC Regression:                                      |
|    - Verify ROOT, Admin, and CSR roles retain unchanged authorization.  |
|    - Verify FindAllUserRoles() and PolicyAllows() behavior is preserved.|
+-------------------------------------------------------------------------+
```

---

## 13. Non-Functional Constraints

1. **Evaluation Efficiency**: Subscriber capability and path authorization checks MUST be evaluated before executing administrative role resolution or expensive downstream queries, rejecting unauthorized subscriber requests as early as possible.
2. **Policy Tamper Resistance**: Subscriber authorization policy MUST be defined statically in code rather than loaded from runtime-configurable database records, preventing unauthorized modification via database state or misconfigured administrative policies.
3. **Cohesive Scope Delivery**: The complete subscriber authorization pipeline across all three active resources (`Subscriber`, `SubscriberDevice`, `Venue`) MUST function end-to-end to satisfy Issue #90.
4. **Administrative Query Avoidance**: Evaluating the subscriber authorization branch prior to administrative role resolution MUST avoid executing unnecessary administrative `ManagementRole` database queries for subscriber requests.
5. **Concurrency / TOCTOU**: Race-condition and TOCTOU hardening are intentionally deferred to a follow-up issue. This specification defines the authorization and ownership invariants only and does not require transaction/concurrency redesign as part of Issue #90.
6. **Query Performance Verification**: The implementation MUST verify that the subscriber-device collection query uses an appropriate existing index on `subscriberId`, or an appropriate composite index based on the actual filtering/sorting pattern. Issue #90 does not require creation of a new index or database migration unless verification demonstrates that the existing schema cannot support the required query efficiently.

---

## 14. Non-Goals

The following items are explicit non-goals for Issue #90:
1. **No Operator-Based Subscriber Authorization**: The system will not evaluate operator identity or operator hierarchies for subscriber self-service.
2. **No Tenant / Multi-Tenant Isolation Model**: Multi-tenant isolation concepts (`CallerTenant`, tenant equality) are completely removed from subscriber authorization.
3. **No Synthetic Subscriber Management Roles**: Subscribers will not receive entries in `RolesDB` or `PolicyDB`.
4. **No Administrative RBAC Alteration**: Semantics of `FindAllUserRoles()`, `PolicyAllows()`, and `ManagementRole` will not be altered.
5. **No Standalone Configuration Access**: Subscribers will not be granted access to `/api/v1/configuration*`.
6. **No ORM or Database Framework Overhaul**: Broad ORM refactoring and database schema changes are deferred.
7. **No Transaction or Concurrency Overhaul**: Platform-wide transaction redesign, distributed locking, atomic conditional CAS DB queries, and TOCTOU race-condition hardening are deferred to a follow-up issue ("Issue #90-Followup: Concurrency and TOCTOU DB-level serialization").
8. **No Hardware Proof-of-Possession**: Physical device entitlement and challenge-response protocols (e.g. physical QR code verification, optical barcode scanning, hardware push-button pairing challenges) are upstream physical provisioning concerns outside the scope of Issue #90.
9. **No Business Workflow Redesign**: Existing handlers retain responsibility for their internal business validations.
10. **No UserPortal Architectural Flow Redesign**: This PR will not alter the existing UserPortal (`owsub`) call chain. Specifically, the existing flow where UserPortal handles `POST /api/v1/action?action=configure` by dispatching directly to `owgw` will not be altered or redirected in this PR.
11. **No Subscriber Device Mutation on `owprov`**: Direct device or configuration mutation on `PUT /api/v1/subscriberDevice/{id}` is not permitted for subscribers (denied fail-closed with `HTTP 403 ACCESS_DENIED`). In production, subscribers configure devices via UserPortal directly to `owgw`.
12. **No Upstream Contract Modifications**: Existing Mobile App client contracts with UserPortal remain unmodified.
13. **No Configuration Mutation via `owprov` for Subscribers**: Direct uCentral device configuration creation, parsing, or mutation on `owprov` is not a subscriber capability. All subscriber Wi-Fi and device configuration changes route through UserPortal directly to `owgw` (`POST /api/v1/action?action=configure`).
14. **No Device Payload Redaction Overhaul**: This PR will not introduce response filtering or property redaction on `GET /api/v1/subscriberDevice/{id}` or `GET /api/v1/subscriberDevice`. Subscribers receive the standard `SubscriberDevice` projection; sensitive operator-only internal metadata does not exist on this entity.
15. **No Custom Query Parameter Engine**: This PR will not implement duplicate query parameter rejection or custom URI parsing beyond standard POCO `URI::QueryParameters` processing.
16. **No Schema Removal of `operatorId`**: While `operatorId` has zero authority in subscriber authorization, this PR will not remove or alter `operatorId` columns in database tables or the subscriber profile representation.
17. **No Internal Microservice Permission Matrix Overhaul**: Existing internal microservice authorization and routing behavior for requests on `Internal_ == true` are preserved without alteration; granular microservice permission hardening is deferred to a follow-up issue.

---

## 15. Traceability

The following matrix maps requirement areas to specification sections, acceptance criteria, and testing suites.

| Requirement Area | Specification Section | Acceptance Criteria | Testing Requirement |
|---|---|---|---|
| **Subscriber Identity** | Section 4.1, 4.6 | AC-SUB-1, AC-DEV-5 | Identity & Context Suite |
| **Exact-Match Capabilities** | Section 4.2 | AC-CFG-1, AC-DEV-6, AC-VEN-5, AC-VEN-6 | Capability Enforcement Suite |
| **Collection Scoping** | Section 4.4 | AC-DEV-1 | Collection Scoping Suite |
| **Resource Ownership** | Section 4.3 | AC-DEV-2, AC-DEV-7, AC-VEN-1 | Ownership & Isolation Suite |
| **Creation Sentinel Path** | Section 5.2.3 | AC-DEV-3, AC-DEV-4 | Sentinel Creation Suite |
| **Device Mutation Denial** | Section 5.2.4 | AC-DEV-6 | Capability Enforcement Suite |
| **Profile Safe Projection** | Section 5.1.2 | AC-SUB-2, AC-SUB-3, AC-SUB-4 | Profile & Venue Suite |
| **Profile Operations** | Section 5.1.3 | AC-SUB-5, AC-SUB-6 | Profile & Venue Suite |
| **Subscriber Self-Deletion** | Section 5.1.4, 5.1.5 | AC-SUB-7, AC-SUB-8, AC-SUB-9 | Profile & Venue Suite |
| **Venue Authorization** | Section 5.3, 6.2 | AC-VEN-1, AC-VEN-2, AC-VEN-3, AC-VEN-4 | Profile & Venue Suite |
| **Configuration API Boundary** | Section 5.4 | AC-CFG-1 | Capability Enforcement Suite |
| **Registration Flow (Flow A)** | Section 7 | AC-REG-1, AC-REG-2 | Registration Separation Suite |
| **Administrative Compatibility**| Section 8 | AC-ADM-1 | Administrative Regression Suite |
| **Error Differentiation (403/404/400)**| Section 9 | AC-DEV-2, AC-DEV-6, AC-SUB-8, AC-VEN-3 | All Suites |
| **Security Requirements** | Section 10 | All ACs | All Suites |
