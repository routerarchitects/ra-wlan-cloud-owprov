# Hierarchical RBAC and Scoped Access Requirements

**Document Version:** 2.1  
**Status:** Requirements & Architecture Baseline  
**Target Service:** OpenWifi Provisioning Service (`owprov`)  
**Organization:** Router Architects  

---

## 1. Purpose and Guiding Principles

### 1.1 Purpose
This document specifies the required entity hierarchy, policy administration, Management Role assignment, delegation model, runtime authorization, scoped data visibility, Operator tenancy isolation, and IAM user ownership requirements for the OpenWifi Provisioning Service (`owprov`).

### 1.2 Core Architectural Principles
1. **Authoritative Backend Enforcement:** The backend is solely responsible for enforcing all identity, ownership, scope, permission, precedence, and data visibility rules. Client-side filtering or frontend UI disabling is not an authorization control.
2. **Explicit Allow-List Model:** Access is denied by default. Every protected operation requires an explicit grant defined within an active Management Policy associated with an effective Management Role covering the exact target scope.
3. **No Ambient Authority:** A user possesses permissions only within the context of an effective scope (Entity or Venue). Permissions held on one scope never grant rights on unrelated sibling or child entity scopes.
4. **Target Context Authoritativeness:** Runtime authorization resolves the exact entity and venue target before dispatching requests. If an exact venue role is matched, it is strictly authoritative and **shadows** higher-level entity roles without fallback or permission aggregation.
5. **Exact Scope Key Uniqueness:** A user may possess at most one active Management Role for any unique exact scope key `(User ID, Entity ID, Venue ID)`. Duplicate assignments on the same scope update the policy ID (upsert semantics).

---

## 2. Actors and Identity Model

The provisioning service integrates with the OpenWifi Security Service (`owsec`) as the authoritative source of identity, authentication tokens, and user metadata.

### 2.1 Actor Taxonomy

| Actor Type | Source Authority | Scope & Privileges | Limitations & Boundary Controls |
| :--- | :--- | :--- | :--- |
| **ROOT** | `owsec` (`userRole: "root"`) | Globally privileged system superuser. Can manage all policies, roles, entities, venues, operators, subscribers, configurations, and devices across all tenants. | Bypasses Management Role checks, scope precedence, delegation comparison, and target-user ownership checks. |
| **Admin** | `owsec` (`userRole: "admin"`) | Delegated administrative identity. Can manage IAM users and assign Management Roles within their assigned scope hierarchy. | May only manage and assign roles to IAM users whose creator ID (`createdBy`) matches this Admin's User ID. Cannot delegate permissions or scopes exceeding their own effective authority. Self-assignment, self-modification, and self-deletion of roles are strictly prohibited. |
| **Other Authenticated Users** | `owsec` (`installer`, `csr`, `noc`, `accounting`, etc.) | Functional operators executing assigned tasks. | Can read policies globally. Cannot create, modify, or delete policies. Cannot perform user management or client-requested role assignment. Can execute protected resources only through effective roles. |

### 2.2 Actor Authorization Invariants
- **ROOT Identity vs. Root Entity Assignment:** The `ROOT` system role is a global identity property asserted by security tokens. Assigning an ordinary user to the Root Entity does **not** make that user `ROOT`. Non-root users assigned to the Root Entity remain strictly constrained by Management Policy grants and delegation rules.
- **Admin Policy Name Neutrality:** A policy named `"admin"` or `"Admin"` is only a configurable collection of permissions. Its name has no special authorization meaning and must never trigger privileged conditional logic.
- **Creator Ownership Enforcement via OWSEC:** IAM user tenancy and creator ownership are fundamentally enforced by the security service (`owsec`). When an Admin lists, searches, or retrieves users, `owsec` returns only those users created by that Admin (`createdBy == Requester.id`). In `owprov`, Management Role assignment requires verifying the target user against `owsec`. An Admin can only create, update, or delete roles for users created by that Admin (`TargetUser.createdBy == Requester.id`). An Admin cannot assign, modify, or delete their own Management Roles; an Admin's roles may only be provisioned, altered, or removed by their creator (ROOT or parent Admin).
- **Privilege Escalation Prevention:** Non-root users (including Admins) are strictly blocked from creating, modifying, or elevating any account to the `root` role.

---

## 3. Core Concepts and Hierarchy Model

```
                         +-----------------------+
                         |      Root Entity      |
                         |  (0000-0000-0000)     |
                         +-----------------------+
                                    |
                 +------------------+------------------+
                 |                                     |
                 v                                     v
     +-----------------------+             +-----------------------+
     |    Operator Entity    |             |     Normal Entity     |
     | (1:1 with Operator)   |             +-----------------------+
     +-----------------------+                         |
                 |                                     |
                 +------------------+                  v
                 |                  |           +---------------+
                 v                  v           |  Top Venue    |
     +-----------------------+ +----------+     +---------------+
     |     Normal Entity     | |Top Venue |            |
     +-----------------------+ +----------+            v
                 |                  |           +---------------+
                 v                  v           |  Child Venue  |
           +----------+       +----------+      +---------------+
           |Top Venue |       |ChildVenue|
           +----------+       +----------+
```

### 3.1 Structural Primitives
- **Entity:** An administrative organizational boundary. Supported types:
  - **Root Entity:** The singular apex root of the system hierarchy (constant identifier `0000-0000-0000`).
  - **Operator Entity:** A tenant boundary associated one-to-one and created atomically with an Operator record.
  - **Normal Entity:** An organizational sub-unit created beneath the Root Entity or an Operator Entity.
- **Venue:** A physical or logical deployment location owned by an Entity. Venues may be nested beneath parent Venues to form location trees.
- **Operator:** A business or multi-tenant carrier record that binds directly to an Operator Entity.
- **Management Policy:** A discrete, reusable specification of allow-rules assigning atomic permissions (`READ`, `CREATE`, `UPDATE`, `DELETE`, `FULL`) across target resources.
- **Management Role:** An association linking exactly one User ID to exactly one Management Policy over an exact scope (Entity ID and optional Venue ID).

### 3.2 Scope Definitions
- **Entity Scope:** An assignment targeting an Entity ID with no Venue ID (`venue == ""`). Covers the assigned Entity and all direct and nested Venues owned by that Entity. It **excludes** child Entities and any Venues owned by those child Entities.
- **Venue Scope:** An assignment targeting an Entity ID and an exact Venue ID (`venue != ""`). Strictly covers that single Venue. It **excludes** parent Venues, sibling Venues, and descendant child Venues.

---

## 4. Entity and Venue Hierarchy Rules

### 4.1 Hierarchy Tree Structure and Level Constraints
1. **Three Entity Types:** Exactly three entity types are supported: `Root`, `Operator`, and `Normal`.
2. **Maximum Entity Depth:** Entity depth must not exceed 3 levels:
   - Level 1: Root Entity (`0000-0000-0000`)
   - Level 2: Operator Entity OR Normal Entity directly beneath Root
   - Level 3: Normal Entity directly beneath an Operator Entity
3. **Disallowed Entity Nesting:** 
   - A Normal Entity **cannot** exist beneath another Normal Entity.
   - An Operator Entity **cannot** exist beneath an Operator Entity or Normal Entity.
   - An Entity **cannot** exist beneath a Venue.
4. **Venue Nesting Depth:** Venues support unlimited nesting depth beneath any Entity. Every nested Venue remains owned by the top-level Entity that anchors the branch.

### 4.2 Hierarchy Lifecycle and Mutation Constraints

| Operation | Target Object | Authorization Requirement | Structural Validation Rules |
| :--- | :--- | :--- | :--- |
| **Create** | Root Entity | System Initialization Only | Cannot be created via API. System boots with fixed Root ID `0000-0000-0000`. |
| **Create** | Operator Entity | `Operator:CREATE` on Root Scope | Cannot be created through the generic Entity API. Created atomically with an Operator record. |
| **Create** | Normal Entity | `Entity:CREATE` on Parent Entity | Parent must exist and be Root Entity or Operator Entity. |
| **Create** | Top-Level Venue | `Venue:CREATE` on Owning Entity | Request specifies `entity` and leaves `parent` empty. |
| **Create** | Child Venue | `Venue:CREATE` on Exact Parent Venue | Request specifies `parent` Venue ID and leaves `entity` empty. Inherits owning `entity` from parent venue. |
| **Update** | Entity | `Entity:UPDATE` on Exact Entity | `parent` field is strictly **immutable**. Type cannot be converted. |
| **Update** | Venue | `Venue:UPDATE` on Exact Venue | `parent` and `entity` references are strictly **immutable**. |
| **Delete** | Entity / Venue | `Entity:DELETE` / `Venue:DELETE` | Dependency check: Must have zero children, devices, contacts, locations, configs, and subscribers. Returns `StillInUse` on failure. |

### 4.3 Parent Immutability Rule
Once an Entity or Venue is created, its parent reference is permanent:
- Updating an Entity attempting to modify its `parent` is rejected.
- Updating a Venue attempting to modify its `parent` or owning `entity` is rejected.
- Structural reparenting across trees is prohibited. Changing a branch requires recreating nodes at the target destination.

### 4.4 Self-Reference and Cycle Prevention
Venue creation validation must strictly verify that a venue cannot reference itself as parent (`parent == id`) and that parent references do not form circular ancestor chains.

---

## 5. Management Policy Administration

### 5.1 Access Rules
Policies define resource allow-lists. Policy endpoints are governed by strict CRUD constraints:

| Operation | ROOT | Admin | Other Users |
| :--- | :---: | :---: | :---: |
| **Create Policy** | Allowed | Denied | Denied |
| **List Policies** | Allowed | Allowed (Global) | Allowed (Global) |
| **Get Policy** | Allowed | Allowed (Global) | Allowed (Global) |
| **Update Policy** | Allowed | Denied | Denied |
| **Delete Policy** | Allowed | Denied | Denied |

- **Global Read Exception:** Authenticated users of any role can read policy definitions globally. Policy reads are not filtered by assigned scope so users can inspect role capabilities.
- **In-Use Protection:** A policy referenced by one or more active Management Roles **cannot** be deleted. Attempting deletion of an in-use policy returns `StillInUse` error and preserves the policy.
- **Database Referential Integrity:** Deletion restriction is backed by database foreign key constraints (`ON DELETE RESTRICT`) preventing orphaned role references.

### 5.2 Supported Resources
The policy allow-list enforces permissions across the following canonical resource types:

| Canonical Resource | Covered Areas & Managed Components |
| :--- | :--- |
| `entity` | Organizational entities, hierarchy management |
| `venue` | Venue locations, venue topologies, venue actions |
| `inventory` | Device inventory tags, subscriber devices, configuration overrides |
| `configuration` | Device configuration profiles and templates |
| `operator` | Tenant operator profiles, carrier service classes |
| `subscriber` | Subscriber user accounts and signup records |
| `contact` | Entity and venue contact records |
| `location` | Entity and venue geographic location records |
| `managementRole` | Management role assignments |
| `managementPolicy`| Management policy administration |

### 5.3 Permission Semantics and Expansion
Policies are allow-lists. Missing grants are denied. Permissions expand into atomic grants:

| Permission Name | Atomic Grants Included | Supported Operations |
| :--- | :--- | :--- |
| `FULL` | `READ`, `CREATE`, `UPDATE`, `DELETE` | Read, Create, Update, Delete |
| `CREATE` | `READ`, `CREATE` | Read, Create |
| `UPDATE` | `READ`, `UPDATE` | Read, Update |
| `DELETE` | `READ`, `DELETE` | Read, Delete |
| `READ` | `READ` | Read |

- **Canonical Naming:** `UPDATE` is the canonical update permission name. Legacy synonyms (`MODIFY`, `READWRITE`) are supported during evaluation but normalized internally.
- **Atomic Subset Evaluation:** When validating delegation, every atomic grant in the target policy must exist in the assigner's effective policy for that resource.

---

## 6. Management Roles

### 6.1 Role Structure and Uniqueness
A Management Role associates a user with a policy over an exact scope:

```json
{
  "id": "uuid",
  "name": "role-name",
  "description": "role-description",
  "entity": "entity-uuid",
  "venue": "venue-uuid-or-empty",
  "managementPolicy": "policy-uuid",
  "users": ["user-uuid"]
}
```

#### Exact Scope Uniqueness Invariant
For any given user, only one role may exist per exact scope key:
$$\text{Scope Key} = \langle \text{User ID}, \text{Entity ID}, \text{Venue ID} \rangle$$
- **Entity Scope Role:** `Venue ID` is empty (`""`).
- **Venue Scope Role:** `Venue ID` is populated (`<venue-uuid>`).
- **Upsert Semantics:** Submitting an assignment for an existing exact scope updates the `managementPolicy` ID and timestamp on the existing record rather than creating duplicate records.

### 6.2 Management Role Provisioning APIs

#### Single-Scope Role Provisioning (V1 API)
- Targets a single scope per request.
- Request payload provides `entity` and optional single `venue` string.
- Returns a single Management Role object.

#### Multi-Venue Batch Provisioning (V2 API)
- Designed for atomic, bulk multi-venue role provisioning.
- Request payload accepts `entity` and an array of venue identifiers (`venueIds: ["uuid1", "uuid2"]`).
- When `venueIds` is omitted or empty (`[]`), creates or upserts one Entity-wide role (`venue = ""`).
- When `venueIds` contains $K$ venue identifiers, creates or upserts $K$ individual Venue-scoped roles. It does **not** create an Entity-scoped role.
- **Response Envelope:** Always returns an envelope containing all created/updated roles:
  ```json
  {
    "roles": [
      { "id": "...", "entity": "E1", "venue": "V1", "managementPolicy": "P1", "users": ["U1"] },
      { "id": "...", "entity": "E1", "venue": "V2", "managementPolicy": "P1", "users": ["U1"] }
    ]
  }
  ```
- **Transaction Atomicity:** Multi-venue assignment is all-or-nothing. If any venue validation or write fails, all newly created roles in the batch are rolled back and existing roles restored.

### 6.3 Management Role Immutability
Once created, a Management Role's scope and subject are immutable:
- `entity`, `venue`, `venueIds`, and `users` **cannot** be modified.
- Any update request attempting to modify these fields is rejected with an error indicating that Entity ID, Venue ID, and User ID are immutable.
- An update operation may **only** change the `managementPolicy` ID.
- To reassign a user or move scope, the existing role must be deleted and a new role created at the target scope.

### 6.4 Role Lifecycle Permissions Matrix

| Operation | ROOT | Admin | Other Users |
| :--- | :---: | :---: | :---: |
| **List Roles** | All Roles | Roles of users created by Admin | Own Roles Only |
| **Get Role** | Any Role | Roles of users created by Admin | Own Roles Only |
| **Create / Upsert Role** | Allowed | Allowed only for created users, requiring scope authority, effective `managementRole:CREATE` grant, and delegation subsetting; self-assignment prohibited | Denied |
| **Update Policy** | Allowed | Allowed only for created users, requiring scope authority, effective `managementRole:UPDATE` grant, and delegation subsetting; self-modification prohibited | Denied |
| **Delete Role** | Allowed | Allowed only for created users, requiring scope authority and effective `managementRole:DELETE` grant over exact scope; self-deletion prohibited | Denied |

---

## 7. Controlled Delegation and Creator Ownership

### 7.1 IAM Creator Ownership Rule and OWSEC Enforcement
- **Authoritative Source of Identity:** `owsec` is the authoritative source for IAM user identities, user listing, and creator ownership (`createdBy`).
- **OWSEC User Isolation:** At the identity layer, `owsec` enforces strict tenancy isolation: when an Admin lists or queries users, `owsec` returns only those users created by that Admin (`createdBy == Requester.id`). Users created by other Admins or by ROOT are never visible or returned to that Admin.
- **Backend Verification Flow in `owprov`:** When an Admin creates, updates, or deletes a Management Role for a target user:
  1. `owprov` queries `owsec` to fetch the target user's identity record. If the user does not exist or is inaccessible in `owsec`, the request fails immediately with *"The selected user could not be found."*
  2. For non-root requesters, `owprov` strictly validates creator ownership:
     $$\text{TargetUser.createdBy} == \text{Requester.id}$$
  3. **Self-Assignment and Self-Modification Prohibited:** An Admin is strictly forbidden from assigning, modifying, or deleting their own Management Roles (`TargetUser.id == Requester.id` is not permitted). An Admin's own roles may only be provisioned, altered, or deleted by the higher-tier identity that created them (ROOT or a parent Admin). This prevents irreversible privilege self-demotion, permanent self-lockouts, and unauthorized self-governance.
  4. Attempting to target any user not created by that Admin (including the Admin themselves) is rejected with:
     > *"You are not authorized to assign or modify roles for users you did not create."*

### 7.2 Five-Pillar Cumulative Authorization Model
For any client-requested Management Role operation (create, update, delete), the system enforces a strict **five-pillar cumulative authorization requirement**. All five conditions are mandatory and must be satisfied concurrently. If any single condition fails, the operation is immediately denied:

1. **Pillar 1 — Requester Actor Identity:** The requester must be authenticated as `ROOT` or an `owsec` `Admin`. Standard functional users (`csr`, `installer`, `noc`, etc.) are unconditionally denied role-management endpoints.
2. **Pillar 2 — Target-User Creator Ownership:** For non-root requesters, the target user must have been created directly by the requester (`TargetUser.createdBy == Requester.id`).
3. **Pillar 3 — Scope Authority:** The requester must hold an active Management Role governing the exact target scope (either an exact Venue-scoped role for venue assignments or an Entity-scoped role covering that entity and its direct/nested venues).
4. **Pillar 4 — Effective `managementRole` Operation Grant:** The requester's effective policy on that target scope must explicitly grant the required role-lifecycle permission:
   - For creating or upserting roles: `managementRole:CREATE` (or `entity:CREATE`, `operator:CREATE`, or `*:FULL`).
   - For updating role policies: `managementRole:UPDATE` (or `entity:UPDATE`, `operator:UPDATE`, or `*:FULL`).
   - For deleting roles: `managementRole:DELETE` (or `entity:DELETE`, `operator:DELETE`, or `*:FULL`).
   *Crucial distinction:* Holding administrative permissions on underlying functional resources (such as `inventory:FULL` or `configuration:FULL`) does **not** grant the ability to manage roles. Role management requires an explicit `managementRole` (or enclosing entity) operational grant.
5. **Pillar 5 — Delegated Policy Subsetting:** For role creation and policy updates, every resource grant defined in the target policy must be an equal or weaker subset of the permissions the assigner effectively holds on that target scope.

### 7.3 Permission Subsetting Rules
An Admin cannot delegate permissions exceeding their own effective authority:
1. **Scope Authority:** The Admin must hold an active role that covers the target scope (Entity or Venue).
2. **Permission Coverage:** For every resource and atomic grant requested in the target policy, the Admin must hold an equal or stronger grant on that exact scope.
3. **Delegation Scope Boundaries:**
   - An Entity-scoped role can delegate roles for that Entity or any Venue owned by that Entity.
   - An Entity-scoped role **cannot** delegate roles for a child Entity or venues owned by a child Entity.
   - A Venue-scoped role can delegate **only** to that exact Venue. It cannot delegate to child venues or parent entities.
4. **Rejection:** If any requested permission or scope exceeds the Admin's held authority, the assignment is rejected with:
   > *"Cannot assign permissions stronger than held: missing '<ACCESS>' on '<RESOURCE>'."*

---

## 8. Runtime Authorization and Resolution Logic

Every HTTP request to a protected endpoint passes through the runtime authorization pipeline before execution.

```
Incoming Request (User, Method, Path, Params, Body)
                         |
                         v
              Is User ROOT? ---------------------------> [ALLOW]
                         | No
                         v
          Resolve Target Context (Path / Params / Body)
                         |
              +----------+----------+
              |                     |
      Target Venue Set?      Target Entity Set?
              |                     |
              v                     v
    Find Exact Venue Role?   Find Entity Role?
       |             |              |
     Yes             No             v
       |             |       Policy Allows?
       v             v        |          |
  Policy Allows?   Find Entity Role?  Yes   No
   |         |       |         |       |     |
  Yes        No     Yes        No      v     v
   |         |       |         |    [ALLOW] [DENY]
   v         v       v         v
[ALLOW]   [DENY] Policy Allows? [DENY]
                   |         |
                  Yes        No
                   |         |
                   v         v
                [ALLOW]   [DENY]
```

### 8.1 Target Scope Resolution Logic

The runtime authorization engine maps each incoming request to an authoritative target scope (`TargetEntity` and `TargetVenue`). The scope resolution strategy differs fundamentally between operations on existing resources (`GET`, `PUT`, `DELETE`) and resource creation operations (`POST`):

#### 8.1.1 Resolution for Existing Resources (`GET`, `PUT`, `DELETE`)
For operations targeting existing objects, the target scope is derived from the stored record:
1. **Bound Object ID in URL Path:**
   - The engine extracts the object identifier (`id`, `uuid`, or `serialNumber`) from the URL path bindings.
   - A verified database lookup is performed against the target resource storage (Entity, Venue, Inventory Tag, Configuration, Role, Operator, etc.).
   - The stored owning Entity and Venue IDs are extracted directly from the persistent record to establish `TargetEntity` and `TargetVenue`.
   - If the path ID does not exist in storage, resolution fails immediately, blocking unauthorized operations on missing or unverified objects.

#### 8.1.2 Resolution for Resource Creation (`POST`)
For creation operations, the resource being created does not exist in the database yet:
1. **Creation Placeholder Bypass in URL Path:**
   - In creation endpoints (e.g., `POST /api/v1/entity/{id}` or `POST /api/v1/venue/{id}`), the URL path `{id}` contains a creation placeholder (`"0"`, `"0000-0000-0000"`, or `"new"`).
   - The engine explicitly bypasses the URL path binding during creation lookups rather than attempting a database fetch against a non-existent ID.
2. **Destination Scope Resolution from Request Payload:**
   - The authorization target scope is resolved strictly from the **destination parent/owning container** specified in the request payload:
     - **Entity Creation (`POST /api/v1/entity/{id}`):** Resolves the target scope from `parent` in the request body. Authorization requires `entity:CREATE` on that parent Entity scope.
     - **Top-Level Venue Creation (`POST /api/v1/venue/{id}`):** Resolves `entity` in the request body. Authorization requires `venue:CREATE` on that parent Entity scope.
     - **Child Venue Creation (`POST /api/v1/venue/{id}`):** Resolves `parent` Venue in the request body. Authorization requires `venue:CREATE` on that exact parent Venue scope.
     - **Operator Creation (`POST /api/v1/operator/{id}`):** Operates on the global Root Entity scope. Authorization requires `operator:CREATE` on the Root Entity (`0000-0000-0000`).
     - **Inventory Registration (`POST /api/v1/inventory/{id}`):** Resolves target destination `venue` or `entity` from the payload.
     - **Management Role Assignment (`POST /api/v1/managementRole/{id}` or `/api/v2/...`):** Resolves target `entity` and `venue` (or `venueIds`) from the request payload.

#### 8.1.3 Conflict Validation and Disambiguation (No Silent Overrides)
To prevent security discrepancies where a request might be authorized against one scope but written to another:
- If a client supplies scope identifiers across multiple sources (e.g. query parameter `?entity=A` alongside a JSON body with `parent=B`):
  1. All supplied identifiers must be verified for existence against stored records.
  2. The resolved scopes across parameters and body must be mutually consistent.
  3. Any conflicting or contradictory scope identifiers **MUST be rejected with `400 Bad Request`** rather than silently selecting or prioritizing one source over the other.

If a scope constraint applies to the resource and resolution fails, the request is immediately denied.

### 8.2 Effective Role Precedence and Venue Shadowing
When authorizing operations targeting a Venue or a device within a Venue:

1. **Exact Venue Precedence:** The engine checks all active roles assigned to the user for an exact match where:
   $$\text{role.venue} == \text{TargetVenue}$$
2. **Authoritative Shadowing:** If an exact Venue role exists, it is the **sole effective role**.
   - If the exact Venue role grants the required permission, access is **allowed**.
   - If the exact Venue role lacks the required permission, access is **denied**.
   - The engine **must not** fall back to the owning Entity role.
   - The engine **must not** combine permissions across multiple roles.
3. **Entity Fallback:** If and only if no exact Venue role exists, the engine checks for an Entity-scoped role where:
   $$\text{role.entity} == \text{TargetEntity} \quad \land \quad \text{role.venue} == ""$$
   - Confirms that `TargetVenue` is a direct or nested venue owned by `TargetEntity`.
   - Evaluates the policy of the Entity role.
4. **Denial:** If neither an exact Venue role nor an owning Entity role matches, access is **denied**.

### 8.3 Automatic Creator-Role Assignment
When an authorized non-ROOT user creates a new Entity or Venue:
- The system automatically creates a Management Role for the creator on the newly created scope.
- **Policy Inheritance:** Copies the exact Management Policy ID of the effective parent-scope role that authorized the creation.
- **Atomicity:** The new scope and creator role are committed in the same atomic transaction. If role creation fails, the entire creation transaction is rolled back.
- **Bypass:** As a system-managed operation, automatic creator-role assignment is not subject to Admin creator-ownership restrictions.

---

## 9. Scoped Data Visibility and Tenancy Isolation

Visibility rules govern collection listing, searching, individual record lookups, counts, exports, and hierarchy navigation.

### 9.1 Entity Visibility and Tree Structure
- An Entity is visible to a non-ROOT user if and only if the user holds an exact Entity-scoped role granting `entity:READ`.
- **Tree Navigation:** In hierarchy tree queries and navigation breadcrumbs, non-root users see trees rooted at their highest visible scope. Unassigned ancestors and siblings are hidden.

### 9.2 Venue Visibility
- Venue visibility is evaluated independently for each exact Venue.
- An exact Venue-scoped role shadows the owning-Entity role for `venue:READ`.
- If an exact Venue role lacks `venue:READ`, that Venue is hidden even if the owning Entity role grants `venue:READ`.
- A child Venue may be visible even when its parent Venue is unassigned or hidden.

### 9.3 Inventory and Device Visibility
A device is visible if and only if both applicable scope and `inventory:READ` permit access:

| Assignment Type | Visible Devices |
| :--- | :--- |
| **ROOT** | All devices across all tenants. |
| **Entity Scope Role (`inventory:READ`)** | Devices in direct and nested Venues of that Entity, **excluding** venues shadowed by an exact Venue role lacking `inventory:READ`. |
| **Exact Venue Role (`inventory:READ`)** | Devices in that exact Venue only. |
| **Shadowed Venue (Exact role lacks `inventory:READ`)** | Devices in that Venue remain hidden, even if the Entity role grants `inventory:READ`. |
| **Unassigned Scope** | Zero devices visible. |

### 9.4 Operator Tenancy Isolation
Operator tenant data is strictly isolated:
1. **Operator Visibility Requirements:**
   - User must hold an Entity-scoped role directly on that Operator's associated Operator Entity (`role.entity == Operator.entityId` with `role.venue == ""`).
   - The role's policy must grant `operator:READ`.
2. **Isolation Invariants:**
   - An assignment on the Root Entity does **not** reveal Operators.
   - An assignment on a Normal Entity does **not** reveal Operators.
   - A Venue-scoped role does **not** reveal Operators, even if the venue belongs to the Operator Entity.
3. **Operator Sub-Resources:**
   - **Operator Details:** Requires `operator:READ`.
   - **Service Classes:** Scoped strictly to the Operator. Cross-operator access is blocked.
   - **Operator Devices and Subscribers:** Candidate devices must belong to subscribers of that Operator and pass individual venue scope and `inventory:READ` checks.

---

## 10. Deletion Dependencies and Structural Validation

Deletion requires both permission authorization and structural validation.

### 10.1 Structural Deletion Pre-conditions (`StillInUse`)
A resource cannot be deleted if it is referenced by or contains dependent records:

| Resource Type | Blocking Dependencies | Result on Failed Dependency |
| :--- | :--- | :--- |
| **Root Entity** | Root Entity cannot be deleted under any circumstances. | Rejected (`CannotDeleteRoot`) |
| **Operator Entity** | Cannot be deleted through generic Entity deletion. Must delete Operator. | Rejected (`StillInUse`) |
| **Normal Entity** | Child entities, venues, devices, contacts, locations, configs, roles. | Rejected (`StillInUse`) |
| **Venue** | Child venues, devices, or active subscriber bindings. | Rejected (`StillInUse`) |
| **Operator** | Active subscribers or attached entity dependencies. | Rejected (`StillInUse`) |
| **Management Policy** | Referenced by one or more active Management Roles. | Rejected (`StillInUse`) |

### 10.2 Cascade Cleanup on Deletion
When a structurally eligible scope or user is deleted, associated authorization assignments are cleaned up:
- **Entity Deletion:** Removes all Management Roles associated with that Entity ID.
- **Venue Deletion:** Removes all Management Roles associated with that Venue ID.
- **Cache Invalidation:** All affected authorization caches are invalidated immediately.
- **User Deletion:** All Management Roles assigned to that User ID are deleted.
- **Admin Deletion:** All Management Roles assigned to that Admin user are deleted. Users created by that Admin become orphan users until reassigned by ROOT.

---

## 11. REST API Specification and Scoped Resolution Matrix

| HTTP Method | API Endpoint | Required Permission | Resource Scope Resolution | Description |
| :--- | :--- | :--- | :--- | :--- |
| `GET` | `/api/v1/entity` | `entity:READ` | Scoped list | Lists entities visible to user |
| `GET` | `/api/v1/entity/{id}` | `entity:READ` | Target Entity `{id}` | Gets entity details |
| `POST` | `/api/v1/entity/{id}` | `entity:CREATE` | Parent Entity (`parent` in body; `{id}` is placeholder `0`) | Creates normal entity under parent |
| `PUT` | `/api/v1/entity/{id}` | `entity:UPDATE` | Target Entity `{id}` | Updates entity details |
| `DELETE` | `/api/v1/entity/{id}` | `entity:DELETE` | Target Entity `{id}` | Deletes entity (validates StillInUse) |
| `GET` | `/api/v1/venue` | `venue:READ` | Scoped list | Lists venues visible to user |
| `GET` | `/api/v1/venue/{id}` | `venue:READ` | Target Venue `{id}` | Gets venue details |
| `POST` | `/api/v1/venue/{id}` | `venue:CREATE` | Parent Scope (`parent` venue or `entity` in body; `{id}` is placeholder `0`) | Creates top-level or child venue |
| `PUT` | `/api/v1/venue/{id}` | `venue:UPDATE` | Target Venue `{id}` | Updates venue details |
| `DELETE` | `/api/v1/venue/{id}` | `venue:DELETE` | Target Venue `{id}` | Deletes venue (validates StillInUse) |
| `GET` | `/api/v1/inventory` | `inventory:READ` | Scoped list | Lists inventory devices visible to user |
| `GET` | `/api/v1/inventory/{id}` | `inventory:READ` | Device's Venue / Entity | Gets device details |
| `POST` | `/api/v1/inventory/{id}` | `inventory:CREATE` | Destination `venue` or `entity` in body | Registers inventory tag |
| `PUT` | `/api/v1/inventory/{id}` | `inventory:UPDATE` | Target Venue / Entity | Updates inventory tag |
| `DELETE` | `/api/v1/inventory/{id}` | `inventory:DELETE` | Target Venue / Entity | Deletes inventory tag |
| `GET` | `/api/v1/managementPolicy` | Authenticated | Global | Lists management policies |
| `GET` | `/api/v1/managementPolicy/{uuid}` | Authenticated | Global | Gets policy details |
| `POST` | `/api/v1/managementPolicy/{uuid}` | ROOT Only | Global | Creates management policy |
| `PUT` | `/api/v1/managementPolicy/{uuid}` | ROOT Only | Global | Updates management policy |
| `DELETE` | `/api/v1/managementPolicy/{uuid}` | ROOT Only | Global | Deletes policy (checks in-use) |
| `GET` | `/api/v1/managementRole` | Scoped List | User Roles / Created Users | Lists management roles |
| `GET` | `/api/v1/managementRole/{id}` | Scoped Read | Target Role Scope | Gets role details |
| `POST` | `/api/v1/managementRole/{id}` | ROOT / Admin Delegation | Target Entity / Venue Scope (in body; `{id}` is placeholder `0`) | V1: Creates/upserts single role |
| `POST` | `/api/v2/managementRole/{id}` | ROOT / Admin Delegation | Target Entity / `venueIds` (in body; `{id}` is placeholder `0`) | V2: Multi-venue batch role allocation |
| `PUT` | `/api/v1/managementRole/{id}` | ROOT / Admin Delegation | Target Role Scope | Updates policy ID on existing role |
| `PUT` | `/api/v2/managementRole/{id}` | ROOT / Admin Delegation | Target Role Scope | Updates policy ID on existing role |
| `DELETE` | `/api/v1/managementRole/{id}` | ROOT / Admin Deletion | Target Role Scope | Deletes role |
| `DELETE` | `/api/v2/managementRole/{id}` | ROOT / Admin Deletion | Target Role Scope | Deletes role |
| `GET` | `/api/v1/operator` | `operator:READ` | Target Operator Entity | Lists operators visible to user |
| `GET` | `/api/v1/operator/{id}` | `operator:READ` | Target Operator Entity | Gets operator details |
| `POST` | `/api/v1/operator/{id}` | `operator:CREATE` | Root Entity Scope (`{id}` is placeholder `0`) | Creates operator and operator entity |
| `PUT` | `/api/v1/operator/{id}` | `operator:UPDATE` | Target Operator Entity | Updates operator details |
| `DELETE` | `/api/v1/operator/{id}` | `operator:DELETE` | Target Operator Entity | Deletes operator and operator entity |

---

## 12. Security and Data Integrity Invariants

1. **Information Leakage Prevention:** Responses for unauthorized accesses must return generic error codes (`ACCESS_DENIED`, `NotFound`, or `BadRequest`) to avoid disclosing the existence or counts of inaccessible resources to unprivileged users.
2. **Atomic Rollback Guarantee:** In multi-venue batch role provisioning, any validation or write failure during iteration rolls back all previous changes in the batch to maintain a consistent state.
3. **Cache Synchronization:** To prevent stale permission windows, any successful modification to a role, policy, user, entity, or venue invalidates all runtime authorization caches before the response is returned to the client.
