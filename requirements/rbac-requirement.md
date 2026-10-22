# Hierarchical RBAC and Scoped Access Requirements Specification

**Version:** 2.1  
**Status:** Requirements Baseline & Implementation Architecture  
**Organization:** Router Architects / Mango Cloud  
**Service:** `ra-wlan-cloud-owprov` (OpenWiFi Provisioning Service)  
**Reference Document:** `hierarchical-rbac-scoped-access-requirements-v2.1.docx`

---

## 1. Purpose & Scope

This specification defines the mandatory identity hierarchy, policy administration, Management Role assignment, delegation, authorization, scoped data visibility, Operator access, and IAM user ownership rules for the OpenWiFi Provisioning Service (`ra-wlan-cloud-owprov`).

The backend service is solely responsible for enforcing all identity, ownership, scope, permission, precedence, and data-visibility rules. **Frontend filtering is not an authorization control.**

This document bridges the formal requirements baseline with the live C++ backend architecture in `ra-wlan-cloud-owprov`.

---

## 2. Actors & Identity Model

### 2.1 ROOT
* **Identity Source:** Globally privileged OWSEC identity (`userinfo.userType == "root"`).
* **Authority:** Can manage all policies, users, Management Roles, entities, venues, operators, subscribers, devices, and configurations.
* **Bypass:** Bypasses all Management Role checks, scope precedence, target-user ownership, and delegation constraints.
* **Separation:** The ROOT identity is fundamentally distinct from an ordinary user assigned to the Root Entity. An entity role on the Root Entity does not grant ROOT system status.

### 2.2 Admin
* **Identity Source:** OWSEC identity type (`userinfo.userType == "admin"`).
* **User Ownership:** Can manage only IAM users whose `createdBy` value directly matches that Admin's User ID.
* **Role Delegation:** Can assign Management Roles only to users created by that Admin.
* **Cumulative Gates:** Target-user ownership (`createdBy`) and scope authority are cumulative requirements. Both must be satisfied.
* **Sub-delegation:** Can delegate only scopes and permissions equal to or lower than the Admin's effective authority.
* **Policy Name Independence:** A policy named `"Admin"` is solely a configurable set of permissions. Its name has no special authorization meaning and must never be used in conditional logic.

### 2.3 Other Authenticated Users
* Can read policies globally.
* Cannot modify policies.
* Cannot perform IAM user CRUD or client-requested Management Role assignments.
* Can perform protected operations only through an effective Management Role whose policy grants the required permission.

---

## 3. Core Concepts & Definitions

| Concept | Definition |
| :--- | :--- |
| **Entity** | A organizational tenant node: Root Entity, Operator Entity, or Normal Entity. |
| **Venue** | A physical location owned by an Entity, optionally nested beneath another Venue. |
| **Operator** | A business or tenant record associated one-to-one with an Operator Entity. |
| **Policy** | An allow-list collection of resource permissions (`resources` + `access`). |
| **Management Role** | A user-policy assignment for one exact Entity or Venue scope. |
| **Entity Scope** | The assigned Entity and all direct or nested Venues owned by that Entity. It strictly excludes child Entities and their Venues. |
| **Venue Scope** | One exact Venue. It strictly excludes parent, sibling, and descendant Venues. |

---

## 4. Entity and Venue Hierarchy

### 4.1 Entity Types & Depth Limits
The system supports exactly three Entity types and a maximum hierarchy depth of three Entity levels:
1. **Root Entity** (Level 1)
2. **Operator Entity** (Level 2)
3. **Normal Entity** (Level 2 or Level 3)

```text
Root Entity
├── Operator Entity (Level 2)
│   └── Normal Entity (Level 3)
└── Normal Entity (Level 2)
```

Venues may be nested to **unlimited depth** beneath any Entity type.

### 4.2 Structural Hierarchy Rules
* **Root Entity:** Exactly one exists. Created during backend initialization. Has no parent and cannot be created, duplicated, or deleted via APIs.
* **Operator Entity:** Exists only directly beneath the Root Entity. Cannot be created via the generic Entity API; created atomically upon Operator creation. Cannot exist beneath another Operator or Normal Entity.
* **Normal Entity:** Can exist directly beneath the Root Entity or an Operator Entity. Cannot exist beneath another Normal Entity.
* **Venues:** May exist beneath any Entity type or another Venue. Every nested Venue remains owned by the Entity that owns its top-level Venue branch (`venue.entity == parentEntityId`).
* **Parent Immutability:** Entity and Venue parent references (`parent`, `entity`) are immutable after creation. Creation validation prevents self-reference and cycles.

### 4.3 Creation, Update & Deletion Lifecycle Permissions
* **Entity Creation:** Creating a Normal Entity requires `entity:CREATE` on the parent Entity scope.
* **Operator Creation:** Requires `operator:CREATE` on the Root Entity scope. The Operator record and Operator Entity are created atomically.
* **Venue Creation:**
  - Creating a top-level Venue requires `venue:CREATE` on its parent Entity scope.
  - Creating a child Venue requires `venue:CREATE` through the effective role for its exact parent Venue.
* **Updates:** Updating an Entity requires `entity:UPDATE` on that exact Entity scope. Updating a Venue requires `venue:UPDATE` on that exact Venue scope.
* **Deletion:** Requires both structural validity (zero child entities, child venues, devices, configurations, roles, or in-use dependencies) and authorization (`entity:DELETE` or `venue:DELETE`). Failed dependency checks return `StillInUse` (HTTP 400).

---

## 5. Policy Administration & Permission Semantics

### 5.1 Permission Verbs & Atomic Grants
Policies are strict **allow-lists**. Missing grants are denied. The canonical update verb is `UPDATE` (`MODIFY` is not supported).

| Permission | Included Atomic Grants | Description |
| :--- | :--- | :--- |
| **`FULL`** | `READ`, `CREATE`, `UPDATE`, `DELETE` | Full administrative control over the resource. |
| **`CREATE`** | `READ`, `CREATE` | Create objects and read them. |
| **`UPDATE`** | `READ`, `UPDATE` | Update existing objects and read them. |
| **`DELETE`** | `READ`, `DELETE` | Delete objects and read them. |
| **`READ`** | `READ` | Read-only inspection. |

### 5.2 Supported Policy Resources
1. `entity`
2. `venue`
3. `configuration`
4. `inventory`
5. `operator`
6. `subscriber`
7. `managementRole` / `managementPolicy`

---

## 6. Management Role Assignment & Delegation

### 6.1 Request Structure & Exact Scope Keys
A Management Role request targets exactly one Entity:
* `userId`: Target user UUID.
* `policyId`: Management Policy UUID.
* `entity`: Entity UUID.
* `venue` (optional): List of Venue UUIDs belonging to the specified Entity.

**Scope Uniqueness Key:** For a given user, only one role may exist per exact scope:
* **Entity Scope Key:** `(userId, entityId, venue="")`
* **Venue Scope Key:** `(userId, entityId, venueId)`

Submitting an assignment for an existing exact scope updates its `policyId` (upsert semantics) rather than creating a duplicate.

### 6.2 Controlled Delegation Rules (Admin)
1. **Target-User Ownership:** Admin can assign/modify roles only for users where `user.createdBy == Admin.id`.
2. **Scope Authority:** Admin must hold effective authority over every target scope.
3. **Permission Subset:** The assigned policy's atomic grants must be a subset of the Admin's effective permissions for the target scope.
4. **Scope Boundaries:**
   - Entity-scoped role can delegate to the same Entity or a Venue owned by that Entity.
   - Entity-scoped role cannot delegate to child Entities or child-Entity Venues.
   - Venue-scoped role can delegate only to that exact Venue.
5. **Atomicity:** The assignment request is completely atomic. If any validation or write fails, no role is created or modified.

### 6.3 Automatic Creator-Role Assignment
When an authorized non-ROOT user creates an Entity or Venue:
* The backend automatically creates a Management Role for the creator on the exact new scope.
* It copies the complete `policyId` of the effective parent role that authorized creation.
* The new resource and the creator role are committed in the same database transaction.
* This system operation bypasses Admin identity and target-user ownership checks.

---

## 7. Runtime Authorization Engine: Scope & Effective Role Precedence

### 7.1 Scope Coverage
* **Entity-Scoped Role:** Covers the assigned Entity and all direct and nested Venues owned by that Entity. Does **not** cover child Entities or their Venues.
* **Venue-Scoped Role:** Covers **only the exact Venue**. Does not cover parent, sibling, or descendant Venues.

### 7.2 Effective Role Precedence Rule (Section 7.2)
When evaluating an operation on a target resource:

```text
Target Resource has Venue?
     │
     ├── YES ──► Does user have an exact Venue-scoped role for TargetVenue?
     │                │
     │                ├── YES ──► Exact Venue Role is the ONLY effective role.
     │                │           (Authoritative: NO fallback to Entity role.
     │                │            Permissions are NOT combined across roles.)
     │                │
     │                └── NO ───► Fall back to owning-Entity role.
     │                                 │
     │                                 ├── Role found? ──► Use Owning-Entity Role.
     │                                 └── Not found?  ──► DENY ACCESS.
     │
     └── NO  ──► Check exact Entity-scoped role.
                      ├── Role found? ──► Use Entity Role.
                      └── Not found?  ──► DENY ACCESS.
```

#### Precedence Principles:
1. **Exact-Venue Shadowing:** An exact Venue-scoped role **shadows** the owning-Entity role. If an exact Venue role exists, it is the sole effective role.
2. **No Fallback After Selection:** If the selected exact Venue role lacks the required permission, access is **denied**. The system must never fall back to the Entity role.
3. **No Cross-Scope Combination:** Permissions are not additive across different hierarchical scopes.
4. **Example:**
   - User holds `Role 1` on `Entity A` with `configuration:FULL`.
   - User also holds `Role 2` on `Venue A1` with `venue:READ` (lacks `configuration:READ`).
   - Access to configurations on `Venue A1` is **DENIED** because `Role 2` is the authoritative effective role for `Venue A1` and does not grant `configuration:READ`.

---

## 8. Scoped Data Visibility & Collection Filtering

### 8.1 Configurations & Variables Visibility (`GET /api/v1/configurations`, `GET /api/v1/variables`)
A non-ROOT user's list query must strictly partition records based on Section 7.2 precedence:

1. **Entity-Scoped Records (`venue IS NULL OR venue=''`):**
   - Visible if the user holds an Entity-scoped role for `record.entity` granting `configuration:READ` (or `variables:READ`).
2. **Venue-Scoped Records (`venue IS NOT NULL AND venue != ''`):**
   - If the user holds an exact Venue-scoped role for `record.venue`: visible only if that role grants `configuration:READ`. If the role lacks read permission, the venue is shadowed/denied.
   - If the user has **no** exact role on `record.venue`: visible if the user holds an Entity-scoped role for the owning Entity that grants `configuration:READ`.
3. **SQL Query Generation:**
   ```sql
   WHERE (
     ((venue IS NULL OR venue='') AND entity IN ('<AllowedEntities>'))
     OR (venue IN ('<AllowedVenues>'))
   )
   AND (venue IS NULL OR venue='' OR venue NOT IN ('<ShadowedDeniedVenues>'))
   ```
4. **Parameter Handling (`?entity=`, `?venue=`):**
   - If `?entity=` is supplied: must be in `AllowedEntities`. Otherwise return `403 Forbidden` (`ACCESS_DENIED`).
   - If `?venue=` is supplied: must be in `AllowedVenues` and not in `DeniedVenues`. Otherwise return `403 Forbidden` (`ACCESS_DENIED`).
   - If both are supplied: validate both and combine with `AND`.
5. **Select Queries (`?select=uuid1,uuid2`):**
   - Must evaluate each selected record against the user's permitted scopes, explicit parameters, and denied venues before returning.
   - `countOnly=true` returns the exact size of authorized matches.

### 8.2 Inventory & Device Visibility (`GET /api/v1/inventory`)
For every candidate device:
1. Resolve its exact Venue (`device.venue`).
2. Select the effective role using Section 7.2.
3. Include the device only if that same role grants `inventory:READ`.
4. If an exact Venue role lacks `inventory:READ`, devices in that Venue are hidden, even if the user has an Entity role granting `inventory:READ`.

### 8.3 Entity & Venue Visibility (`GET /api/v1/entity`, `GET /api/v1/venue`)
* **Entity:** Visible only through an exact Entity-scoped role granting `entity:READ`.
* **Venue:** Evaluated independently per exact Venue. An exact Venue role shadows the owning Entity role for `venue:READ`. Hierarchy trees and breadcrumbs begin at the highest visible scope.

### 8.4 Operator Visibility (`GET /api/v1/operator`)
Visible only when the user holds an Entity-scoped role directly on the `Operator Entity` (`venue == ""`) with `operator:READ`. Normal Entity roles or Venue-scoped roles never reveal Operators.

---

## 9. Codebase Implementation Mapping (`ra-wlan-cloud-owprov`)

| Requirement | Code Implementation File | Core Methods & Logic |
| :--- | :--- | :--- |
| **Point-in-Time Scope Evaluation** | `src/framework/RESTAPI_Handler.cpp` | `RESTAPIHandler::process()`, `CheckScopeAccess()`, `ResolveTargetContext()`, `PolicyAllows()` |
| **Exact-Venue Precedence (Section 7.2)** | `src/framework/RESTAPI_Handler.cpp:98–140` | Checks exact `role.venue == TargetVenue`. If found, evaluates policy without fallback. If absent, falls back to `role.entity == TargetEntity`. |
| **Configuration Scoped List** | `src/RESTAPI/RESTAPI_configurations_list_handler.cpp` | `AllowedEntities`, `AllowedVenues`, `DeniedVenues` (shadowed), SQL `ScopeWhere`, `ParamFilters` validation (403 on out-of-scope), `QB_.Select` filtering. |
| **Variables Scoped List** | `src/RESTAPI/RESTAPI_variables_list_handler.cpp` | Identical scoping, parameter validation, and select filtering for variable blocks. |
| **Entity & Venue Scoped List** | `src/RESTAPI/RESTAPI_entity_list_handler.cpp`, `RESTAPI_venue_list_handler.cpp` | Traverses visible entities and descendant venues with breadcrumb anchoring. |
| **Device Inventory Scoped List** | `src/RESTAPI/RESTAPI_inventory_list_handler.cpp` | Scopes devices to authorized venues and entities based on `inventory:READ`. |
| **Management Role Delegation** | `src/RESTAPI/RESTAPI_managementRole_handler.cpp` | Validates target user `createdBy`, exact scope authority, subset policy delegation, atomic batch creation. |
| **Parent Entity & Venue Discovery** | `src/framework/RESTAPI_Handler.cpp:715–735` | `GetDescendantVenues()`, `GetDescendantEntities()`. |
| **Auth & Policy Cache** | `src/framework/RESTAPI_Handler.cpp:615–640` | `AuthCache` singleton caching policies and user roles in shared memory. |

---

## 10. Performance & Hierarchy Traversal Architecture

### 10.1 Single-Pass Traversal with Visited Set
To eliminate $O(N^2)$ redundant database queries when resolving descendant venues under entities:
1. `GetDescendantVenues` and entity venue traversal must use a `visited` set (`std::set<std::string> visited`).
2. When iterating paginated venue records, any venue ID already present in the visited set is skipped.
3. Cycles or multi-parent references are terminated immediately.

### 10.2 Scope Caching Strategy
* User effective scopes `(userId -> {AllowedEntities, AllowedVenues, DeniedVenues})` can be cached in `AuthCache` with a short-lived TTL (e.g. 30–60 seconds).
* Cache invalidation must trigger upon any mutation of:
  - Management Roles (`POST/PUT/DELETE /api/v1/managementRole/{id}`)
  - Management Policies (`PUT/DELETE /api/v1/managementPolicy/{id}`)
  - Entity hierarchy (`POST/PUT/DELETE /api/v1/entity/{id}`)
  - Venue hierarchy (`POST/PUT/DELETE /api/v1/venue/{id}`)

---

## 11. Test Coverage & Verification Matrix

The test suite in `tests/rbac_tests/rbac_configurations_variables_scoping_test.go` verifies compliance against this specification:

| Test Case | Requirement Reference | Verification Assertion |
| :--- | :--- | :--- |
| **Root Bypass** | Section 2.1 | ROOT user sees all configurations and variable blocks globally across all tenants. |
| **Entity Admin Inheritance** | Section 7.1, 8.1 | Entity Admin with `configuration:FULL` accesses entity configs and all child venue configs. |
| **Exact-Venue Shadowing** | Section 7.2 | Venue role lacking `configuration:GET` shadows Entity Admin role: child venue configs are hidden and return 403 on explicit query. |
| **Out-of-Scope Parameter** | Section 8.1 | Tampering with `?entity=unauthorized` or `?venue=unauthorized` returns HTTP 403 Forbidden (`ACCESS_DENIED`). |
| **Combined Parameters** | Section 8.1 | Querying `?entity=E1&venue=V1` validates both parameters and returns only matching records. |
| **Select Query Filtering** | Section 8.1 | Requesting `?select=...` returns only IDs within authorized scopes; unauthorized IDs are omitted; `countOnly` returns matching count. |
| **Variable Block Scoping** | Section 8.1 | Verifies `variableBlocks` array response matches exact tenant scoping rules. |
| **Unassigned Tenant Isolation** | Section 7.2, 8.1 | Users with no roles on Entity B receive zero records from Entity B. |

