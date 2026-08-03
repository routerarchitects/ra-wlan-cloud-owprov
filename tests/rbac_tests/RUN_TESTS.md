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

## Run All Tests Simultaneously

To run all RBAC tests in one single command:
```bash
cd /home/iotina/routerarchitects_repos/ra-wlan-cloud-owprov/tests/rbac_tests
TEST_TOKEN="Bearer <your_active_token>" go test -v .
```
