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
2. **`SubscriberDevice`**: Collection retrieval, individual device inspection, device claiming on sentinel path `/0`, Wi-Fi configuration mutations, and device unclaiming/deletion.
3. **`Venue`**: Venue inspection and descriptive metadata updates.

In-scope functional and security requirements comprise:
- Deriving authoritative subscriber identity strictly from validated Bearer tokens (`UserInfo_.userinfo.id`).
- Evaluating a dedicated subscriber authorization branch at `RoleIsAuthorized()` prior to administrative role lookups.
- Enforcing static, in-code, exact-match subscriber capabilities with zero method aliasing.
- Scoping collection queries strictly to the authenticated subscriber (`WHERE subscriberId = CallerId`) with no operator-wide fallback.
- Verifying direct resource ownership (`TargetResource.subscriberId == CallerId` or `venue.subscriber == CallerId`) whenever an endpoint targets a specific resource identifier.
- Enforcing server-derived ownership on device creation, requiring the sentinel path `/api/v1/subscriberDevice/0`.
- Enforcing strict top-level property allowlists and deep recursive uCentral Wi-Fi configuration allowlists.
- Protecting internal configuration records, ensuring configurations mutated by subscribers are owned by the caller and cannot modify administrative templates or management networks.
- Enforcing the architectural separation between unauthenticated internal registration (Flow A) and authenticated subscriber self-service (Flow B).
- Enforcing semantic differentiation between authorization rejections (`HTTP 403 ACCESS_DENIED`) and request content/schema violations (`HTTP 400 Bad Request`).
- Enforcing fail-closed behavior across all endpoints, paths, operations, query parameters, and ambiguous contexts.
- Enforcing direct resource ownership invariants across all subscriber operations.
- Preserving administrative RBAC behavior (`FindAllUserRoles()`, `PolicyAllows()`, `ManagementRole`, `PolicyDB`) without regression.

### 3.2 Out of Scope

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
Mutation & Field Allowlist Authorization
        |
        v
Existing Business Operation Execution
```

Authorization MUST fail closed at the earliest failing stage. A request MUST NOT proceed to execution unless every applicable stage succeeds.

### 4.1 Subscriber Identity

1. **Authoritative Source**: The system MUST derive the authenticated subscriber identity exclusively from the validated Bearer token payload via `UserInfo_.userinfo.id` (designated as `CallerId`).
2. **Context Immutability**: `CallerId` MUST remain immutable for the entire lifecycle of the HTTP request.
3. **No Synthetic Role Injection**: The system MUST NOT synthesize `ManagementRole` objects or insert artificial subscriber roles into `FindAllUserRoles()`.
4. **Mandatory Identity Context**: If `UserInfo_.userinfo.id` is null, empty, or unresolvable, the system MUST terminate authorization immediately and return `HTTP 403 ACCESS_DENIED`.
5. **No Ambient Identity**: An unauthenticated or anonymous request MUST NOT be evaluated under the subscriber authorization pipeline.
6. **Principal Classification Gate**: Incoming requests evaluated at `RoleIsAuthorized()` MUST be classified into one of four distinct principal tracks:
   - **Track 1 (Internal Microservice Principal)**: When a request arrives on the internal router (`Internal_ == true`) accompanied by trusted inter-service credentials (valid `X-API-KEY` with verified `X-INTERNAL-NAME` header from registered ecosystem services such as `owsec`, `UserPortal`, or `owgw`), the request is recognized as an internal microservice principal. Internal service calls MUST bypass the subscriber authorization pipeline and proceed directly to administrative execution.
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
| **`subscriberDevice`** | **Allowed** | **Allowed** | **Allowed** | **Allowed** | Full device self-care lifecycle (`POST` constrained to sentinel `/0`). |
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
2. **Single-Pass Loading and In-Memory Ownership Assertion**:
   - The system MUST load the target resource by its identifier (`:resourceId`, via UUID or hardware serial number for devices) in a single pass.
   - Ownership and status differentiation MUST be evaluated in memory from the resolved record:
     a) If the target resource does not exist in the database, the system MUST return `HTTP 404 Not Found`.
     b) If the target resource exists but ownership fails (`Device.subscriberId != CallerId` or `Venue.subscriber != CallerId`), the system MUST reject the request fail-closed with `HTTP 403 ACCESS_DENIED`.
   - For authorized requests (`subscriberId == CallerId`), the pre-loaded entity MUST be passed/memoized within the request context so downstream business handlers do not reload the same resource from the database.
3. **Point-of-Mutation Ownership Invariant**:
   - For mutations (`PUT`, `DELETE`), ownership MUST remain asserted at the point of database modification (e.g., atomic conditional update/delete `WHERE id = :resourceId AND subscriberId = :CallerId`) to prevent execution against unassigned or concurrently reassigned resources.
4. Resource ownership verification MUST be evaluated as an individual direct resource ownership check (`resource.subscriberId == CallerId` or `venue.subscriber == CallerId`); it MUST NOT be delegated to tenant or operator checks.

### 4.4 Collection Scoping and Bounded Retrieval

1. **Unconditional Query Constraint**: For subscriber collection requests (`GET /api/v1/subscriberDevice`), the system MUST constrain the underlying database query strictly to:
   ```sql
   WHERE subscriberId = :CallerId
   ```
2. **Zero Operator Fallback**: If the query returns zero records, or if request query parameters are omitted, the query MUST NOT broaden or fall back to operator-wide, entity-wide, or venue-wide visibility.
3. **Consistent Scoping and Parameter Tolerance**:
   - **Client-Supplied `subscriberId`**: If `subscriberId` is omitted, the collection query MUST automatically inject `WHERE subscriberId = :CallerId`. If `subscriberId` is explicitly supplied in the query string, the consistent `subscriberId` rule (Section 4.6.3) MUST be enforced: if `subscriberId == CallerId`, the query proceeds; if `subscriberId != CallerId`, the request MUST be rejected immediately with `HTTP 403 ACCESS_DENIED`. The server MUST NOT silently ignore or rewrite a mismatched `subscriberId`.
   - **Upstream Parameter Tolerance**: Context parameters routinely supplied by upstream clients (such as `operatorId` passed by UserPortal) MUST be tolerated and safely ignored without expanding visibility or returning an error, as subscribers possess zero operator-level visibility.
4. **Bounded Result Sizes**: The collection API MUST enforce bounded result sets to prevent unbounded database reads and excessive response serialization. If the client omits pagination parameters, a safe default limit (e.g., 20 or 50) MUST be applied, and client-requested limits MUST be clamped to a platform maximum bound (e.g., 100).
5. **Scoping Invariant Across Pagination**: Pagination (`limit`, `offset`), filtering, and sorting parameters MUST operate strictly within the `subscriberId = :CallerId` predicate. Client parameters MUST NOT be permitted to loosen or bypass the subscriber ownership boundary.

### 4.5 Fail-Closed Authorization

1. **Evaluation Order**: The subscriber authorization pipeline MUST be evaluated in `RESTAPIHandler::RoleIsAuthorized()` BEFORE `FindAllUserRoles()`, BEFORE administrative `ManagementRole` database queries, and BEFORE staff/operator global GET shortcuts, conditioned strictly upon subscriber principal classification (`userRole == SUBSCRIBER`).
2. **Authorization Rejection Precedence**: If a subscriber request fails authorization conditions—including capability checks, unauthorized method/path operations, missing or unresolvable identity context, foreign resource ownership, or attempts to modify administrative fields—the request MUST fail closed immediately with `HTTP 403 ACCESS_DENIED`.
3. **Invalid Request and Schema Precedence**: If an authorized subscriber request contains malformed request syntax, unknown top-level mutation fields, unknown nested configuration keys, or unsupported nested configuration structures, the request MUST fail closed with `HTTP 400 Bad Request` in accordance with Section 9.
4. **No Cross-Pipeline Fallthrough**: A rejected or unmatched subscriber request MUST NOT fall through to administrative RBAC evaluation. Conversely, administrative principals accessing subscriber-capable endpoints (e.g., `GET /api/v1/subscriberDevice` or `DELETE /api/v1/subscriber/{id}`) MUST NOT enter the subscriber authorization pipeline and MUST evaluate strictly under administrative RBAC.
5. **Path and Operation Boundary**:
   - When a requested path or operation is syntactically recognized but represents an action or path variant the subscriber is not permitted to invoke (such as `DELETE /api/v1/subscriber/{foreign_id}`, `POST /api/v1/venue`, or `POST /api/v1/subscriberDevice/{non-zero-id}`), authorization MUST fail closed with `HTTP 403 ACCESS_DENIED`.
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
6. **Client Parameter Tolerance**: Upstream consumer gateways and portals (such as UserPortal) routinely submit context parameters such as `operatorId` in collection queries and creation payloads (`POST /api/v1/subscriberDevice/0`). The subscriber authorization layer MUST tolerate and safely ignore client-supplied `operatorId` without failing or rejecting the request, while ensuring it confers zero authorization authority.

---

## 5. Endpoint Requirements

The system MUST enforce subscriber authorization across all endpoints in accordance with the specifications below.

| Method | Endpoint | Subscriber Access | Required Scope & Authorization Rule |
|---|---|:---:|---|
| `POST` | `/api/v1/subscriber` | **N/A (Flow A)** | Unauthenticated registration flow via internal service authentication (`X-API-KEY`); outside subscriber RBAC capability set. |
| `GET` | `/api/v1/subscriber` | **Allow** | Scoped strictly to `CallerId`; prohibits `email`, `macAddress`, `listOnly`; subscriber-safe projection. |
| `PUT` | `/api/v1/subscriber` | **Allow** | Scoped strictly to `CallerId`; permitted operation `updateMac` only; verifies MAC availability. |
| `DELETE` | `/api/v1/subscriber` | **Allow** | Bare path; scoped to `CallerId`; requires active claimed device count == 0. |
| `DELETE` | `/api/v1/subscriber/{id}` | **Conditional** | Permitted for subscriber self-deletion if `{id} == CallerId` (active device count == 0); `{id} != CallerId` rejected with `403`. Administrative principals evaluate via Admin RBAC. |
| `GET` | `/api/v1/subscriberDevice` | **Allow** | Collection scoped strictly to `WHERE subscriberId = CallerId`; no operator fallback. |
| `GET` | `/api/v1/subscriberDevice/{id}` | **Allow** | Verifies target device ownership: `device.subscriberId == CallerId`. |
| `POST` | `/api/v1/subscriberDevice/0` | **Allow** | Sentinel path `/0` only; forces `newDevice.subscriberId = CallerId`; verifies claim eligibility. |
| `PUT` | `/api/v1/subscriberDevice/{id}` | **Allow** | Verifies device ownership + top-level allowlist + nested Wi-Fi allowlist. |
| `DELETE` | `/api/v1/subscriberDevice/{id}` | **Allow** | Verifies device ownership (`device.subscriberId == CallerId`) + executes unclaim workflow. |
| `GET` | `/api/v1/venue/{id}` | **Allow** | Verifies target venue ownership: `venue.subscriber == CallerId`. |
| `PUT` | `/api/v1/venue/{id}` | **Allow** | Verifies venue ownership (`venue.subscriber == CallerId`) + permitted metadata allowlist. |
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
   - If the record does not exist, the system MUST return `HTTP 404 Not Found`.
   - The system MUST assert that `SE.userId == CallerId`. If `SE.userId != CallerId`, the system MUST reject the request fail-closed with `HTTP 403 ACCESS_DENIED`.
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
   - The system MUST verify that the normalized MAC address is unassigned across `SubscriberDeviceDB`, `InventoryDB`, and `SignupDB`. If the address is assigned, malformed, or invalid, the request MUST be rejected.
   - Uniqueness checks SHOULD evaluate indexed tables first (short-circuiting immediately upon collision) to minimize multi-table latency.
   - Uniqueness MUST be preserved against concurrent claim requests; conflicting assignments MUST fail closed.

#### 5.1.4 DELETE `/api/v1/subscriber` (Bare Path)
1. The system MUST authorize self-deletion on the bare URI path `/api/v1/subscriber` without path parameters.
2. The deletion operation MUST target `CallerId` directly.
3. **Active Device Check**: The system MUST query `InventoryDB` and `SubscriberDeviceDB` for active claimed devices associated with `CallerId`.
4. **Conditional Deletion**:
   - If active claimed devices remain associated with the subscriber, the system MUST reject the request with `HTTP 400 Bad Request` (`StillInUse`).
   - If active claimed device count is zero, the system MUST execute subscriber self-deletion.

#### 5.1.5 DELETE `/api/v1/subscriber/{id}` (Parameterized Path)
1. When invoked by an authenticated subscriber principal, the system MUST enforce the consistent identity rule:
   - If `{id} == CallerId`: The request is authorized as valid self-deletion and MUST enforce the Section 5.1.4 active claimed device constraint (rejecting with `HTTP 400 Bad Request` if active claimed devices remain).
   - If `{id} != CallerId`: The request MUST be rejected fail-closed with `HTTP 403 ACCESS_DENIED`.
2. When invoked by an administrative principal (`ROOT`, `Admin`, `CSR`), the endpoint MUST evaluate strictly through administrative RBAC, authorizing deletion of arbitrary subscriber records according to administrative policy.

---

### 5.2 SubscriberDevice

#### 5.2.1 GET `/api/v1/subscriberDevice` (Collection)
1. The system MUST authorize authenticated subscribers to retrieve their claimed devices.
2. The database query MUST be constrained unconditionally to `WHERE subscriberId = CallerId`.
3. The response MUST contain only devices belonging to `CallerId`.
4. The system MUST NOT fall back to operator-wide visibility under any circumstances.

#### 5.2.2 GET `/api/v1/subscriberDevice/{id}` (Individual)
1. The system MUST load the target device from `SubscriberDeviceDB` using `{id}` (resolving by internal UUID or hardware serial number).
2. If the device does not exist, the system MUST return `HTTP 404 Not Found`.
3. The system MUST verify that `device.subscriberId == CallerId`. If the device belongs to another subscriber or has no assigned subscriber, the system MUST return `HTTP 403 ACCESS_DENIED`.

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
4. **Device Claim Eligibility**: The system MUST enforce existing claim validations:
   - Serial number MUST NOT already exist in `SubscriberDeviceDB`.
   - Serial number MUST satisfy syntax validation (`ValidSerialNumber`).
   - `deviceGroup` MUST be specified and MUST be `"ap"` or `"olg"`.
   - A subscriber MUST NOT own more than one active OLG gateway.
5. **Configuration Record Initialization**: Any configuration record created for a claimed subscriber device in `ConfigurationDB` MUST explicitly set `cfg.subscriber = CallerId` and `cfg.subscriberOnly = true`. Subscriber devices MUST NOT reference unowned or shared administrative configurations.

#### 5.2.4 PUT `/api/v1/subscriberDevice/{id}` (Mutation)
1. The system MUST load the target device (resolving by UUID or hardware serial number) and verify device ownership: `device.subscriberId == CallerId`. Ownership mismatch MUST return `HTTP 403 ACCESS_DENIED`.
2. Device ownership alone DOES NOT authorize arbitrary modification. The request MUST satisfy the pure positive allowlist defined in Section 6.2 and Section 6.3.
3. **Configuration Ownership & Lazy Adoption Rule**:
   - Subscriber devices MUST reference subscriber-specific configuration records (`cfg`). Shared administrative templates MUST NOT be associated with subscriber devices.
   - When an authenticated subscriber updates an owned device (`device.subscriberId == CallerId`), configuration ownership MUST be verified and migrated according to the **Device-Authoritative Lazy Adoption Rule**:
     - **Case A (Already Owned):** If `cfg.subscriber == CallerId`, configuration ownership is satisfied and the mutation proceeds to payload validation.
     - **Case B (Legacy / Unclaimed Configuration Adoption):** If `cfg.subscriber == ""` (empty) on an owned device (`device.subscriberId == CallerId`):
       - The system MUST dynamically adopt the configuration record during the update transaction by setting `cfg.subscriber = CallerId` and `cfg.subscriberOnly = true`. Existing provisioned devices MUST NOT be locked out due to unpopulated legacy ownership fields.
     - **Case C (Foreign Ownership Conflict):** If `cfg.subscriber != ""` AND `cfg.subscriber != CallerId`:
       - The system MUST reject the request fail-closed with `HTTP 403 ACCESS_DENIED` (*"Configuration record belongs to another subscriber"*).
   - No shared-template cloning or copy-on-write (CoW) behavior is required or permitted.
   - **Migration Job**: A background database maintenance script SHOULD backfill `cfg.subscriber = sub_devices4.subscriberId` and `cfg.subscriberOnly = true` for all active records where `sub_devices4.configuration == cfg.id`.

#### 5.2.5 DELETE `/api/v1/subscriberDevice/{id}` (Unclaiming & Removal)
1. The system MUST load the target device (resolving by UUID or hardware serial number) and verify ownership: `device.subscriberId == CallerId`. Ownership mismatch MUST return `HTTP 403 ACCESS_DENIED`.
2. Upon successful ownership verification, the system MUST execute the existing device unclaim and inventory disassociation workflow.

---

### 5.3 Venue

#### 5.3.1 GET `/api/v1/venue/{id}`
1. The system MUST load the venue record from `VenueDB` using `{id}`.
2. If the venue does not exist, the system MUST return `HTTP 404 Not Found`.
3. The system MUST verify that `venue.subscriber == CallerId`. If the venue belongs to another subscriber or an administrative entity (`venue.subscriber != CallerId`), the system MUST return `HTTP 403 ACCESS_DENIED`.

#### 5.3.2 PUT `/api/v1/venue/{id}`
1. The system MUST load the venue record and assert `venue.subscriber == CallerId`. Ownership mismatch MUST return `HTTP 403 ACCESS_DENIED`.
2. The system MUST enforce the venue field allowlist defined in Section 6.4.

#### 5.3.3 POST `/api/v1/venue` and DELETE `/api/v1/venue/{id}`
1. Venue creation and venue deletion MUST NOT be permitted as subscriber capabilities.
2. The system MUST reject any subscriber request attempting `POST /api/v1/venue` or `DELETE /api/v1/venue/{id}` fail-closed with `HTTP 403 ACCESS_DENIED`.

---

### 5.4 Configuration (Administrative Boundary)

1. **Administrative Exclusivity**: Standalone configuration endpoints under `/api/v1/configuration*` are purely administrative APIs.
2. **Fail-Closed Denial**: Any direct or indirect invocation of `GET`, `POST`, `PUT`, or `DELETE` on `/api/v1/configuration*` using a subscriber Bearer token MUST be rejected fail-closed with `HTTP 403 ACCESS_DENIED`.
3. **Encapsulation Independence**: Internal configuration records generated or modified during authorized `SubscriberDevice` operations MUST NOT grant subscriber access to the standalone Configuration API.

---

## 6. Mutation and Field Authorization Requirements

Authorization to access or own a resource DOES NOT grant unrestricted permission to modify its schema or data transfer object (DTO). The system MUST enforce the following authorization rule:

```text
Resource Ownership
        +
Permitted Operation
        +
Permitted Field Allowlists
        =
Authorized Mutation
```

### 6.1 Subscriber Mutations

For `PUT /api/v1/subscriber` (Subscriber Self-Service):
1. **Permitted Operation**: `operation=updateMac` is the ONLY permitted subscriber operation.
2. **Permitted Fields**: The request payload MUST contain only the new hardware `macAddress`.
3. **Prohibited Operations**: Any subscriber request attempting other operations—including `emailVerified`, operator notes, billing tags, or status updates—MUST be rejected with `HTTP 403 ACCESS_DENIED`. (As defined in Section 5.1.3, `operation=emailVerified` is an internal microservice operation executed under Track 1 inter-service authentication).

### 6.2 SubscriberDevice Pure Positive Allowlist

For `PUT /api/v1/subscriberDevice/{id}`:

1. **Pure Positive Validation Model (Zero-Trust Whitelist Only)**:
   - The validation engine MUST NOT evaluate or maintain denylists, blacklists, or prohibited field sets in code.
   - The system MUST accept ONLY explicitly allowlisted fields at their exact designated schema layers.
   - Any unlisted field, extra property, misplaced key, unexpected configuration section, or out-of-range enumerated value MUST be rejected immediately with `HTTP 400 Bad Request` (*"Invalid subscriber device configuration payload: contains unrecognized or disallowed properties"*).

2. **Layer 1: Top-Level DTO Allowlist**:
   - Subscribers MAY mutate only the following top-level properties on `SubscriberDevice`:
     - `name`: String, length $1 \le L \le 64$ characters.
     - `configuration`: Array of configuration element objects.
   - If the request payload contains ANY other top-level property (such as `id`, `serialNumber`, `subscriberId`, `venueId`, `operatorId`, `locationId`, `deviceType`, `rrm`, etc.), the system MUST reject the request with `HTTP 400 Bad Request`.

### 6.3 Deep Configuration Pure Positive Allowlist & Merge Semantics

When a subscriber update includes the `configuration` array, the system MUST enforce positive structural matching at every layer of the JSON tree:

1. **Layer 2: Configuration Section Allowlist**:
   - The `configuration` array MUST contain only element objects where `"name": "interfaces"`.
   - The array MUST be bounded to at most 1 section element.
   - Any section element whose `"name"` is not `"interfaces"` (such as `"radios"`, `"switch"`, `"services"`, `"metrics"`, `"firewall"`, or `"config-raw"`) is not in the allowlist and MUST be rejected with `HTTP 400 Bad Request`.

2. **Layer 3: Interface Object Allowlist**:
   - Within the `"interfaces"` section, only interface objects where `"role": "downstream"` (or `"role": "lan"`) are permitted.
   - Any interface object specifying any other role (e.g. `"upstream"`, `"wan"`), or defining `"network": "management"`, is not in the allowlist and MUST be rejected with `HTTP 400 Bad Request`.

3. **Layer 4: SSID Object Allowlist**:
   - Within permitted downstream interfaces, only SSID elements where `"bss-mode": "ap"` are permitted.
   - Any SSID element specifying any other mode (e.g. `"mesh"`, `"sta"`) is not in the allowlist and MUST be rejected with `HTTP 400 Bad Request`.

4. **Layer 5: SSID Leaf Property Allowlist**:
   - Within an allowlisted AP SSID object, the system permits strictly the following leaf properties:
     - `name`: SSID name, string, length $1 \le L \le 32$ printable characters.
     - `hidden-ssid`: Boolean (`true` or `false`), optional.
     - `encryption`: Object containing strictly:
       - `proto`: String enum restricted to `["psk2", "sae", "sae-mixed", "psk-mixed"]`.
       - `key`: Passphrase string, length $8 \le L \le 63$ ASCII characters (or 64 hexadecimal characters).
   - Any other property inside the SSID object or encryption block is not in the allowlist and MUST be rejected with `HTTP 400 Bad Request`.

5. **Non-Destructive Deep Patching**:
   - The backend MUST NOT overwrite the canonical device configuration template with the subscriber payload.
   - The system MUST apply only the validated leaf properties (`name`, `hidden-ssid`, `encryption.proto`, `encryption.key`) to the matching client AP SSID in the existing configuration, leaving radios, switch ports, upstream WAN interfaces, telemetry blocks, and downstream parental-control filtering rules intact.

### 6.4 Venue Mutations

For `PUT /api/v1/venue/{id}`:
1. **Permitted Descriptive Fields**: Subscribers MAY mutate only descriptive metadata:
   - `name` (string)
   - `description` (string)
   - `location` (object/string)
2. **Prohibited Ownership & Hierarchy Fields**: Any attempt to modify `subscriber`, `entity`, parent venue pointers, or administrative policies MUST be rejected with `HTTP 403 ACCESS_DENIED`.

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
4. **Internal Service Authentication Responsibility**: Direct external invocation without valid trusted internal service authentication MUST NOT be permitted. Requests arriving on the internal router (`Internal_ == true`) with verified `X-INTERNAL-NAME` are classified as Track 1 Internal Microservice Principals at `RoleIsAuthorized()`, bypassing subscriber RBAC checks to allow registration (`POST /api/v1/subscriber`) and email verification (`PUT /api/v1/subscriber?operation=emailVerified`) to proceed to existing service handlers.

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
> **Rule:** The request is structurally understood, but the caller is not permitted to perform the requested operation or access/modify the requested resource.

Enforced for:
- Foreign resource access (e.g., target device or venue does not belong to caller).
- Identity mismatch: A client-supplied `subscriberId` does not match the authenticated caller (e.g., `{ "subscriberId": "someone-else" }` $\longrightarrow$ `HTTP 403 ACCESS_DENIED`).
- Administrative field modification: Attempts to mutate administrative, ownership, or policy properties (`deviceType`, `managementPolicy`, `deviceRules`, `subscriberId`, `operatorId`, `realMacAddress`).
- Unauthorized endpoints or operations: Invocations of administrative endpoints (e.g., `POST /api/v1/venue`, `DELETE /api/v1/venue/{id}`, or parameterized `DELETE /api/v1/subscriber/{foreign_id}` where `{foreign_id} != CallerId`).
- Standalone configuration access: Invocations of `/api/v1/configuration*`.
- Unsupported subscriber operations: Profile mutations outside `operation=updateMac`.
- Syntactically recognized unauthorized paths (e.g., `POST /api/v1/subscriberDevice/123`).
- Prohibited query parameters: Use of `?email=...`, `?macAddress=...`, or `?listOnly=true` on profile GET.
- Query parameter `signupUUID` mismatching `CallerId`.
- Missing, empty, or unresolvable subscriber identity context.

#### HTTP 400 — Request Validation Failure
> **Rule:** The operation is permitted for the caller, but the request representation itself is invalid, unsupported, or violates domain schema constraints.

Enforced for:
- Unknown fields: Request payload contains unrecognized properties where an allowlist applies (e.g., `{ "someUnknownField": "value" }` $\longrightarrow$ `HTTP 400 Bad Request`).
- Invalid nested Wi-Fi structure: Configuration payload targets non-wireless interfaces, upstream WAN backhaul, or unsupported sections.
- Unsupported configuration blocks or unknown nested configuration keys.
- Management network violations: Nested Wi-Fi payload defining, modifying, or referencing `network: "management"`.
- Malformed request syntax or unparseable JSON payload.
- Malformed path parameters or invalid structural representation.
- Invalid data types, formats, or values outside allowable ranges (e.g., invalid MAC format).
- Active claimed devices remaining during subscriber self-deletion (`StillInUse`).

---

## 10. Security Requirements

1. **Cross-Subscriber Isolation**: The system MUST strictly enforce isolation between subscribers. A subscriber MUST NOT be able to view, claim, mutate, or delete resources belonging to any other subscriber.
2. **Server-Derived Ownership and Identity**: Ownership of newly claimed or created resources MUST be assigned server-side from `CallerId`. If a client supplies an optional `subscriberId`, it MUST match `CallerId` (`HTTP 403 ACCESS_DENIED` on mismatch); client-supplied ownership values have zero authority to establish or expand caller authorization scope.
3. **No Operator / Tenant Dependency**: The subscriber authorization path MUST NOT evaluate or depend upon `operatorId`, `CallerTenant`, or tenant equality checks.
4. **Strict Collection Scoping**: Device collection queries MUST be scoped strictly to `WHERE subscriberId = CallerId` with zero possibility of falling back to operator-wide visibility.
5. **Exact-Match Capabilities**: The system MUST enforce exact-match HTTP method capabilities with zero method aliasing.
6. **Property-Level Mutation Allowlists**: The system MUST enforce strict property-level allowlists for all mutations; denylist-based authorization controls MUST NOT be used.
7. **Administrative Field Immutability**: Administrative, management, and ownership properties MUST remain strictly read-only for subscribers and rejected with `403 ACCESS_DENIED` upon mutation attempt.
8. **Deep Configuration Protection**: Nested configuration JSON MUST enforce schema and structural constraints, preventing modification of management networks or non-client wireless interfaces.
9. **Fail-Closed Default**: Authorization MUST fail closed with `HTTP 403 ACCESS_DENIED` for any unauthorized capability, unsupported subscriber operation, unauthorized path variant, ownership mismatch, or missing/incomplete identity context. Structurally invalid, malformed, or unparseable request representations MUST fail closed with `HTTP 400 Bad Request`.
10. **Subscriber-Safe Response Projection**: The system MUST project subscriber profile responses to strip internal operator notes, classification tags, and internal error traces.
11. **Preservation of Administrative RBAC**: Administrative roles, policies, and privileges MUST remain unaffected by subscriber authorization logic.
12. **Business Logic Separation**: Authorization logic determines whether an operation is permitted; existing business logic handlers remain responsible for operational integrity checks (e.g., serial syntax, MAC uniqueness).
13. **Concurrency and TOCTOU Invariant**: Authorization and direct resource ownership invariants MUST be enforced for all operations. Concurrency and TOCTOU hardening are intentionally deferred to a follow-up issue and do not require transaction or concurrency redesign as part of Issue #90.

---

## 11. Acceptance Criteria

### 11.1 Subscriber Profile
- **AC-SUB-1**: An authenticated subscriber calling bare `GET /api/v1/subscriber` successfully retrieves their own profile (`userId == CallerId`).
- **AC-SUB-2**: The response payload for `GET /api/v1/subscriber` contains only subscriber-safe fields; internal operator metadata (`notes`, `tags`, internal error traces) is redacted.
- **AC-SUB-3**: A subscriber calling `GET /api/v1/subscriber` with query parameter `?email=...`, `?macAddress=...`, or `?listOnly=true` receives `HTTP 403 ACCESS_DENIED`.
- **AC-SUB-4**: A subscriber calling `GET /api/v1/subscriber?signupUUID=<own_id>` successfully retrieves their profile; calling with `?signupUUID=<foreign_id>` receives `HTTP 403 ACCESS_DENIED`.
- **AC-SUB-5**: An authenticated subscriber calling `PUT /api/v1/subscriber?operation=updateMac` with a valid, unassigned MAC successfully updates their profile.
- **AC-SUB-6**: An authenticated subscriber calling `PUT /api/v1/subscriber` with an operation other than `updateMac` (or an empty operation) receives `HTTP 403 ACCESS_DENIED`.
- **AC-SUB-7**: An authenticated subscriber calling bare `DELETE /api/v1/subscriber` with zero active claimed devices successfully deletes their profile.
- **AC-SUB-8**: An authenticated subscriber calling bare `DELETE /api/v1/subscriber` with active claimed devices remaining receives `HTTP 400 Bad Request` (`StillInUse`).
- **AC-SUB-9**: An authenticated subscriber calling parameterized `DELETE /api/v1/subscriber/{id}` where `{id} == CallerId` and zero active claimed devices remain successfully deletes their profile; calling where `{id} != CallerId` receives `HTTP 403 ACCESS_DENIED`.

### 11.2 Subscriber Devices
- **AC-DEV-1**: An authenticated subscriber calling `GET /api/v1/subscriberDevice` receives only devices where `subscriberId == CallerId`.
- **AC-DEV-2**: An authenticated subscriber calling `GET /api/v1/subscriberDevice/{id}` (via UUID or hardware serial number) for an owned device receives the device payload; calling for a foreign device receives `HTTP 403 ACCESS_DENIED`.
- **AC-DEV-3**: An authenticated subscriber calling `POST /api/v1/subscriberDevice/0` with eligible claim data successfully claims the device, with ownership assigned to `CallerId`.
- **AC-DEV-4**: An authenticated subscriber calling `POST /api/v1/subscriberDevice/{non_zero_id}` receives `HTTP 403 ACCESS_DENIED`.
- **AC-DEV-5**: A client-supplied `subscriberId` in the body of `POST /api/v1/subscriberDevice/0` that does not match `CallerId` is rejected with `HTTP 403 ACCESS_DENIED`; if absent or matching `CallerId`, the device is claimed with `newDevice.subscriberId = CallerId`.
- **AC-DEV-6**: An authenticated subscriber calling `PUT /api/v1/subscriberDevice/{id}` on an owned device with allowlisted fields (`name`, `configuration` containing valid downstream AP SSID updates) successfully updates the device.
- **AC-DEV-7**: An authenticated subscriber calling `PUT /api/v1/subscriberDevice/{id}` on a foreign device receives `HTTP 403 ACCESS_DENIED`.
- **AC-DEV-8**: An authenticated subscriber calling `PUT /api/v1/subscriberDevice/{id}` with unlisted top-level fields (e.g. `serialNumber`, `deviceType`, `subscriberId`, `operatorId`) receives `HTTP 400 Bad Request`.
- **AC-DEV-9**: An authenticated subscriber calling `PUT /api/v1/subscriberDevice/{id}` with unlisted configuration sections (e.g. `radios`, `switch`, `services`, `config-raw`), WAN roles (`upstream`), or mesh modes receives `HTTP 400 Bad Request`.
- **AC-DEV-10**: A nested Wi-Fi configuration update containing valid allowlisted leaf properties (`name`, `hidden-ssid`, `encryption.proto`, `encryption.key`) in a client AP SSID block succeeds.
- **AC-DEV-11**: A nested configuration update referencing `network: "management"` or specifying any unlisted property at any layer receives `HTTP 400 Bad Request`.
- **AC-DEV-12**: An authenticated subscriber calling `PUT /api/v1/subscriberDevice/{id}` on an owned device where the linked configuration has `cfg.subscriber == ""` (legacy configuration) automatically adopts the configuration (`cfg.subscriber = CallerId`, `cfg.subscriberOnly = true`); calling where `cfg.subscriber != ""` and `cfg.subscriber != CallerId` receives `HTTP 403 ACCESS_DENIED`.
- **AC-DEV-13**: An authenticated subscriber calling `DELETE /api/v1/subscriberDevice/{id}` (via UUID or hardware serial number) on an owned device successfully unclaims the device; calling on a foreign device receives `HTTP 403 ACCESS_DENIED`.

### 11.3 Venues
- **AC-VEN-1**: An authenticated subscriber calling `GET /api/v1/venue/{id}` on an owned venue (`venue.subscriber == CallerId`) receives the venue record; calling on a foreign venue receives `HTTP 403 ACCESS_DENIED`.
- **AC-VEN-2**: An authenticated subscriber calling `PUT /api/v1/venue/{id}` on an owned venue with permitted fields (`name`, `description`, `location`) successfully updates the venue.
- **AC-VEN-3**: An authenticated subscriber calling `PUT /api/v1/venue/{id}` attempting to modify `subscriber`, `entity`, or administrative settings receives `HTTP 403 ACCESS_DENIED`.
- **AC-VEN-4**: An authenticated subscriber calling `POST /api/v1/venue` receives `HTTP 403 ACCESS_DENIED`.
- **AC-VEN-5**: An authenticated subscriber calling `DELETE /api/v1/venue/{id}` receives `HTTP 403 ACCESS_DENIED`.

### 11.4 Standalone Configuration Boundary
- **AC-CFG-1**: Any direct subscriber request targeting `/api/v1/configuration*` (`GET`, `POST`, `PUT`, `DELETE`) receives `HTTP 403 ACCESS_DENIED`.

### 11.5 Registration Flow
- **AC-REG-1**: A direct external request to `POST /api/v1/subscriber` without valid internal service credentials (`X-API-KEY`) is rejected by the authentication/authorization layer.
- **AC-REG-2**: An internal service call from UserPortal with valid `X-API-KEY` to `POST /api/v1/subscriber` succeeds and initiates the signup workflow.

### 11.6 Administrative RBAC Preservation
- **AC-ADM-1**: Administrative requests (`ROOT`, `Admin`, `CSR`, `Installer`) continue to evaluate through hierarchical `ManagementRole` and `PolicyDB` rules without deviation or regression.
- **AC-ADM-2**: An authenticated administrative user accessing subscriber-capable endpoints (such as `GET /api/v1/subscriberDevice` or `DELETE /api/v1/subscriber/{id}`) is not classified as a subscriber and evaluates exclusively through administrative RBAC.

---

## 12. Testing Requirements

Test suites MUST provide comprehensive verification across all authorization boundaries without redundant test cases.

```text
+-------------------------------------------------------------------------+
| Test Suite Structure & Boundary Verification Matrix                     |
+-------------------------------------------------------------------------+
| 1. Subscriber Identity & Context:                                       |
|    - Verify valid Bearer JWT resolves to UserInfo_.userinfo.id.         |
|    - Verify missing or unresolvable identity fails closed with 403.     |
|    - Verify client-supplied subscriberId must match CallerId (403 on    |
|      mismatch); if omitted, CallerId is used.                           |
|                                                                         |
| 2. Exact-Match Capability Enforcement:                                  |
|    - Verify zero method aliasing (PUT does not authorize POST/DELETE).  |
|    - Verify unauthorized resources (configuration, etc.) fail with 403. |
|                                                                         |
| 3. Collection Scoping:                                                  |
|    - Verify GET /subscriberDevice filters strictly to caller's devices. |
|    - Verify zero operator-wide fallback on empty or foreign filters.    |
|                                                                         |
| 4. Resource Ownership & Isolation:                                      |
|    - Verify owned device/venue GET/PUT/DELETE succeeds.                 |
|    - Verify cross-subscriber access on foreign resource returns 403.     |
|                                                                         |
| 5. Sentinel Creation Enforcement:                                       |
|    - Verify POST /subscriberDevice/0 claims device for caller.          |
|    - Verify non-sentinel paths (/123, /new) fail closed with 403.       |
|                                                                         |
| 6. Pure Positive Allowlists & Schema Enforcement:                       |
|    - Verify top-level device allowlist (name, configuration).           |
|    - Verify unlisted top-level/administrative fields return 400.        |
|    - Verify nested configuration allowlist (interfaces -> downstream -> |
|      ap -> name, hidden-ssid, encryption).                              |
|    - Verify unlisted sections/roles/modes/keys return 400 Bad Request.  |
|    - Verify legacy config adoption (cfg.subscriber == "" -> CallerId).  |
|                                                                         |
| 7. Profile & Venue Boundaries:                                          |
|    - Verify GET /subscriber response redaction (notes/tags stripped).   |
|    - Verify prohibited query parameters on GET /subscriber return 403.  |
|    - Verify PUT /subscriber permits only operation=updateMac for subs.  |
|    - Verify bare DELETE /subscriber enforces device-count constraint.   |
|    - Verify parameterized DELETE /subscriber/{id} allows {id}==CallerId |
|      (with device constraint) and rejects foreign {id} with 403.        |
|    - Verify venue POST and DELETE return 403.                           |
|                                                                         |
| 8. Registration vs. Self-Service Separation:                            |
|    - Verify POST /subscriber requires internal X-API-KEY/X-INTERNAL-NAME|
|    - Verify internal calls bypass subscriber RBAC via Track 1.          |
|    - Verify PUT /subscriber?operation=emailVerified allowed for Track 1 |
|      and rejected with 403 for external subscriber Bearer tokens.       |
|                                                                         |
| 9. Administrative RBAC Regression:                                      |
|    - Verify ROOT and operator roles retain unchanged authorization.     |
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
7. **No Transaction or Concurrency Overhaul**: Platform-wide transaction redesign, distributed locking, and TOCTOU race-condition hardening are deferred to a follow-up issue.
8. **No Hardware Proof-of-Possession**: Physical device entitlement and challenge-response protocols will not be added in this issue.
9. **No Business Workflow Redesign**: Existing handlers retain responsibility for their internal business validations.

---

## 15. Traceability

The following matrix maps requirement areas to specification sections, acceptance criteria, and testing suites.

| Requirement Area | Specification Section | Acceptance Criteria | Testing Requirement |
|---|---|---|---|
| **Subscriber Identity** | Section 4.1, 4.6 | AC-SUB-1, AC-DEV-5 | Identity & Context Suite |
| **Exact-Match Capabilities** | Section 4.2 | AC-CFG-1, AC-VEN-4, AC-VEN-5 | Capability Enforcement Suite |
| **Collection Scoping** | Section 4.4 | AC-DEV-1 | Collection Scoping Suite |
| **Resource Ownership** | Section 4.3 | AC-DEV-2, AC-VEN-1 | Ownership & Isolation Suite |
| **Creation Sentinel Path** | Section 5.2.3 | AC-DEV-3, AC-DEV-4 | Sentinel Creation Suite |
| **Mutation Allowlists** | Section 6.2 | AC-DEV-6, AC-DEV-8, AC-DEV-9 | Mutation Allowlists Suite |
| **Nested Wi-Fi Configuration** | Section 6.3 | AC-DEV-10, AC-DEV-11 | Mutation Allowlists Suite |
| **Profile Safe Projection** | Section 5.1.2 | AC-SUB-2, AC-SUB-3, AC-SUB-4 | Profile & Venue Suite |
| **Profile Operations** | Section 5.1.3 | AC-SUB-5, AC-SUB-6 | Profile & Venue Suite |
| **Subscriber Self-Deletion** | Section 5.1.4, 5.1.5 | AC-SUB-7, AC-SUB-8, AC-SUB-9 | Profile & Venue Suite |
| **Venue Authorization** | Section 5.3, 6.4 | AC-VEN-1, AC-VEN-2, AC-VEN-3 | Profile & Venue Suite |
| **Configuration API Boundary** | Section 5.4 | AC-CFG-1 | Capability Enforcement Suite |
| **Registration Flow (Flow A)** | Section 7 | AC-REG-1, AC-REG-2 | Registration Separation Suite |
| **Administrative Compatibility**| Section 8 | AC-ADM-1 | Administrative Regression Suite |
| **Error Differentiation (403/400)**| Section 9 | AC-DEV-8, AC-DEV-9, AC-SUB-8 | All Suites |
| **Security Requirements** | Section 10 | All ACs | All Suites |
