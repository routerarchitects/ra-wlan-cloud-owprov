# OpenWifi `owprov` RBAC Test Suite Execution Guide

This document contains a complete reference of all RBAC integration test cases, environment variables, expected HTTP status codes, and exact execution commands.

---

## Environment Variables Reference Table

Before running tests, you can set one or more of these environment variables to test against your live database environment:

| Variable Name | Description | Default Fallback Value |
| :--- | :--- | :--- |
| **`OWPROV_URL`** | Base URL of `owprov` API | `https://openwifi.wlan.local:16005/api/v1` |
| **`TEST_TOKEN`** | General Token override (used for all requests) | *(Uses test case default)* |
| **`TOKEN_ROOT`** | Bearer token for ROOT user | `Bearer root-test-token` |
| **`TOKEN_ADMIN_OPERATOR_A`** | Bearer token for Admin A (Operator A Entity) | `Bearer user-admin-operator-a-token` |
| **`TOKEN_ADMIN_OPERATOR_B`** | Bearer token for Admin B (Operator B Entity) | `Bearer user-admin-operator-b-token` |
| **`TOKEN_NO_ACCESS`** | Bearer token for user with no access / read-only | `Bearer user-read-only-token` |
| **`TARGET_USER_A`** | User UUID created by Admin A | `user-a-uuid` |
| **`TARGET_USER_B`** | User UUID created by Admin B | `user-b-uuid` |
| **`OPERATOR_A_UUID`** | Operator A UUID | `b7dcf2fa-f35c-4e0a-818d-3136f38a6990` |
| **`OPERATOR_B_UUID`** | Operator B UUID | `c8edf3ab-a46d-5f1b-929e-4247g49b7001` |
| **`OPERATOR_A_REG_ID`** | Operator A Registration ID | `111` |
| **`OPERATOR_B_REG_ID`** | Operator B Registration ID | `222` |
| **`OPERATOR_A_ENTITY_UUID`** | Operator A Entity UUID | `7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097` |
| **`OPERATOR_B_ENTITY_UUID`** | Operator B Entity UUID | `8ab2b291-d04d-5c4c-b4bd-c4gcca1gb108` |
| **`VENUE_A_UUID`** | Venue UUID (owned by Operator A Entity) | `venue-a-uuid` |
| **`VENUE_B_UUID`** | Venue UUID (owned by Operator B Entity) | `venue-b-uuid` |
| **`POLICY_WEAK_ID`** | Policy ID with READ access | `94bb61a3-aa3e-49aa-9704-cc254fec4482` |
| **`POLICY_STRONG_ID`** | Policy ID with FULL access | `6f0e350a-8b7b-4ae1-bbd7-5f559792bc95` |

---

## Test Execution Commands & Details

Navigate to the test directory first:
```bash
cd /home/iotina/routerarchitects_repos/ra-wlan-cloud-owprov/tests/rbac_tests
```

---

### 1. Management Role Immutability Tests (Section 6.4)

#### **1.1 Policy ID Update Allowed (Positive)**
- **Test Function**: `TestManagementRoleImmutability` (Sub-test: `Positive: Updating only Policy ID allowed`)
- **Description**: Verifies that updating only the `managementPolicy` ID on an existing role is permitted.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
TEST_TOKEN="Bearer <token>" go test -v . -run TestManagementRoleImmutability/Positive
```

#### **1.2 Entity ID Modification Blocked (Negative)**
- **Test Function**: `TestManagementRoleImmutability` (Sub-test: `Negative: Modifying Entity ID returns 400 Bad Request`)
- **Description**: Verifies that attempting to change the `entity` field on an existing role is rejected.
- **Expected Output**: **`400 Bad Request`** (*"Entity ID, Venue ID, and User ID are immutable"*)
- **Command**:
```bash
TEST_TOKEN="Bearer <token>" go test -v . -run TestManagementRoleImmutability/Entity
```

#### **1.3 Venue ID Modification Blocked (Negative)**
- **Test Function**: `TestManagementRoleImmutability` (Sub-test: `Negative: Modifying Venue ID returns 400 Bad Request`)
- **Description**: Verifies that attempting to change the `venue` field on an existing role is rejected.
- **Expected Output**: **`400 Bad Request`**
- **Command**:
```bash
TEST_TOKEN="Bearer <token>" go test -v . -run TestManagementRoleImmutability/Venue
```

#### **1.4 Users List Modification Blocked (Negative)**
- **Test Function**: `TestManagementRoleImmutability` (Sub-test: `Negative: Modifying Users array returns 400 Bad Request`)
- **Description**: Verifies that attempting to change the `users` array on an existing role is rejected.
- **Expected Output**: **`400 Bad Request`**
- **Command**:
```bash
TEST_TOKEN="Bearer <token>" go test -v . -run TestManagementRoleImmutability/Users
```

#### **1.5 Empty Policy ID Update Blocked (Negative)**
- **Test Function**: `TestManagementRoleImmutability` (Sub-test: `Negative: Updating managementPolicy to empty string returns 400 Bad Request`)
- **Description**: Verifies that attempting to clear or set `managementPolicy` to an empty string on an existing role is rejected.
- **Expected Output**: **`400 Bad Request`**
- **Command**:
```bash
TEST_TOKEN="Bearer <token>" go test -v . -run TestManagementRoleImmutability/empty_string
```

#### **1.6 Non-Existent Policy ID Update Blocked (Negative)**
- **Test Function**: `TestManagementRoleImmutability` (Sub-test: `Negative: Updating managementPolicy to non-existent UUID returns 400 Bad Request`)
- **Description**: Verifies that updating `managementPolicy` to a non-existent UUID is rejected.
- **Expected Output**: **`400 Bad Request`**
- **Command**:
```bash
TEST_TOKEN="Bearer <token>" go test -v . -run TestManagementRoleImmutability/non-existent
```

#### **1.7 Malformed Policy ID Update Blocked (Negative)**
- **Test Function**: `TestManagementRoleImmutability` (Sub-test: `Negative: Updating managementPolicy with malformed string returns 400 Bad Request`)
- **Description**: Verifies that updating `managementPolicy` with an invalid/malformed string is rejected.
- **Expected Output**: **`400 Bad Request`**
- **Command**:
```bash
TEST_TOKEN="Bearer <token>" go test -v . -run TestManagementRoleImmutability/malformed
```

---

### 2. Operator Scope Isolation & Visibility Tests (Section 11.1)

#### **2.1 Own Operator Access Allowed (Positive)**
- **Test Function**: `TestOperatorIsolation_GetAndList` (Sub-test: `Positive: User A GET own Operator A`)
- **Description**: Verifies non-root user with role on Operator A Entity can access Operator A.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
TEST_TOKEN="Bearer <token_operator_a>" OPERATOR_A_UUID="<uuid_a>" go test -v . -run TestOperatorIsolation_GetAndList/Positive
```

#### **2.2 Cross-Operator Access Blocked (Negative)**
- **Test Function**: `TestOperatorIsolation_GetAndList` (Sub-test: `Negative: User A GET Operator B`)
- **Description**: Verifies non-root user on Operator A Entity attempting to access Operator B is blocked.
- **Expected Output**: **`403 Access Denied`**
- **Command**:
```bash
TEST_TOKEN="Bearer <token_operator_a>" OPERATOR_B_UUID="<uuid_b>" go test -v . -run TestOperatorIsolation_GetAndList/Negative:_User_A_GET_Operator_B
```

#### **2.3 Root Entity Blanket Privilege Removed (Negative)**
- **Test Function**: `TestOperatorIsolation_GetAndList` (Sub-test: `Negative: User assigned to Root Entity without operator:READ policy`)
- **Description**: Verifies user assigned to Root Entity without explicit role on Operator A cannot access Operator A.
- **Expected Output**: **`403 Access Denied`**
- **Command**:
```bash
TEST_TOKEN="Bearer <root_entity_user_token>" go test -v . -run TestOperatorIsolation_GetAndList/Root_Entity
```

#### **2.4 Operator Creation Auto-Created Creator Role (Positive)**
- **Test Function**: `TestOperator_CreatorRole_AutoCreated_Positive`
- **Description**: Verifies that when a non-root Admin creates an Operator via `POST /api/v1/operator/0`, a Management Role is automatically created for the creator directly on the companion Operator Entity with exact policy inheritance, guaranteeing immediate `GET /operator/{id}` visibility and verifying cascade role deletion on operator removal.
- **Expected Output**: **`200 OK` / `201 Created`**; auto-created role found on companion entity; operator access granted.
- **Command**:
```bash
go test -v . -run TestOperator_CreatorRole_AutoCreated_Positive
```

#### **2.5 Operator Creation by ROOT Exemption (Positive)**
- **Test Function**: `TestOperator_CreatorRole_RootExemption_Positive`
- **Description**: Verifies that ROOT can create operators without requiring a redundant local scoped role.
- **Expected Output**: **`200 OK` / `201 Created`**
- **Command**:
```bash
go test -v . -run TestOperator_CreatorRole_RootExemption_Positive
```

#### **2.6 Operator Creation Negative & Edge Validations (Negative)**
- **Test Function**: `TestOperator_Creation_MissingPermission_Negative`, `TestOperator_Creation_DuplicateRegistrationId_Negative`, `TestOperator_Creation_EntityIdInPayload_Negative`, `TestOperator_CrossTenant_Isolation_Negative`
- **Description**: Verifies `403 Access Denied` on missing `operator:CREATE`, `400 Bad Request` on duplicate `registrationId`, `400 Bad Request` on body `entityId`, and cross-tenant isolation enforcement.
- **Expected Output**: **`403 Access Denied`** or **`400 Bad Request`**
- **Command**:
```bash
go test -v . -run "TestOperator_Creation_|TestOperator_CrossTenant_"
```

#### **2.7 Operator Deletion Atomic Multi-Role Cleanup (Positive)**
- **Test Function**: `TestOperator_Deletion_AtomicMultiRoleCleanup_Positive`
- **Description**: Verifies that deleting an Operator atomically cascades to delete its companion Entity and all attached Management Roles in a single transaction without record caps, leaving zero orphaned records.
- **Expected Output**: **`200 OK`**; 404 for Operator, 404 for companion Entity, 404 for all attached roles.
- **Command**:
```bash
go test -v . -run TestOperator_Deletion_AtomicMultiRoleCleanup_Positive
```

#### **2.8 Non-Root Operator Creation Without Root Scope Grant Blocked (Negative)**
- **Test Function**: `TestOperatorCreation_NonRootWithoutRootScope_Rejected`
- **Description**: Verifies Finding 3 (P1) — operator creation (`POST /api/v1/operator/0`) attaches a companion entity to `EntityDB::RootUUID()`. A non-root caller holding `operator:CREATE` or `FULL` on a sub-entity/tenant entity without an effective grant on `EntityDB::RootUUID()` is rejected with `403 Forbidden`.
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run TestOperatorCreation_NonRootWithoutRootScope_Rejected
```

#### **2.9 Non-Root Operator Creation With Root Scope Grant Allowed (Positive)**
- **Test Function**: `TestOperatorCreation_NonRootWithRootScope_Allowed`
- **Description**: Verifies that when a non-root Admin holds `operator:CREATE` and `entity:CREATE` on `EntityDB::RootUUID()`, `POST /api/v1/operator/0` successfully resolves target scope to Root Entity without failing on the new unpersisted `registrationId`, and creates the operator.
- **Expected Output**: **`200 OK` / `201 Created`**
- **Command**:
```bash
go test -v . -run TestOperatorCreation_NonRootWithRootScope_Allowed
```

---

### 3. Subscriber Scope Resolution & CRUD Tests (Section 11.2)

#### **3.1 `registrationId` Scope Resolution (Positive)**
- **Test Function**: `TestScopeResolution_OperatorRegistrationIdQuery` (Sub-test: `Positive: Resolve registrationId=111`)
- **Description**: Verifies `registrationId=111` query parameter resolves to Operator A Entity UUID so role matching succeeds.
- **Expected Output**: **`200 OK / 201 Created`**
- **Command**:
```bash
TEST_TOKEN="Bearer <token_operator_a>" OPERATOR_A_REG_ID="111" go test -v . -run TestScopeResolution_OperatorRegistrationIdQuery/Positive
```

#### **3.2 Cross-Operator `registrationId` Resolution Blocked (Negative)**
- **Test Function**: `TestScopeResolution_OperatorRegistrationIdQuery` (Sub-test: `Negative: Cross-Operator registrationId`)
- **Description**: Verifies user on Operator A trying `registrationId=222` (Operator B) is blocked.
- **Expected Output**: **`403 Access Denied`**
- **Command**:
```bash
TEST_TOKEN="Bearer <token_operator_a>" OPERATOR_B_REG_ID="222" go test -v . -run TestScopeResolution_OperatorRegistrationIdQuery/Negative
```

#### **3.3 Missing Subscriber `CREATE` Permission (Negative)**
- **Test Function**: `TestSubscriber_MissingCreatePermission`
- **Description**: Verifies user with `subscriber:READ` policy attempting `POST /api/v1/subscriber` is blocked.
- **Expected Output**: **`403 Access Denied`**
- **Command**:
```bash
TOKEN_NO_ACCESS="Bearer <read_only_token>" OPERATOR_A_REG_ID="111" go test -v . -run TestSubscriber_MissingCreatePermission
```

#### **3.4 Missing Subscriber `UPDATE` Permission (Negative)**
- **Test Function**: `TestSubscriberUpdate_MissingUpdatePermission`
- **Description**: Verifies user with `subscriber:READ` policy attempting `PUT /api/v1/subscriber/{id}` is blocked.
- **Expected Output**: **`403 Access Denied`**
- **Command**:
```bash
TOKEN_NO_ACCESS="Bearer <read_only_token>" SUBSCRIBER_ID="<subscriber_uuid>" go test -v . -run TestSubscriberUpdate_MissingUpdatePermission
```

#### **3.5 Missing Subscriber `DELETE` Permission (Negative)**
- **Test Function**: `TestSubscriberDelete_MissingDeletePermission`
- **Description**: Verifies user with `subscriber:READ` policy attempting `DELETE /api/v1/subscriber/{id}` is blocked.
- **Expected Output**: **`403 Access Denied`**
- **Command**:
```bash
TOKEN_NO_ACCESS="Bearer <read_only_token>" SUBSCRIBER_ID="<subscriber_uuid>" go test -v . -run TestSubscriberDelete_MissingDeletePermission
```

---

### 4. Subscriber Device & Inventory Permission Tests (Sections 10 & 11.2)

#### **4.1 Query Devices with `inventory:READ` Permission (Positive)**
- **Test Function**: `TestSubscriberDevicePermissions` (Sub-test: `Positive: Query subscriber devices with inventory:READ permission`)
- **Description**: Verifies user with `inventory:READ` policy querying `GET /api/v1/subscriberDevice` succeeds.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
TEST_TOKEN="Bearer <token_with_inventory>" OPERATOR_A_UUID="<uuid_a>" go test -v . -run TestSubscriberDevicePermissions/Positive
```

#### **4.2 Query Devices without `inventory` Permission (Negative)**
- **Test Function**: `TestSubscriberDevicePermissions` (Sub-test: `Negative: Query subscriber devices with subscriber:FULL policy but missing inventory permission`)
- **Description**: Verifies user with `subscriber:FULL` policy but missing `inventory` permission is blocked.
- **Expected Output**: **`403 Access Denied`**
- **Command**:
```bash
TEST_TOKEN="Bearer <subscriber_only_token>" OPERATOR_A_UUID="<uuid_a>" go test -v . -run TestSubscriberDevicePermissions/Negative:_Query_subscriber_devices_with_subscriber:FULL
```

---

### 5. Advanced Controlled Delegation Tests (Section 8)

#### **5.1 Delegation Subset Check (Negative)**
- **Test Function**: `TestRoleAssignment_DelegationSubsetCheck`
- **Description**: Verifies Admin A with `POLICY_WEAK_ID` attempting to assign `POLICY_STRONG_ID` to Target User A is blocked.
- **Expected Output**: **`400 Bad Request`** (*"Privilege mismatch: requester does not have FULL permission..."*)
- **Command**:
```bash
TOKEN_ADMIN_OPERATOR_A="Bearer <admin_a_token>" TARGET_USER_A="<user_a_uuid>" POLICY_STRONG_ID="<strong_policy_id>" go test -v . -run TestRoleAssignment_DelegationSubsetCheck
```

#### **5.2 Venue Belongs to Entity Check (Negative)**
- **Test Function**: `TestRoleAssignment_VenueBelongsToEntityCheck`
- **Description**: Verifies assigning role on `entity = EntityA` and `venue = VenueB` (mismatched venue) is rejected.
- **Expected Output**: **`400 Bad Request`** (*"Venue does not belong to specified Entity"*)
- **Command**:
```bash
TOKEN_ADMIN_OPERATOR_A="Bearer <admin_a_token>" TARGET_USER_A="<user_a_uuid>" VENUE_B_UUID="<venue_b_uuid>" go test -v . -run TestRoleAssignment_VenueBelongsToEntityCheck
```

#### **5.3 IAM Creator Ownership Check (Negative)**
- **Test Function**: `TestRoleAssignment_CreatedByOwnershipCheck`
- **Description**: Verifies Admin A attempting to assign role to Target User B (created by Admin B) is rejected.
- **Expected Output**: **`400 Bad Request`** (*"Missing or invalid parameters"*)
- **Command**:
```bash
TOKEN_ADMIN_OPERATOR_A="Bearer <admin_a_token>" TARGET_USER_B="<user_b_uuid>" go test -v . -run TestRoleAssignment_CreatedByOwnershipCheck
```

#### **5.4 Admin Self-Assignment Prohibition (Negative)**
- **Test Function**: `TestAdmin_SelfAssignment_Prohibition_V1`, `TestAdmin_SelfAssignment_Prohibition_V2`
- **Description**: Verifies an Admin attempting to assign a Management Role to themselves (via V1 or V2 API) is strictly prohibited.
- **Expected Output**: **`400 Bad Request`** (*"You are not authorized to assign or modify roles for users you did not create."*)
- **Command**:
```bash
go test -v . -run "TestAdmin_SelfAssignment_Prohibition"
```

#### **5.5 Admin Self-Modification and Self-Deletion Prohibition (Negative & Positive Controls)**
- **Test Function**: `TestAdmin_SelfModification_And_SelfDeletion_Prohibition`
- **Description**: Verifies an Admin attempting `PUT` or `DELETE` on a role assigned to themselves is strictly prohibited with 400 Bad Request, while preserving the Self-Inspection Exemption (`GET`) and ROOT administrative governance.
- **Expected Output**: **`400 Bad Request`** for Admin self-modification and self-deletion; **`200 OK`** for self-inspection GET and ROOT governance.
- **Command**:
```bash
go test -v . -run TestAdmin_SelfModification_And_SelfDeletion_Prohibition
```

---

### 6. Hierarchy, Parent Immutability & Deletion Tests (Sections 4 & 5)

#### **6.1 Venue Parent Immutability (Negative)**
- **Test Function**: `TestVenueUpdate_ParentImmutability`
- **Description**: Verifies attempting `PUT /api/v1/venue/{id}` to change parent entity/venue is rejected.
- **Expected Output**: **`400 Bad Request`** (*"Parent reference is immutable"*)
- **Command**:
```bash
TOKEN_ROOT="Bearer <root_token>" VENUE_A_UUID="<venue_uuid>" OPERATOR_B_ENTITY_UUID="<different_entity>" go test -v . -run TestVenueUpdate_ParentImmutability
```

#### **6.2 Venue Self-Reference Cycle Prevention (Negative)**
- **Test Function**: `TestVenueCreation_SelfReferenceAndCyclePrevention`
- **Description**: Verifies `PUT /api/v1/venue/{id}` setting `parent = venue_id` is rejected.
- **Expected Output**: **`400 Bad Request`** (*"Self-reference / cycle detected"*)
- **Command**:
```bash
TOKEN_ROOT="Bearer <root_token>" VENUE_A_UUID="<venue_uuid>" go test -v . -run TestVenueCreation_SelfReferenceAndCyclePrevention
```

#### **6.3 Deleting Entity with Active Dependencies (Negative)**
- **Test Function**: `TestEntityDelete_EntityWithVenuesReturnsStillInUse`
- **Description**: Verifies `DELETE /api/v1/entity/{id}` for entity with child venues returns `StillInUse`.
- **Expected Output**: **`400 Bad Request / 409 Conflict`** (*"StillInUse"*)
- **Command**:
```bash
TOKEN_ROOT="Bearer <root_token>" OPERATOR_A_ENTITY_UUID="<entity_with_venues_uuid>" go test -v . -run TestEntityDelete_EntityWithVenuesReturnsStillInUse
```

#### **6.4 Deleting In-Use Policy Protection (Negative)**
- **Test Function**: `TestPolicyDelete_ReferencedPolicyCannotBeDeleted`
- **Description**: Verifies attempting `DELETE /api/v1/managementPolicy/{id}` for a policy referenced by active roles is rejected.
- **Expected Output**: **`400 Bad Request / 409 Conflict`**
- **Command**:
```bash
TOKEN_ROOT="Bearer <root_token>" POLICY_STRONG_ID="<active_policy_id>" go test -v . -run TestPolicyDelete_ReferencedPolicyCannotBeDeleted
```

---

### 7. Resource Permission & Exact Scope Key Uniqueness Tests (Sections 5.1 & 6.3)

#### **7.1 Configuration Missing `READ` Permission (Negative)**
- **Test Function**: `TestConfiguration_MissingReadPermission`
- **Description**: Verifies accessing `/api/v1/configuration/{id}` without `configuration:READ` permission is blocked.
- **Expected Output**: **`403 Access Denied`**
- **Command**:
```bash
TOKEN_NO_ACCESS="Bearer <read_only_token>" CONFIG_ID="<config_uuid>" go test -v . -run TestConfiguration_MissingReadPermission
```

#### **7.2 Service Class Cross-Operator Access Blocked (Negative)**
- **Test Function**: `TestServiceClass_CrossOperatorAccessBlocked`
- **Description**: Verifies user on Operator A attempting to access Service Classes of Operator B is blocked.
- **Expected Output**: **`403 Access Denied`**
- **Command**:
```bash
TOKEN_ADMIN_OPERATOR_A="Bearer <admin_a_token>" SERVICE_CLASS_B_ID="<service_class_b_uuid>" go test -v . -run TestServiceClass_CrossOperatorAccessBlocked
```

#### **7.3 Exact Scope Key Role Upsert (Positive)**
- **Test Function**: `TestManagementRoleCreation_ExactScopeKeyUniqueness`
- **Description**: Verifies creating a role on an exact scope key (`User ID + Entity ID + Venue ID`) where a role already exists updates the `managementPolicy` ID without creating duplicate DB rows.
- **Expected Output**: **`200 OK / 201 Created`**
- **Command**:
```bash
TOKEN_ADMIN_OPERATOR_A="Bearer <admin_a_token>" TARGET_USER_A="<user_a_uuid>" OPERATOR_A_ENTITY_UUID="<entity_uuid>" POLICY_WEAK_ID="<policy_id>" go test -v . -run TestManagementRoleCreation_ExactScopeKeyUniqueness
```

---

### 8. Management Policy Deletion Protection Tests

#### **8.1 Unreferenced Policy Deletion (Positive)**
- **Test Function**: `TestManagementPolicyDeletionProtection` (Sub-test: `Positive: Deleting unreferenced policy succeeds`)
- **Description**: Verifies deleting a policy that is not linked to any role succeeds.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
TEST_TOKEN="Bearer <root_token>" go test -v . -run TestManagementPolicyDeletionProtection/unreferenced
```

#### **8.2 In-Use Policy Deletion Blocked (Negative)**
- **Test Function**: `TestManagementPolicyDeletionProtection` (Sub-test: `Negative: Deleting in-use policy returns 400 Bad Request`)
- **Description**: Verifies deleting a policy assigned to an active role returns 400 Bad Request with "Management policy is currently assigned to one or more management roles".
- **Expected Output**: **`400 Bad Request`**
- **Command**:
```bash
TEST_TOKEN="Bearer <root_token>" go test -v . -run TestManagementPolicyDeletionProtection/in-use
```

#### **8.3 Non-Root Policy Deletion Blocked (Negative)**
- **Test Function**: `TestManagementPolicyDeletionProtection` (Sub-test: `Negative: Non-root user cannot delete management policy`)
- **Description**: Verifies non-root users cannot delete management policies.
- **Expected Output**: **`401 Unauthorized / 403 Forbidden`**
- **Command**:
```bash
TEST_TOKEN="Bearer <root_token>" TOKEN_NO_ACCESS="Bearer <non_root_token>" go test -v . -run TestManagementPolicyDeletionProtection/Non-root
```

#### **8.4 Non-Existent Policy Deletion (Negative)**
- **Test Function**: `TestManagementPolicyDeletionProtection` (Sub-test: `Negative: Deleting non-existent policy returns 404 Not Found`)
- **Description**: Verifies deleting a non-existent policy ID returns 404 Not Found.
- **Expected Output**: **`404 Not Found`**
- **Command**:
```bash
TEST_TOKEN="Bearer <root_token>" go test -v . -run TestManagementPolicyDeletionProtection/non-existent
```

#### **8.5 Policy Deletion After Role Reassignment (Positive)**
- **Test Function**: `TestManagementPolicyDeletionProtection` (Sub-test: `Positive: Deleting policy after reassigning role succeeds`)
- **Description**: Verifies that reassigning an active role to another policy frees the original policy, allowing it to be deleted.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
TEST_TOKEN="Bearer <root_token>" go test -v . -run TestManagementPolicyDeletionProtection/reassigning
```

---

### **9. Issue 3 (P1): Dual-Scope Inventory Transfer Authorization (`PUT /inventory/{id}`)**

#### **9.1 Lacking Role on Destination Scope (Negative)**
- **Test Function**: `TestInventoryDualScopeTransfer` (Sub-test: `Negative: Transfer rejected when actor lacks role on Destination Venue`)
- **Description**: Verifies that an actor holding `inventory:UPDATE` on the originating venue but lacking an assigned role on the destination venue is rejected with 403 Forbidden.
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run TestInventoryDualScopeTransfer/lacks_role
```

#### **9.2 Destination Scope Has Only `inventory:READ` (Negative)**
- **Test Function**: `TestInventoryDualScopeTransfer` (Sub-test: `Negative: Transfer rejected when Destination Venue has only inventory:READ`)
- **Description**: Verifies that holding only `inventory:READ` on the destination venue is insufficient; the transfer is rejected with 403 Forbidden.
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run TestInventoryDualScopeTransfer/only_inventory:READ
```

#### **9.3 Destination Scope Has Only `inventory:CREATE` (Negative)**
- **Test Function**: `TestInventoryDualScopeTransfer` (Sub-test: `Negative: Transfer rejected when Destination Venue has only inventory:CREATE`)
- **Description**: Verifies Section 8.1.4 Item 3: holding only `inventory:CREATE` on destination scope is insufficient to receive a transferred device; returns 403 Forbidden.
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run TestInventoryDualScopeTransfer/only_inventory:CREATE
```

#### **9.4 Authorized Dual-Scope Transfer (Positive)**
- **Test Function**: `TestInventoryDualScopeTransfer` (Sub-test: `Positive: Transfer succeeds when actor holds inventory:UPDATE on both scopes`)
- **Description**: Verifies that holding effective `inventory:UPDATE` on both source and destination venues allows device transfer with 200 OK and updates container memberships.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run TestInventoryDualScopeTransfer/holds_inventory:UPDATE_on_both_scopes
```

#### **9.5 Mutual Exclusivity Violation on `PUT` (Edge Case)**
- **Test Function**: `TestInventoryDualScopeTransfer` (Sub-test: `Edge: Supplying both entity and venue simultaneously is rejected with NotBoth`)
- **Description**: Verifies that supplying both `entity` and `venue` simultaneously in `PUT /inventory/{id}` is rejected with 400 Bad Request (`NotBoth`).
- **Expected Output**: **`400 Bad Request`**
- **Command**:
```bash
go test -v . -run TestInventoryDualScopeTransfer/NotBoth
```

#### **9.6 Single-Scope Invariant Auto-Detachment (Edge Case)**
- **Test Function**: `TestInventoryDualScopeTransfer` (Sub-test: `Edge: Moving to Venue automatically clears direct Entity binding`)
- **Description**: Verifies that moving a directly entity-bound device to a venue automatically detaches it from the entity (`entity == ""`), ensuring the single-scope invariant.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run TestInventoryDualScopeTransfer/Moving_to_Venue_automatically_clears
```

#### **9.7 Detaching to Unassigned Scope Blocked for Non-Root (Edge Case)**
- **Test Function**: `TestInventoryDualScopeTransfer` (Sub-test: `Edge: Detaching device to unassigned pool by non-root is rejected`)
- **Description**: Verifies that non-root actors cannot detach devices into unassigned inventory (`venue: ""` and `entity: ""`), returning 403 Forbidden.
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run TestInventoryDualScopeTransfer/Detaching_device_to_unassigned
```

#### **9.8 ROOT Unconditional Transfer Parity (Positive)**
- **Test Function**: `TestInventoryDualScopeTransfer` (Sub-test: `Positive: ROOT user can transfer devices unconditionally`)
- **Description**: Verifies that ROOT user can move devices between any scopes unconditionally without scope role constraints (provided device is not subscriber-assigned).
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run TestInventoryDualScopeTransfer/ROOT_user_can_transfer
```

#### **9.9 Transfer of Subscriber-Assigned Device Prohibited for Admin & ROOT (Negative)**
- **Test Function**: `TestInventoryDualScopeTransfer` (Sub-tests: `Negative: Transfer of subscriber-assigned device prohibited for Admin`, `Negative: Transfer of subscriber-assigned device prohibited for ROOT`)
- **Description**: Verifies the business invariant that any attempt to transfer a device currently assigned to a subscriber (`subscriber != ""`) is rejected with 400 Bad Request (StillInUse), for both Administrators and ROOT.
- **Expected Output**: **`400 Bad Request`** (*"Subscriber-assigned inventory cannot be transferred. Remove subscriber assignment first."*)
- **Command**:
```bash
go test -v . -run "TestInventoryDualScopeTransfer/Negative:_Transfer_of_subscriber-assigned_device"
```

#### **9.10 Device Transfer Permitted After Subscriber Assignment Removed (Positive)**
- **Test Function**: `TestInventoryDualScopeTransfer` (Sub-test: `Positive: Transfer permitted after subscriber assignment removed`)
- **Description**: Verifies that after de-provisioning the subscriber assignment via `?removeSubscriber=`, the device can be transferred normally.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run "TestInventoryDualScopeTransfer/Positive:_Transfer_permitted_after_subscriber"
```

---

### **10. Issue 4 (P2): Decouple Entity / Operator Permissions from Management Role Administration**

#### **10.1 `entity:FULL` Insufficient for `managementRole:CREATE` (Negative)**
- **Test Function**: `TestManagementRoleDecoupling` (Sub-test: `Negative: entity:FULL is denied managementRole:CREATE`)
- **Description**: Verifies Section 7.2 Pillar 4 (Strict Separation of Duties) — holding `entity:FULL` without an explicit `managementRole` grant is rejected with 403 Forbidden when creating/assigning roles.
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run TestManagementRoleDecoupling/entity:FULL_is_denied
```

#### **10.2 `operator:FULL` Insufficient for `managementRole:CREATE` (Negative)**
- **Test Function**: `TestManagementRoleDecoupling` (Sub-test: `Negative: operator:FULL is denied managementRole:CREATE`)
- **Description**: Verifies that holding `operator:FULL` without an explicit `managementRole` grant is rejected with 403 Forbidden.
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run TestManagementRoleDecoupling/operator:FULL_is_denied
```

#### **10.3 Non-Root Users Cannot Mutate `managementPolicy` (Negative)**
- **Test Function**: `TestManagementRoleDecoupling` (Sub-test: `Negative: entity:FULL cannot mutate managementPolicy (ROOT only)`)
- **Description**: Verifies Section 6.4 — management policy mutation is strictly restricted to ROOT; non-root users holding `entity:FULL` are rejected with 403 Forbidden.
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run TestManagementRoleDecoupling/cannot_mutate_managementPolicy
```

#### **10.4 Authenticated Users Can Read `managementPolicy` (Positive)**
- **Test Function**: `TestManagementRoleDecoupling` (Sub-test: `Positive: Authenticated users can GET managementPolicy`)
- **Description**: Verifies that all authenticated users can view/list management policies (`GET /api/v1/managementPolicy`).
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run TestManagementRoleDecoupling/can_GET_managementPolicy
```

#### **10.5 Explicit `managementRole` Grant Authorizes Role Assignment (Positive)**
- **Test Function**: `TestManagementRoleDecoupling` (Sub-test: `Positive: Explicit managementRole:FULL authorizes managementRole:CREATE`)
- **Description**: Verifies that holding an explicit `managementRole:FULL` or `managementRole:CREATE` grant authorizes role administration.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run TestManagementRoleDecoupling/Explicit_managementRole:FULL
```

#### **10.6 `managementRole:UPDATE` Insufficient for `managementRole:CREATE` (Negative P1 Regression)**
- **Test Function**: `TestManagementRoleDecoupling` (Sub-test: `Negative: managementRole:UPDATE is denied managementRole:CREATE (P1 Regression)`) & `TestManagementRoleCreate_UpdatePermissionDoesNotGrantCreate_P1Regression`
- **Description**: Verifies Section 5.3 Permission Semantics — holding `managementRole:UPDATE` grants READ and UPDATE, but strictly DENIES `managementRole:CREATE`.
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run TestManagementRoleDecoupling/managementRole:UPDATE_is_denied
```

#### **10.7 `managementRole:UPDATE` Authorizes Role Listing & Reading (Positive Section 5.3)**
- **Test Function**: `TestManagementRoleDecoupling` (Sub-test: `Positive: managementRole:UPDATE authorizes role listing (READ)`)
- **Description**: Verifies Section 5.3 Permission Expansion — `managementRole:UPDATE` inherently expands to include `READ` permissions, authorizing `GET /api/v1/managementRole`.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run TestManagementRoleDecoupling/authorizes_role_listing
```

#### **10.8 `managementRole:UPDATE` Cannot Delete Roles (Edge Case)**
- **Test Function**: `TestManagementRoleDecoupling` (Sub-test: `Edge Case: managementRole:UPDATE is denied managementRole:DELETE`)
- **Description**: Verifies that `managementRole:UPDATE` cannot delete management roles (`DELETE /api/v1/managementRole/{id}`).
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run TestManagementRoleDecoupling/denied_managementRole:DELETE
```

---

### **11. Issue 5 (P2): Venue Deletion Structural Dependency Checks (`DELETE /venue/{id}`)**

#### **11.1 Venue Deletion Blocked by Attached Contact (Negative)**
- **Test Function**: `TestVenueDeletionDependencies` (Sub-test: `Negative: Venue deletion blocked by attached contact with clear error`)
- **Description**: Verifies Section 4.2 & 10.1 — venue deletion is blocked when contacts are attached, returning `StillInUse` (1005) with clear message identifying `contact(s)`.
- **Expected Output**: **`400 Bad Request`** (*"Still in use. (Cannot delete venue while dependencies exist: 1 contact(s))"*)
- **Command**:
```bash
go test -v . -run TestVenueDeletionDependencies/blocked_by_attached_contact
```

#### **11.2 Venue Deletion Blocked by Attached Location (Negative)**
- **Test Function**: `TestVenueDeletionDependencies` (Sub-test: `Negative: Venue deletion blocked by attached location with clear error`)
- **Description**: Verifies Section 4.2 & 10.1 — venue deletion is blocked when a location is attached, returning `StillInUse` with clear message identifying `attached location`.
- **Expected Output**: **`400 Bad Request`** (*"Still in use. (Cannot delete venue while dependencies exist: attached location)"*)
- **Command**:
```bash
go test -v . -run TestVenueDeletionDependencies/blocked_by_attached_location
```

#### **11.3 Venue Deletion Permitted When Configuration Is Attached (Positive)**
- **Test Function**: `TestVenueDeletionDependencies` (Sub-test: `Positive: Venue deletion permitted when configuration is attached`)
- **Description**: Verifies Section 4.2 & 10.1 — venue deletion is not blocked by attached configurations (configurations are detached upon deletion).
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run TestVenueDeletionDependencies/Venue_deletion_permitted_when_configuration
```

#### **11.4 Venue Deletion Blocked by Child Venue (Negative)**
- **Test Function**: `TestVenueDeletionDependencies` (Sub-test: `Negative: Venue deletion blocked by child venue with clear error`)
- **Description**: Verifies Section 4.2 & 10.1 — venue deletion is blocked when child venues exist, identifying `child venue(s)`.
- **Expected Output**: **`400 Bad Request`** (*"Still in use. (Cannot delete venue while dependencies exist: 1 child venue(s))"*)
- **Command**:
```bash
go test -v . -run TestVenueDeletionDependencies/blocked_by_child_venue
```

#### **11.5 Management Roles Do NOT Block Deletion & Are Cascaded (Positive)**
- **Test Function**: `TestVenueDeletionDependencies` (Sub-test: `Positive: Management roles do not block deletion and are cascadingly purged`)
- **Description**: Verifies Section 10.1 & 10.2 — Management Roles do not block venue deletion; deleting a venue returns 200 OK and automatically cascades deletion of all associated Management Roles.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run TestVenueDeletionDependencies/Management_roles_do_not_block
```

---

### **12. Issue 6 (P2): Self-Role Inspection Exemption for Functional Users (`GET /managementRole`, `GET /managementRole/{id}`)**

#### **12.1 Functional User Can List Own Roles Without `managementRole:READ` (Positive)**
- **Test Function**: `TestSelfRoleInspectionExemption` (Sub-test: `Positive: Functional user can list own roles without managementRole:READ`)
- **Description**: Verifies Section 6.4 — functional callers without `managementRole:READ` get their own assigned roles filtered and returned, without leaking other users' roles.
- **Expected Output**: **`200 OK`** (contains only caller's roles)
- **Command**:
```bash
go test -v . -run "TestSelfRoleInspectionExemption/Positive:_Functional_user_can_list_own_roles"
```

#### **12.2 Querying `?user=<own_id>` Returns Own Roles (Positive)**
- **Test Function**: `TestSelfRoleInspectionExemption` (Sub-test: `Positive: Functional user query with ?user=<own_id> returns own roles`)
- **Description**: Verifies Section 6.4 — filtering by own user ID returns assigned roles.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run "TestSelfRoleInspectionExemption/Positive:_Functional_user_query_with_\?user=<own_id>"
```

#### **12.3 Direct Role Fetch by ID under Self-Inspection Exemption (Positive)**
- **Test Function**: `TestSelfRoleInspectionExemption` (Sub-test: `Positive: Functional user can fetch own role directly by ID`)
- **Description**: Verifies Section 6.4 — `GET /api/v1/managementRole/{id}` permits direct inspection of own role even without `managementRole:READ`.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run "TestSelfRoleInspectionExemption/Positive:_Functional_user_can_fetch_own_role_directly_by_ID"
```

#### **12.4 Direct Role Fetch of Other User's Role Denied (Negative)**
- **Test Function**: `TestSelfRoleInspectionExemption` (Sub-test: `Negative: Functional user fetching other user's role by ID is denied (403)`)
- **Description**: Verifies Section 6.4 — attempting to fetch another user's role without `managementRole:READ` is rejected with 403 Forbidden.
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run "TestSelfRoleInspectionExemption/Negative:_Functional_user_fetching_other_user's_role"
```

#### **12.5 Querying `?user=<other_id>` Returns Empty List (Negative)**
- **Test Function**: `TestSelfRoleInspectionExemption` (Sub-test: `Negative: Functional user querying ?user=<other_user_id> returns empty list`)
- **Description**: Verifies Section 6.4 — attempting to query another user's roles via `?user=<other>` returns an empty list `[]` (not error, but zero records).
- **Expected Output**: **`200 OK`** (`roles: []`)
- **Command**:
```bash
go test -v . -run "TestSelfRoleInspectionExemption/Negative:_Functional_user_querying_\?user=<other_user_id>"
```

#### **12.6 Regression: Mixed Role Permissions (Installer in Venue A, Admin in Venue B)**
- **Test Function**: `TestSelfRoleInspection_MixedPermissions_Regression`
- **Description**: Verifies that a non-root user holding both an Installer role in Venue A (without `managementRole:READ`) and an Admin role in Venue B (with `managementRole:READ`) receives the union of all directly assigned roles plus scope-authorized roles. The user sees both of their assigned roles, sees other users' roles in Venue B, and never sees other users' roles in Venue A.
- **Expected Output**: **`200 OK`** (union without leakage)
- **Command**:
```bash
go test -v . -run TestSelfRoleInspection_MixedPermissions_Regression
```

---

### **13. Variable Resource Access & Scope Resolution (`GET /variable`)**

#### **13.1 Admin Can Read Variable Block via `?select={uuid}` (Positive)**
- **Test Function**: `TestVariableAccessAuthorization` (Sub-test: `Positive: Admin can read variable block via ?select={uuid}`)
- **Description**: Verifies that `/variable` maps to `configuration` permissions, resolves target scope from DB via `?select=`, and allows admin within scope.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run "TestVariableAccessAuthorization/Positive:_Admin_can_read_variable_block_via_\?select={uuid}"
```

#### **13.2 Admin Can Read Variable Block by Direct UUID Path (Positive)**
- **Test Function**: `TestVariableAccessAuthorization` (Sub-test: `Positive: Admin can read variable block by direct UUID path`)
- **Description**: Verifies direct path `/api/v1/variable/{uuid}` resolves target scope and authorizes access.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
go test -v . -run "TestVariableAccessAuthorization/Positive:_Admin_can_read_variable_block_by_direct_UUID_path"
```

#### **13.3 Multi-ID Selection Skips Unauthorized Variable Block (Negative)**
- **Test Function**: `TestVariableAccessAuthorization` (Sub-test: `Negative: Multi-ID selection skips unauthorized out-of-scope variable block`)
- **Description**: Verifies that when a caller queries `GET /api/v1/variable?select=<authorized_id>,<unauthorized_id>`, each ID is evaluated against the caller's allowed scopes with `configuration:READ`; the authorized variable block is returned and the unauthorized variable block is skipped without leaking tenant data.
- **Expected Output**: **`200 OK`** (only authorized variable blocks included in array)
- **Command**:
```bash
go test -v . -run "TestVariableAccessAuthorization/Negative:_Multi-ID_selection_skips_unauthorized"
```

#### **13.4 Unfiltered Listing Excludes Out-of-Scope Variable Blocks (Negative)**
- **Test Function**: `TestVariableAccessAuthorization` (Sub-test: `Negative: Unfiltered listing excludes out-of-scope variable blocks`)
- **Description**: Verifies that `GET /api/v1/variable` filters variables strictly by caller's allowed entities/venues with `configuration:READ`. Variable blocks from other tenants/entities are excluded.
- **Expected Output**: **`200 OK`** (strictly scoped variable blocks)
- **Command**:
```bash
go test -v . -run "TestVariableAccessAuthorization/Negative:_Unfiltered_listing_excludes_out-of-scope"
```

#### **13.5 Variable Block Transfer to Unauthorized Destination Blocked (Negative)**
- **Test Function**: `TestVariableAccessAuthorization` (Sub-test: `Negative: Transferring variable block to unauthorized entity scope is blocked (403)`)
- **Description**: Verifies Finding 4 (P1) — when mutating a variable block's `entity` or `venue` via `PUT /api/v1/variable/{uuid}`, the caller must hold `configuration:UPDATE` on the destination scope. Transfers without destination authorization are rejected with `403 Forbidden`.
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
go test -v . -run "TestVariableAccessAuthorization/Negative:_Transferring_variable_block_to_unauthorized"
```

#### **13.6 Mutual Exclusivity Violation on Variable Update (Edge Case)**
- **Test Function**: `TestVariableAccessAuthorization` (Sub-test: `Edge: Supplying both entity and venue on variable update is rejected with 400 NotBoth`)
- **Description**: Verifies that supplying both `entity` and `venue` simultaneously in `PUT /api/v1/variable/{uuid}` is rejected with `400 Bad Request` (`NotBoth`), preserving the single-scope invariant.
- **Expected Output**: **`400 Bad Request`**
- **Command**:
```bash
go test -v . -run "TestVariableAccessAuthorization/Edge:_Supplying_both_entity_and_venue"
```

---

### **14. Configurations and Variables List Scoping (`GET /configurations`, `GET /variables`)**

#### **14.1 Configurations List Root Access Bypass (Positive)**
- **Test Function**: `TestConfigurationsList_RootBypass`
- **Description**: Verifies ROOT users and internal microservices bypass scoping and receive the complete global list.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
TOKEN_ROOT="Bearer <root_token>" go test -v . -run TestConfigurationsList_RootBypass
```

#### **14.2 Configurations List Entity Scoped Admin & Cross-Tenant Isolation (Positive & Negative)**
- **Test Function**: `TestConfigurationsList_EntityScopedAdmin`
- **Description**: Verifies Admin on Operator A Entity receives configurations in Operator A (and child venues) and verifies zero leakage from Operator B.
- **Expected Output**: **`200 OK` (0 Operator B configs)**
- **Command**:
```bash
TOKEN_ADMIN_OPERATOR_A="Bearer <admin_a_token>" OPERATOR_A_ENTITY_UUID="<entity_a_uuid>" OPERATOR_B_ENTITY_UUID="<entity_b_uuid>" go test -v . -run TestConfigurationsList_EntityScopedAdmin
```

#### **14.3 Venue-Scoped Role Excludes Parent Entity & Descendant Venues (Positive, Negative & Edge)**
- **Test Function**: `TestConfigurationsList_VenueScopedRole_ExcludesParentEntity` & `TestConfigurationsList_VenueScopedRole_ExactVenueOnly_P1Regression`
- **Description**: Verifies user assigned to Venue A receives only Venue A configurations and is denied access to parent Entity A configurations, sibling venues, and descendant/child venues.
- **Expected Output**: **`200 OK` (exact Venue A configs only) / `403 Forbidden` (on child or out-of-scope venues)**
- **Command**:
```bash
TOKEN_VENUE_A="Bearer <venue_a_token>" VENUE_A_UUID="<venue_a_uuid>" OPERATOR_A_ENTITY_UUID="<entity_a_uuid>" VENUE_A_CHILD_UUID="<child_venue_uuid>" go test -v . -run TestConfigurationsList_VenueScopedRole_ExactVenueOnly_P1Regression
```

#### **14.4 Missing configuration:GET Permission (Negative)**
- **Test Function**: `TestConfigurationsList_NoPermissions`
- **Description**: Verifies users with policies lacking `configuration:GET` receive an empty list `{"configurations": []}`.
- **Expected Output**: **`200 OK` (empty array)**
- **Command**:
```bash
TOKEN_NO_ACCESS="Bearer <no_access_token>" go test -v . -run TestConfigurationsList_NoPermissions
```

#### **14.5 Cross-Tenant Query Parameter Tampering Blocked (Negative)**
- **Test Function**: `TestConfigurationsList_CrossTenantQueryParam_Negative`
- **Description**: Verifies that passing `?entity=EntityB` or `?venue=VenueB` with an Entity A token returns `403 Forbidden` (`ACCESS_DENIED`).
- **Expected Output**: **`403 Forbidden`**
- **Command**:
```bash
TOKEN_ADMIN_OPERATOR_A="Bearer <admin_a_token>" OPERATOR_B_ENTITY_UUID="<entity_b_uuid>" VENUE_B_UUID="<venue_b_uuid>" go test -v . -run TestConfigurationsList_CrossTenantQueryParam_Negative
```

#### **14.6 Configurations List Count Only (Positive & Negative)**
- **Test Function**: `TestConfigurationsList_CountOnly`
- **Description**: Verifies `?countOnly=true` returns accurate scoped counts for permitted users and `{"count": 0}` for unpermitted users.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
TOKEN_ADMIN_OPERATOR_A="Bearer <admin_a_token>" TOKEN_NO_ACCESS="Bearer <no_access_token>" go test -v . -run TestConfigurationsList_CountOnly
```

#### **14.7 Configurations List Select Scoped (Positive & Negative)**
- **Test Function**: `TestConfigurationsList_SelectScoped`
- **Description**: Validates that when `?select=` is provided, explicit entity/venue filters are strictly enforced on selected records and `countOnly` returns matching count.
- **Expected Output**: **`200 OK`**
- **Command**:
```bash
TOKEN_ADMIN_OPERATOR_A="Bearer <admin_a_token>" OPERATOR_A_ENTITY_UUID="<entity_a_uuid>" go test -v . -run TestConfigurationsList_SelectScoped
```

#### **14.8 Variables List Scoping (Positive, Negative & Edge)**
- **Test Functions**:
  - `TestVariablesList_RootBypass`
  - `TestVariablesList_EntityScopedAdmin`
  - `TestVariablesList_VenueScopedRole_ExcludesParentEntity`
  - `TestVariablesList_VenueScopedRole_ExactVenueOnly_P1Regression`
  - `TestVariablesList_NoPermissions`
  - `TestVariablesList_CrossTenantQueryParam_Negative`
  - `TestVariablesList_CountOnly`
  - `TestVariablesList_Unauthenticated`
  - `TestVariablesList_SelectScoped`
- **Description**: Verifies that `GET /api/v1/variables` enforces the exact same multi-tenant RBAC inheritance, isolation, descendant venue exclusion, query parameter validation (403 Forbidden on out-of-scope parameters), and select scoping.
- **Expected Output**: **`200 OK` / `403 Forbidden`**
- **Command**:
```bash
TOKEN_VENUE_A="Bearer <venue_a_token>" VENUE_A_UUID="<venue_a_uuid>" VENUE_A_CHILD_UUID="<child_venue_uuid>" go test -v . -run TestVariablesList_VenueScopedRole_ExactVenueOnly_P1Regression
```

---

### **15. Issue 7 (P1): V2 Management-Role Creation Exact Scope & Batch Delegation Enforcement (`POST /api/v2/managementRole/0`)**

#### **15.1 Batch Venue Creation with Full Scope Authority (Positive)**
- **Test Function**: `TestManagementRole_V2_ExactScopeAndBatchDelegation_P1Regression` (Sub-test: `Positive: Authorized entity administrator can batch-create roles across owned venues`)
- **Description**: Verifies an authorized administrator holding `managementRole:CREATE` across target venues can batch-assign roles via `venueIds: [...]`.
- **Expected Output**: **`200 OK` / `201 Created`**
- **Command**:
```bash
TOKEN_ROOT="Bearer <root_token>" go test -v . -run TestManagementRole_V2_ExactScopeAndBatchDelegation_P1Regression/batch-create
```

#### **15.2 Batch Venue Creation Rejected when Target Venue Lacks Authority (Negative)**
- **Test Function**: `TestManagementRole_V2_ExactScopeAndBatchDelegation_P1Regression` (Sub-test: `Negative: Batch creation rejected when target venue lacks managementRole:CREATE authority`)
- **Description**: Verifies that including an unauthorized venue in `venueIds` causes the entire batch creation to be rejected with 400 Bad Request / 403 Forbidden.
- **Expected Output**: **`400 Bad Request` / `403 Forbidden`**
- **Command**:
```bash
TOKEN_ADMIN_OPERATOR_A="Bearer <admin_a_token>" go test -v . -run TestManagementRole_V2_ExactScopeAndBatchDelegation_P1Regression/lacks_managementRole:CREATE
```

#### **15.3 Single Target Venue Creation Rejected when Lacking Authority (Negative)**
- **Test Function**: `TestManagementRole_V2_ExactScopeAndBatchDelegation_P1Regression` (Sub-test: `Negative: User without managementRole:CREATE on single target venue is rejected`)
- **Description**: Verifies a user without `managementRole:CREATE` cannot create a role on a target venue via V2.
- **Expected Output**: **`400 Bad Request` / `403 Forbidden`**
- **Command**:
```bash
TOKEN_NO_ACCESS="Bearer <no_access_token>" go test -v . -run TestManagementRole_V2_ExactScopeAndBatchDelegation_P1Regression/single_target_venue
```

#### **15.4 Entity Role Creation Enforces managementRole:CREATE on Entity (Edge Case)**
- **Test Function**: `TestManagementRole_V2_ExactScopeAndBatchDelegation_P1Regression` (Sub-test: `Edge Case: Empty venueIds defaults to entity role and enforces managementRole:CREATE on entity`)
- **Description**: Verifies that empty `venueIds: []` falls back to entity role creation and strictly validates `managementRole:CREATE` on the target entity.
- **Expected Output**: **`400 Bad Request` / `403 Forbidden`**
- **Command**:
```bash
TOKEN_NO_ACCESS="Bearer <no_access_token>" go test -v . -run TestManagementRole_V2_ExactScopeAndBatchDelegation_P1Regression/Empty_venueIds
```

---

## Run All Tests Simultaneously

To run all RBAC tests in one single command:
```bash
cd /home/iotina/routerarchitects_repos/ra-wlan-cloud-owprov/tests/rbac_tests
TEST_TOKEN="Bearer <your_active_token>" go test -v .
```
