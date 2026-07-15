# Comprehensive Performance & RBAC Hardening Changes Report

This document records the exact details of the optimizations, security hardening, and user role/policy management additions across the **Provisioning Service (`ra-wlan-cloud-owprov`)** and **Frontend UI (`ra-wlan-cloud-owprov-ui`)** relative to their respective `main` branches.

---

## 1. Core Mechanics: How RBAC is Achieved Using Roles & Policies

The Role-Based Access Control (RBAC) model is split between organizational scoping and permission policies:

1. **Management Policies (`managementPolicy`)**:
   - Define a list of `entries`. Each entry maps a list of resource types (e.g., `entity`, `venue`, `inventory`, `device`, `user`) to an authorization level (e.g., `READ`, `MODIFY`, `DELETE`, `CREATE`, `NOACCESS`).
2. **Management Roles (`managementRole`)**:
   - Act as the binding agent between:
     - **Users**: A list of user IDs assigned to this role.
     - **Scope**: A specific `entity` (and optional sub-`venue`) defining the spatial boundaries of the permission.
     - **Permissions**: The assigned `managementPolicy`.

### Request Authorization Execution flow (REST API):
- When a standard user sends a request, `RESTAPIHandler::RoleIsAuthorized` resolves the target context (which `entity` or `venue` ID is requested).
- It looks up the user's roles from the `AuthCache`.
- It matches a role corresponding to the target context's entity/venue scope.
- It fetches the associated `ManagementPolicy` and checks if the HTTP method mapped to that resource (e.g., `GET` maps to `READ`/`LIST`, `POST`/`PUT` to `MODIFY`/`CREATE`) is permitted.

---

## 2. Backend (`ra-wlan-cloud-owprov`) Detailed Changes

### Issue 2.1: Context-Aware Authorization Failure (Reordering)
- **Before (Main Branch)**: Target context parameters (like `?entity=UUID`) were parsed *after* the authorization check was executed, causing the check to always fail for standard users due to missing context.
- **Now**: Reordered `ParseParameters()` to execute before `RoleIsAuthorized()` in the base REST handler pipeline.

### Issue 2.2: Redundant DB Lookup Bottleneck (Performance Optimization)
- **Before (Main Branch)**: Every request resolved roles and policies by making multiple synchronous SQL database calls to target tables.
- **Now**: Introduced `AuthCache` using a thread-safe `std::shared_mutex` to store role lists and policies.

### Issue 2.3: In-Memory Role Filtering
- **Before (Main Branch)**: `FindExistingRole` generated SQL queries to filter roles directly on the database on every lookup request:
  ```cpp
  std::string WhereClause = "entity='" + entityId + "' and venue='" + venueId + "'";
  StorageService()->RolesDB().GetRecords(0, 500, Roles, WhereClause);
  ```
- **Now**: Replaced SQL generation with in-memory iteration over cached role lists:
  ```cpp
  for (const auto &role : CachedRoles) {
      if (role.entity == entityId && role.venue == venueId) {
          ExistingRole = role;
          return true;
      }
  }
  ```

### Issue 2.4: Mutating Cache Invalidation
- **Before (Main Branch)**: Modifications to roles and policies updated SQL tables but left cached permissions out of sync.
- **Now**: Intercepted create, update, and delete endpoints in `RESTAPI_managementRole_handler.cpp` and `RESTAPI_managementPolicy_handler.cpp` to run `AuthCache::GetInstance()->Clear()` synchronously.

### Issue 2.5: Scope Visibility Leakage (List Endpoints)
- **Before (Main Branch)**: Standard users calling list endpoints returned all entries in the database database-wide.
- **Now**: Standard users only retrieve resources within their boundary.
  - **Entity Listing (`DoGet`)**: Standard users only see their assigned entity and descendants resolved recursively.
  - **Venue Listing (`DoGet`)**: Returns only venues associated with allowed descendant entities.
  - **Inventory Listing (`DoGet`)**: Filters device tags to match allowed descendant entity or venue scopes.

---

## 3. Frontend UI (`ra-wlan-cloud-owprov-ui`) Detailed Changes

### Issue 3.1: Sidebar Route Permissions Refactoring
- **Before (Main Branch)**:
  - The `/users` route was visible to all management roles under a generic top-level sidebar link.
  - No Policy Management view existed for administrative users.
- **Now**:
  - Grouped administration views inside a nested sidebar group `users-group` restricted only to `root` and `system` users. This group contains:
    - **Users List** (`/users`)
    - **Policies Management** (`/policies`)
  - A fallback route `/users` is kept for standard roles (`partner`, `admin`, `csr`) so they can access basic user views without policy access.

### Issue 3.2: Context-Aware User Creation Modal Form
- **Before (Main Branch)**:
  - Creating a user only submitted fields to create the security account (email, name, role, password).
  - No role scope or policy mapping could be selected during user creation.
- **Now**:
  - Added new dropdown fields to the User Creation Form: `Scope Type` (None / Entity / Venue), `Select Entity`, `Select Venue`, and `Select Policy`.
  - When submitting a new user, the frontend automatically intercepts the successful creation, generates a UUID for a new `ManagementRole`, and POSTs it to the provisioning API:
    ```javascript
    const newRole = {
      id: uuid(),
      name: `${formData.name}_role`,
      description: `Access role for user ${formData.name}`,
      managementPolicy: formData.scopePolicy,
      users: [createdUserId],
      entity: formData.scopeEntity,
      venue: formData.scopeType === 'venue' ? formData.scopeVenue : '',
    };
    await axiosProv.post(`managementRole/${newRole.id}`, newRole);
    ```

### Issue 3.3: Access Policy Tab on User Editing Modal Form
- **Before (Main Branch)**:
  - Editing a user only had "Main" and "Notes" tabs. No visibility or editing of their assigned tenant scopes was available.
- **Now**:
  - Added an **"Access Policy"** tab to the User Edit Form.
  - Integrated the `<ManagementRolesTable userId={selectedUser.id} />` component inside this tab, enabling admins to view, add, or delete the specific entity/venue scopes and policies linked to the user.

### Issue 3.4: Policies Management Page (`/policies`)
- **Before (Main Branch)**: No dedicated interface existed to manage policies.
- **Now**: Added a fully functional Policies Management Page (`src/pages/PoliciesPage`) featuring:
  - A policy list table with granular resource capability displays.
  - `CreatePolicyModal` and `EditPolicyModal` components to manage policy configurations and permissions.

---

## 4. Multi-Entity/Tenant Visibility Scoping Support

### Issue 4.1: Single Role Limitation on Multi-Tenant Lists
- **Before (Main Branch)**: Standard users assigned to multiple management roles (i.e., multiple entity scopes) only had the first role processed because handlers resolved scope via `FindAnyRole()`. This hid all other assigned entities, venues, and inventory tags.
- **Now**: 
  - Added the `FindAllUserRoles()` helper in `RESTAPIHandler` to query all roles linked to the user's ID using `AuthCache`.
  - Refactored list endpoints to query and merge descendants across the union of all assigned entities.

### Issue 4.2: Merging Disjoint Trees in the UI Modal
- **Before (Main Branch)**: Standard users with multiple assigned entities only saw the first entity's subtree in the "Entity and Venue Navigation" dropdown modal.
- **Now**: 
  - If a user has exactly one assigned entity, the handler returns its subtree directly.
  - If a user has multiple assigned entities, the handler dynamically constructs a virtual root node:
    ```json
    {
      "type": "entity",
      "name": "Assigned Entities",
      "uuid": "0000-0000-0000",
      "children": [ ...subtrees... ],
      "venues": []
    }
    ```
    This groups all disjoint tenant trees under a unified top-level node, allowing the frontend recursive renderer to display all of them correctly.

---

## 5. Entity Nesting Hierarchy Restrictions

### Issue 5.1: Nesting Entities Under Normal Entities
- **Before (Main Branch)**: Sub-entities could be created under any parent entity arbitrarily. This allowed nesting child entities under normal entities, creating complex multi-tier setups that violated scope isolation rules.
- **Now**: 
  - Standardized nesting constraints:
    1. A normal entity can only be created directly under the Root node.
    2. An operator entity is automatically created under the Root node during operator creation.
    3. Nesting sub-entities is only allowed if the parent entity belongs to an operator hierarchy (i.e. descends from an operator entity). Sub-entities cannot be nested under normal entities.
  - Implemented the `IsInsideOperatorHierarchy()` helper to recursively walk up the parent hierarchy chain to search for a non-empty `operatorId`.
  - Added a check in `RESTAPI_entity_handler::DoPost()`: if the parent is not the root node and is not within an operator's hierarchy, the creation is rejected with a `400 Bad Request` returning `RESTAPI::Errors::InvalidEntityType` ("Invalid entity type.").

### Issue 5.2: 403 Access Denied when Creating a Child Entity
- **Before (Main Branch)**: When a standard user (like `sumit`) tried to create a child entity under their assigned operator entity, the UI made a request `POST /api/v1/entity/0` (using the dummy ID `"0"` as a creation placeholder). The RBAC framework resolved the `TargetEntity` to `"0"` (an invalid, unauthorized resource scope) instead of looking at the target `parent` UUID specified in the body. This caused the request to be rejected with a `403 Forbidden` (`ACCESS_DENIED`).
- **Now**:
  - Refactored `RESTAPIHandler::ResolveTargetContext()` to ignore the dummy creation ID `"0"` during path-based context extraction.
  - Added a fallback to check the request's JSON body: if `parent` is defined and `TargetEntity` is empty, it resolves the context to the parent entity or parent venue. This maps the RBAC check to the correct parent hierarchy, authorizing the user's role on their assigned scope.


---

## 6. Automated Creator Role Seeding & Deletion Cleanup

### Issue 6.1: Access Denied After Creating Entity/Venue
- **Before (Main Branch)**: When standard users successfully created child entities or venues under their assigned parent scope, they lacked immediate management access to the new resource because no role granted them explicit authorization. This resulted in `403 Forbidden` (`ACCESS_DENIED`) errors when trying to read or modify the newly created resource.
- **Now**:
  - Implemented `RESTAPIHandler::AutoCreateCreatorRole()` to automatically clone the creator's role policy from the parent resource and assign it to a new `ManagementRole` specifically linked to the newly created entity/venue.
  - Automatically registers the new role in the DB, updates parent resource memberships, updates the policy usage counts, and clears `AuthCache` to apply permissions instantly.
  - Injected this call into `RESTAPI_entity_handler::DoPost` and `RESTAPI_venue_handler::DoPost`.

### Issue 6.2: Orphaning Roles and Policies on Entity/Venue Deletion
- **Before (Main Branch)**: When deleting an entity or venue, any management roles linked to them remained orphaned in the database, polluting the database and leaving policy usage counters out of sync.
- **Now**:
  - Refactored `RESTAPI_entity_handler::DoDelete` and `RESTAPI_venue_handler::DoDelete` to clean up associated management roles.
  - For every role linked in the resource's `managementRoles` array, the handler deletes the role from the database, corrects policy usage count, cleans up memberships, and invalidates `AuthCache`.
