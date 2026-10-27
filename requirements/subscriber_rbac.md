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
- Preventing concurrency/TOCTOU race conditions that could permit cross-subscriber resource access or mutation.
- Preserving administrative RBAC behavior (`FindAllUserRoles()`, `PolicyAllows()`, `ManagementRole`, `PolicyDB`) without regression.

### 3.2 Out of Scope

The following areas are explicitly excluded from the scope of Issue #90:
- **Standalone Configuration Endpoints**: Direct subscriber access to `/api/v1/configuration*` is administrative-only and denied fail-closed.
- **Administrative RBAC Redesign**: Modifications to administrative role structures, policy inheritance, entity hierarchy, or staff permissions.
- **Hardware Proof-of-Possession**: Physical hardware entitlement, challenge-response verification, or supply-chain validation during device claiming.
- **Database / ORM Overhaul**: General ORM refactoring, database schema migrations, composite index additions, query restructuring, or caching layers.
- **Platform Concurrency Redesign**: Global transaction management, distributed database locking, or general multi-column race condition refactoring outside exploitable subscriber ownership boundaries.
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

### 4.2 Subscriber Capability

1. **Static In-Code Policy**: Subscriber authorization policy is a static in-code policy and MUST NOT depend on administrative `RolesDB` or `PolicyDB` resolution.
2. **Dedicated and Explicit**: The subscriber policy MUST be dedicated to subscriber self-service authorization, explicit, and fail-closed.
3. **No Management Role Dependency**: Subscriber policy MUST NOT be derived from `ManagementRole` records, MUST NOT require `FindAllUserRoles()`, and MUST NOT introduce synthetic management roles into administrative storage.
4. **Evaluation Precedence**: Subscriber authorization MUST be evaluated before administrative RBAC resolution, `ManagementRole` queries, and staff/operator shortcuts.
5. **Zero Method Aliasing**: The system MUST enforce exact-match HTTP method authorization.
   - An explicit grant for `GET` MUST NOT authorize `POST`, `PUT`, or `DELETE`.
   - An explicit grant for `PUT` MUST NOT authorize `POST` or `DELETE`.
   - Method-aliasing semantics in `PolicyAllows()` (such as `UPDATE` satisfying `CREATE`) MUST NOT be applied to subscribers.
6. **Capability Matrix**: The system MUST enforce the following exact-match capability matrix:

| Resource | `GET` | `POST` | `PUT` | `DELETE` | Capability Rule |
|---|:---:|:---:|:---:|:---:|---|
| **`subscriberDevice`** | **Allowed** | **Allowed** | **Allowed** | **Allowed** | Full device self-care lifecycle (`POST` constrained to sentinel `/0`). |
| **`subscriber`** | **Allowed** | **Denied** | **Allowed** | **Allowed** | Profile inspection, MAC update, and bare self-deletion. `POST` is denied (Flow A only). |
| **`venue`** | **Allowed** | **Denied** | **Allowed** | **Denied** | Venue inspection and metadata update only. Creation and deletion are administrative. |
| **`configuration`** | **Denied** | **Denied** | **Denied** | **Denied** | Administrative-only resource. All subscriber methods denied fail-closed. |
| *(Any Other Resource)* | **Denied** | **Denied** | **Denied** | **Denied** | Unknown or unspecified resources fail closed immediately. |

### 4.3 Resource Ownership

The system MUST distinguish between endpoints with and without explicit target resource identifiers.

#### 4.3.1 Case A — Endpoints Without Target Resource ID
When an authorized endpoint does not contain a target resource ID in its URI path (e.g., `GET /api/v1/subscriber`, `PUT /api/v1/subscriber`, bare `DELETE /api/v1/subscriber`, and `GET /api/v1/subscriberDevice`):
1. The system MUST bind the operational scope directly and implicitly to `CallerId = UserInfo_.userinfo.id`.
2. The system MUST NOT require the client to supply an external `subscriberId` parameter merely to assert identity.
3. The system MUST NOT perform a separate identity assertion check comparing client-supplied identity against `CallerId`.

#### 4.3.2 Case B — Endpoints With Target Resource ID
When an authorized endpoint contains an explicit target resource ID in its URI path (e.g., `/api/v1/subscriberDevice/{id}` or `/api/v1/venue/{id}`):
1. The target resource ID MUST be treated strictly as an object locator, NOT an authorization assertion.
2. The system MUST load the target resource record from the database and assert that the record is owned directly by the caller:
   - For `SubscriberDevice`: `Device.subscriberId == CallerId`
   - For `Venue`: `Venue.subscriber == CallerId`
3. If the record does not exist, the system MUST return `HTTP 404 Not Found`.
4. If the record exists but the ownership assertion fails (`Device.subscriberId != CallerId` or `Venue.subscriber != CallerId`), the system MUST reject the request fail-closed with `HTTP 403 ACCESS_DENIED`.
5. Resource ownership verification MUST be evaluated as an individual direct resource ownership check (`resource.subscriberId == CallerId` or `venue.subscriber == CallerId`); it MUST NOT be delegated to tenant or operator checks.

### 4.4 Collection Scoping

1. **Unconditional Query Constraint**: For subscriber collection requests (`GET /api/v1/subscriberDevice`), the system MUST constrain the underlying database query strictly to:
   ```sql
   WHERE subscriberId = :CallerId
   ```
2. **Zero Operator Fallback**: If the query returns zero records, or if request query parameters are omitted, the query MUST NOT broaden or fall back to operator-wide, entity-wide, or venue-wide visibility.
3. **Filter Override**: Any client-supplied query parameters attempting to specify a different `subscriberId`, `operatorId`, or `entityId` MUST NOT expand collection visibility beyond `CallerId`.

### 4.5 Fail-Closed Authorization

1. **Evaluation Order**: The subscriber authorization pipeline MUST be evaluated in `RESTAPIHandler::RoleIsAuthorized()` BEFORE `FindAllUserRoles()`, BEFORE administrative `ManagementRole` database queries, and BEFORE staff/operator global GET shortcuts.
2. **Authorization Rejection Precedence**: If a subscriber request fails authorization conditions—including capability checks, unauthorized method/path operations, missing or unresolvable identity context, foreign resource ownership, or attempts to modify administrative fields—the request MUST fail closed immediately with `HTTP 403 ACCESS_DENIED`.
3. **Invalid Request and Schema Precedence**: If an authorized subscriber request contains malformed request syntax, unknown top-level mutation fields, unknown nested configuration keys, or unsupported nested configuration structures, the request MUST fail closed with `HTTP 400 Bad Request` in accordance with Section 9.
4. **No Administrative Fallthrough**: A rejected or unmatched subscriber request MUST NOT fall through to administrative RBAC evaluation.
5. **Path and Operation Boundary**:
   - When a requested path or operation is syntactically recognized but represents an action or path variant the subscriber is not permitted to invoke (such as `DELETE /api/v1/subscriber/{id}`, `POST /api/v1/venue`, or `POST /api/v1/subscriberDevice/{non-zero-id}`), authorization MUST fail closed with `HTTP 403 ACCESS_DENIED`.
   - When the request structure or path parameter itself is malformed or cannot be interpreted as a valid request representation by the parsing layer, the request MUST fail closed with `HTTP 400 Bad Request`.

### 4.6 Client-Supplied Identity and Scope

1. **Zero Authority**: Client-supplied identifiers—including `subscriberId`, `operatorId`, `tenantId`, `entityId`, and `email`—have zero authority to establish, override, or expand subscriber ownership or authorization scope.
2. **Mandatory Caller-Derived Ownership**: For all subscriber-created or subscriber-claimed resources, ownership MUST be derived directly from the authenticated caller (`CallerId`). Specifically, on device creation (`POST /api/v1/subscriberDevice/0`), the implementation MUST enforce:
   ```cpp
   newDevice.subscriberId = CallerId;
   ```
   A client MUST NOT be able to choose or assign ownership to any other subscriber.
3. **Handling of Client-Supplied Ownership Fields**: If a client supplies `subscriberId` or any other ownership field in a request payload:
   - The supplied value MUST NEVER cause ownership to be assigned to that value.
   - The implementation MAY reject the conflicting field with `HTTP 403 ACCESS_DENIED` or ignore/overwrite it where existing endpoint-specific behavior permits.
   - The final persisted ownership of the resource MUST always be `CallerId`.
4. **Elimination of Tenant and Operator Logic**: The subscriber authorization path MUST NOT evaluate `CallerTenant`, tenant equality checks, multi-tenant derivation logic, entity hierarchy checks, or operator boundaries. Subscriber authorization is strictly bounded by `CallerId` and direct resource ownership.

---

## 5. Endpoint Requirements

The system MUST enforce subscriber authorization across all endpoints in accordance with the specifications below.

| Method | Endpoint | Subscriber Access | Required Scope & Authorization Rule |
|---|---|:---:|---|
| `POST` | `/api/v1/subscriber` | **Denied** | Not a subscriber capability; internal service registration flow only (Flow A via `X-API-KEY`). |
| `GET` | `/api/v1/subscriber` | **Allow** | Scoped strictly to `CallerId`; prohibits `email`, `macAddress`, `listOnly`; subscriber-safe projection. |
| `PUT` | `/api/v1/subscriber` | **Allow** | Scoped strictly to `CallerId`; permitted operation `updateMac` only; verifies MAC availability. |
| `DELETE` | `/api/v1/subscriber` | **Allow** | Bare path only; scoped to `CallerId`; requires active claimed device count == 0. |
| `DELETE` | `/api/v1/subscriber/{id}` | **Denied** | Administrative-only endpoint; fails closed with `403 ACCESS_DENIED` even if `{id} == CallerId`. |
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
1. Direct external invocation without valid trusted internal service authentication MUST NOT be permitted.
2. `POST /api/v1/subscriber` is part of the trusted internal registration flow (Flow A) and is NOT an authenticated subscriber RBAC capability.
3. The existing authentication/authorization layer determines the appropriate HTTP status for missing or invalid internal service authentication.
4. The system MUST NOT evaluate subscriber Bearer-token authorization, MUST NOT perform a `CallerId` ownership assertion, MUST NOT perform a `subscriberId == CallerId` check, and MUST NOT require subscriber role lookups on this endpoint.

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
1. The system MUST authorize subscribers to update their profile strictly when all the following conditions are satisfied:
   - The request specifies query parameter `operation=updateMac`.
   - The caller owns the record (`SE.userId == CallerId`).
2. **Operation Guard**: Any operation other than `updateMac` (e.g., `emailVerified`, empty operation, or arbitrary string) MUST be rejected with `HTTP 403 ACCESS_DENIED`.
3. **MAC Address Validation**: The system MUST verify that the requested new MAC address is currently unassigned across `SubscriberDeviceDB`, `InventoryDB`, and `SignupDB`. If the MAC address is currently assigned or invalid, the request MUST be rejected.

#### 5.1.4 DELETE `/api/v1/subscriber` (Bare Path)
1. The system MUST authorize self-deletion strictly on the bare URI path `/api/v1/subscriber` without path parameters.
2. The deletion operation MUST target `CallerId` directly.
3. **Active Device Check**: The system MUST query `InventoryDB` and `SubscriberDeviceDB` for active claimed devices associated with `CallerId`.
4. **Conditional Deletion**:
   - If active claimed devices remain associated with the subscriber, the system MUST reject the request with `HTTP 400 Bad Request` (`StillInUse`).
   - If active claimed device count is zero, the system MUST execute subscriber self-deletion.

#### 5.1.5 DELETE `/api/v1/subscriber/{id}` (Parameterized Path)
1. The system MUST treat `/api/v1/subscriber/{id}` as a strictly administrative endpoint reserved for staff/CSR operators.
2. Any subscriber request targeting `/api/v1/subscriber/{id}` MUST be rejected fail-closed with `HTTP 403 ACCESS_DENIED`, even if `{id}` matches `CallerId`.

---

### 5.2 SubscriberDevice

#### 5.2.1 GET `/api/v1/subscriberDevice` (Collection)
1. The system MUST authorize authenticated subscribers to retrieve their claimed devices.
2. The database query MUST be constrained unconditionally to `WHERE subscriberId = CallerId`.
3. The response MUST contain only devices belonging to `CallerId`.
4. The system MUST NOT fall back to operator-wide visibility under any circumstances.

#### 5.2.2 GET `/api/v1/subscriberDevice/{id}` (Individual)
1. The system MUST load the target device from `SubscriberDeviceDB` using `{id}`.
2. If the device does not exist, the system MUST return `HTTP 404 Not Found`.
3. The system MUST verify that `device.subscriberId == CallerId`. If the device belongs to another subscriber or has no assigned subscriber, the system MUST return `HTTP 403 ACCESS_DENIED`.

#### 5.2.3 POST `/api/v1/subscriberDevice/0` (Claiming & Creation)
1. **Mandatory Sentinel Path**: The system MUST permit subscriber device creation exclusively on the sentinel path `/api/v1/subscriberDevice/0`.
2. **Rejection of Arbitrary Paths**: Any request targeting alternate, syntactically recognized paths (such as `/new`, empty ID, an existing device UUID, or a MAC address) MUST be rejected fail-closed with `HTTP 403 ACCESS_DENIED`. Genuinely malformed request syntax or unparseable paths MUST be rejected with `HTTP 400 Bad Request`.
3. **Server-Derived Ownership**: The system MUST derive device ownership strictly from the authenticated caller:
   ```cpp
   newDevice.subscriberId = CallerId;
   ```
   Client-supplied ownership values in the request body MUST NOT override `CallerId`. If the client supplies a conflicting `subscriberId`, the system MUST reject the request with `HTTP 403 ACCESS_DENIED` or ignore/overwrite it with `CallerId`, ensuring the persisted device owner is always `CallerId`.
4. **Device Claim Eligibility**: The system MUST enforce existing claim validations:
   - Serial number MUST NOT already exist in `SubscriberDeviceDB`.
   - Serial number MUST satisfy syntax validation (`ValidSerialNumber`).
   - `deviceGroup` MUST be specified and MUST be `"ap"` or `"olg"`.
   - A subscriber MUST NOT own more than one active OLG gateway.

#### 5.2.4 PUT `/api/v1/subscriberDevice/{id}` (Mutation)
1. The system MUST load the target device and verify ownership: `device.subscriberId == CallerId`. Ownership mismatch MUST return `HTTP 403 ACCESS_DENIED`.
2. Device ownership alone DOES NOT authorize arbitrary modification. The request MUST satisfy the mutation allowlists defined in Section 6.2 and Section 6.3.
3. **Internal Configuration Encapsulation**: Any internal configuration record updated or created in `ConfigurationDB` as a result of the device mutation MUST satisfy:
   - `cfg.subscriber == CallerId`
   - `cfg.subscriberOnly == true`
4. **Administrative Template Protection**: Any attempt to bind the device to or mutate a shared administrative configuration template (`venue != ""` or `entity != ""` or `cfg.subscriber != CallerId`) MUST be rejected with `HTTP 403 ACCESS_DENIED`.

#### 5.2.5 DELETE `/api/v1/subscriberDevice/{id}` (Unclaiming & Removal)
1. The system MUST load the target device and verify ownership: `device.subscriberId == CallerId`. Ownership mismatch MUST return `HTTP 403 ACCESS_DENIED`.
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

For `PUT /api/v1/subscriber`:
1. **Permitted Operation**: `operation=updateMac` is the ONLY permitted subscriber operation.
2. **Permitted Fields**: The request payload MUST contain only the new hardware `macAddress`.
3. **Prohibited Operations**: Any request attempting other operations—including `emailVerified`, operator notes, billing tags, or status updates—MUST be rejected with `HTTP 403 ACCESS_DENIED`.

### 6.2 SubscriberDevice Mutations

For `PUT /api/v1/subscriberDevice/{id}`:
1. **Top-Level Permitted Fields**: Subscribers MAY mutate only the following top-level fields:
   - `ssid` (string)
   - `wifiPassword` (string)
   - `parentalControl` (object)
2. **Unknown Top-Level Fields**: If the request payload contains any top-level fields outside the allowlist, the system MUST reject the request with `HTTP 400 Bad Request`.
3. **Administrative Field Protection**: If the request payload attempts to modify administrative, ownership, or device-management fields, the system MUST reject the request with `HTTP 403 ACCESS_DENIED`. Protected administrative fields include:
   - `deviceType`
   - `managementPolicy`
   - `deviceRules`
   - `subscriberId`
   - `operatorId`
   - `realMacAddress`

### 6.3 Nested Wi-Fi Configuration

When a device update alters the embedded uCentral Wi-Fi configuration JSON, the system MUST validate the payload recursively against structural and key allowlists:

1. **Target Object Constraints**:
   - Mutations MUST be restricted strictly to user-facing wireless interface blocks and client SSID objects (`interfaces[role != "upstream"].ssids[bss-mode == "ap"]`).
   - Mutations targeting WAN interfaces, upstream backhaul, mesh links (`bss-mode == "mesh"`), or administrative management interfaces MUST be rejected fail-closed with `HTTP 400 Bad Request`.
2. **Property Key Allowlists**:
   - Within permitted user-facing wireless/SSID objects, subscribers MAY alter only:
     - `ssid`
     - `passphrase`
     - `encryption`
3. **Management Network Rejection**: Any nested configuration attempting to define, modify, or reference `network: "management"` MUST be rejected fail-closed with `HTTP 400 Bad Request`.
4. **Unknown Nested Keys**: Any unrecognized properties or structural keys within the nested Wi-Fi object MUST be rejected fail-closed with `HTTP 400 Bad Request`.
5. **Non-Target Section Immutability**: All non-wireless configuration sections—including `radios`, `services`, and `switch`—MUST remain strictly immutable to subscribers.
6. **Internal Configuration Synthesis**: Internal configuration synthesis MUST NOT generate or alter management network blocks.

### 6.4 Venue Mutations

For `PUT /api/v1/venue/{id}`:
1. **Permitted Descriptive Fields**: Subscribers MAY mutate only descriptive metadata:
   - `name` (string)
   - `description` (string)
   - `location` (object/string)
2. **Prohibited Ownership & Hierarchy Fields**: Any attempt to modify `subscriber`, `entity`, parent venue pointers, or administrative policies MUST be rejected with `HTTP 403 ACCESS_DENIED`.

---

## 7. Registration Requirements

The system MUST maintain a strict architectural distinction between unauthenticated user registration (Flow A) and authenticated subscriber self-service (Flow B).

```text
Flow A: New User Registration (Internal Peer IPC)
=================================================
Unauthenticated User
        |
        v
    UserPortal
        |
        | Internal Service Authentication (X-API-KEY)
        v
owprov POST /api/v1/subscriber
        |
        v
Existing Signup Workflow (SignupDB, VenueDB, owsec /signup)


Flow B: Authenticated Subscriber Self-Care (Subscriber RBAC)
============================================================
Authenticated Subscriber
        |
        | Bearer Token (JWT from owsec)
        v
owprov RoleIsAuthorized()
        |
        v
Dedicated Subscriber Authorization Pipeline (CallerId = UserInfo_.userinfo.id)
        |
        v
Subscriber-Scoped Handler & Business Operation
```

1. **Flow A Authentication**: `POST /api/v1/subscriber` is part of Flow A and MUST be authenticated via trusted internal service credentials (`X-API-KEY`). Direct external invocation without valid trusted internal service authentication MUST NOT be permitted.
2. **No Subscriber RBAC on Registration**: Flow A is NOT a subscriber RBAC capability. Unauthenticated external users and Bearer-token callers MUST NOT invoke `POST /api/v1/subscriber` without valid internal service credentials. The existing authentication/authorization layer determines the appropriate HTTP status for missing or invalid internal service authentication.
3. **No Bearer Requirement for Registration**: The system MUST NOT require a Bearer token or authenticated `UserInfo_` context for Flow A.
4. **No Caller Identity Assertion on Registration**: The system MUST NOT execute `subscriberId == CallerId` assertions, subscriber role lookups, or caller ownership checks on `POST /api/v1/subscriber`.

---

## 8. Administrative RBAC Compatibility

1. **Zero Impact on Administrative Roles**: The implementation of subscriber authorization MUST NOT modify the behavior, semantics, or data structures of:
   - `FindAllUserRoles()`
   - Global `PolicyAllows()`
   - `RolesDB`
   - `PolicyDB`
2. **No Synthetic Role Records**: The system MUST NOT insert synthetic subscriber management roles into `RolesDB` or `PolicyDB`.
3. **Execution Precedence**: The dedicated subscriber authorization branch MUST be evaluated in `RoleIsAuthorized()` BEFORE administrative RBAC checks, preventing subscriber tokens from accidentally matching administrative bypass rules or staff global GET shortcuts.
4. **Preservation of Administrative Hierarchy**: Non-subscriber actors (`ROOT`, `Admin`, `CSR`, `Installer`, `NOC`, `Accounting`) MUST continue to be authorized through existing hierarchical `ManagementRole` and `PolicyDB` rules without alteration.

---

## 9. HTTP Error Requirements

The system MUST enforce a clear semantic separation between authorization failures (`HTTP 403 ACCESS_DENIED`) and request content or schema validation failures (`HTTP 400 Bad Request`).

```text
+-------------------------------------------------------------------------+
| HTTP 403 ACCESS_DENIED                                                  |
| Answers: "The caller is not permitted to perform this action."          |
+-------------------------------------------------------------------------+
| Condition: Request is structurally valid, but the authenticated caller  |
| lacks permission for the requested resource, operation, or field.       |
|                                                                         |
| Enforced for:                                                           |
| - Target resource does not belong to caller (ownership mismatch).       |
| - Unsupported subscriber operation (e.g. PUT subscriber !updateMac).    |
| - Administrative endpoint access (DELETE /subscriber/{id}, POST venue). |
| - Standalone configuration API access (/api/v1/configuration*).         |
| - Mutation of administrative fields (deviceType, managementPolicy).     |
| - Syntactically valid unauthorized path (POST /subscriberDevice/123).   |
| - Prohibited query parameters (email, macAddress, listOnly on GET).     |
| - Query parameter signupUUID mismatching CallerId.                      |
| - Missing or ambiguous subscriber identity/context.                     |
+-------------------------------------------------------------------------+

+-------------------------------------------------------------------------+
| HTTP 400 Bad Request                                                    |
| Answers: "This request is not a valid request for this operation."      |
+-------------------------------------------------------------------------+
| Condition: Caller is authorized, but the request payload or structure   |
| violates schema rules, field allowlists, or business integrity rules.   |
|                                                                         |
| Enforced for:                                                           |
| - Malformed request syntax or malformed JSON payload.                   |
| - Unknown top-level fields in device mutation payload.                  |
| - Unknown nested Wi-Fi configuration keys or unsupported structure.    |
| - Nested Wi-Fi payload targeting non-wireless interfaces.               |
| - Nested Wi-Fi payload referencing network: "management".               |
| - Malformed data types, formats, or values outside allowable ranges.    |
| - Active devices remaining during subscriber self-deletion (StillInUse).|
+-------------------------------------------------------------------------+
```

---

## 10. Security Requirements

1. **Cross-Subscriber Isolation**: The system MUST strictly enforce isolation between subscribers. A subscriber MUST NOT be able to view, claim, mutate, or delete resources belonging to any other subscriber.
2. **Server-Derived Ownership**: Ownership of newly claimed or created resources MUST be assigned server-side from `CallerId`. Client-supplied ownership values have zero authority to establish, override, or expand caller authorization scope.
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
13. **Anti-TOCTOU Boundary Protection**: The system MUST prevent concurrency race conditions that could allow a subscriber to cross resource ownership boundaries or mutate foreign resources.

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
- **AC-SUB-9**: An authenticated subscriber calling parameterized `DELETE /api/v1/subscriber/{id}` receives `HTTP 403 ACCESS_DENIED`, even if `{id} == CallerId`.

### 11.2 Subscriber Devices
- **AC-DEV-1**: An authenticated subscriber calling `GET /api/v1/subscriberDevice` receives only devices where `subscriberId == CallerId`.
- **AC-DEV-2**: An authenticated subscriber calling `GET /api/v1/subscriberDevice/{id}` for an owned device receives the device payload; calling for a foreign device receives `HTTP 403 ACCESS_DENIED`.
- **AC-DEV-3**: An authenticated subscriber calling `POST /api/v1/subscriberDevice/0` with eligible claim data successfully claims the device, with ownership assigned to `CallerId`.
- **AC-DEV-4**: An authenticated subscriber calling `POST /api/v1/subscriberDevice/{non_zero_id}` receives `HTTP 403 ACCESS_DENIED`.
- **AC-DEV-5**: A client-supplied foreign `subscriberId` in the body of `POST /api/v1/subscriberDevice/0` cannot establish ownership; the persisted record is assigned to `CallerId` (or the conflicting request is rejected with `HTTP 403 ACCESS_DENIED`).
- **AC-DEV-6**: An authenticated subscriber calling `PUT /api/v1/subscriberDevice/{id}` on an owned device with allowlisted fields (`ssid`, `wifiPassword`, `parentalControl`) successfully updates the device.
- **AC-DEV-7**: An authenticated subscriber calling `PUT /api/v1/subscriberDevice/{id}` on a foreign device receives `HTTP 403 ACCESS_DENIED`.
- **AC-DEV-8**: An authenticated subscriber calling `PUT /api/v1/subscriberDevice/{id}` with unknown top-level fields receives `HTTP 400 Bad Request`.
- **AC-DEV-9**: An authenticated subscriber calling `PUT /api/v1/subscriberDevice/{id}` attempting to mutate administrative fields (`deviceType`, `managementPolicy`, `subscriberId`, `operatorId`, `realMacAddress`) receives `HTTP 403 ACCESS_DENIED`.
- **AC-DEV-10**: A nested Wi-Fi configuration update containing valid `ssid`, `passphrase`, or `encryption` in a client SSID block succeeds.
- **AC-DEV-11**: A nested Wi-Fi configuration update targeting non-wireless interfaces, referencing `network: "management"`, or specifying unknown keys receives `HTTP 400 Bad Request`.
- **AC-DEV-12**: An authenticated subscriber calling `PUT /api/v1/subscriberDevice/{id}` attempting to bind to or alter a shared administrative configuration template receives `HTTP 403 ACCESS_DENIED`.
- **AC-DEV-13**: An authenticated subscriber calling `DELETE /api/v1/subscriberDevice/{id}` on an owned device successfully unclaims the device; calling on a foreign device receives `HTTP 403 ACCESS_DENIED`.

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
|    - Verify client-supplied identity parameters are ignored.            |
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
| 6. Mutation Allowlists & Schema Enforcement:                            |
|    - Verify top-level device allowlist (ssid, wifiPassword, parental).  |
|    - Verify administrative fields return 403 ACCESS_DENIED.             |
|    - Verify unknown top-level fields return 400 Bad Request.            |
|    - Verify nested Wi-Fi allowlist (ssid, passphrase, encryption).      |
|    - Verify network: "management" and unknown keys return 400.          |
|                                                                         |
| 7. Profile & Venue Boundaries:                                          |
|    - Verify GET /subscriber response redaction (notes/tags stripped).   |
|    - Verify prohibited query parameters on GET /subscriber return 403.  |
|    - Verify PUT /subscriber permits only operation=updateMac.           |
|    - Verify bare DELETE /subscriber enforces device-count constraint.   |
|    - Verify parameterized DELETE /subscriber/{id} returns 403.          |
|    - Verify venue POST and DELETE return 403.                           |
|                                                                         |
| 8. Registration vs. Self-Service Separation:                            |
|    - Verify POST /subscriber requires internal X-API-KEY.               |
|    - Verify POST /subscriber does not require subscriber Bearer token.  |
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
5. **Anti-TOCTOU Integrity**: Concurrency controls MUST guarantee that a race condition cannot permit a subscriber to claim, access, or mutate a resource that belongs to or has transitioned to another owner.

---

## 14. Non-Goals

The following items are explicit non-goals for Issue #90:
1. **No Operator-Based Subscriber Authorization**: The system will not evaluate operator identity or operator hierarchies for subscriber self-service.
2. **No Tenant / Multi-Tenant Isolation Model**: Multi-tenant isolation concepts (`CallerTenant`, tenant equality) are completely removed from subscriber authorization.
3. **No Synthetic Subscriber Management Roles**: Subscribers will not receive entries in `RolesDB` or `PolicyDB`.
4. **No Administrative RBAC Alteration**: Semantics of `FindAllUserRoles()`, `PolicyAllows()`, and `ManagementRole` will not be altered.
5. **No Standalone Configuration Access**: Subscribers will not be granted access to `/api/v1/configuration*`.
6. **No ORM or Database Framework Overhaul**: Broad ORM refactoring and database schema changes are deferred.
7. **No General Caching or Performance Redesign**: Platform-wide caching frameworks and general query optimization are deferred.
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

