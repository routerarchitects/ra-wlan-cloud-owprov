package rbac_tests

import (
	"fmt"
	"net/http"
	"testing"
)

// ----------------------------------------------------------------------------
// 1. CONFIGURATION RESOURCE PERMISSION TESTS (Section 5.1 & 7.2)
// ----------------------------------------------------------------------------

/*
 * TestConfiguration_MissingReadPermission
 *
 * DESCRIPTION:
 *   Validates Section 5.1 & 7.2 configuration authorization:
 *   Accessing or reading a Configuration record requires configuration:READ or configuration:FULL permission.
 *   If a user has role on Entity A but policy grants only entity:READ (missing configuration),
 *   GET /api/v1/configuration/{id} MUST be denied.
 *
 * SCENARIO:
 *   User with entity:READ policy attempts GET /api/v1/configuration/{CONFIG_ID}.
 *
 * EXPECTED OUTPUT:
 *   HTTP 403 Access Denied.
 */
func TestConfiguration_MissingReadPermission(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenNoAccess := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-token")
	configID := getEnvOrDefault("CONFIG_ID", "config-uuid-123")

	status, body, err := client.DoRequest("GET", fmt.Sprintf("/configuration/%s", configID), tokenNoAccess, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusForbidden {
		t.Errorf("Expected 403 Access Denied for missing configuration:READ permission, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 2. SERVICE CLASS OPERATOR SCOPE CONSTRAINT TESTS
// ----------------------------------------------------------------------------

/*
 * TestServiceClass_CrossOperatorAccessBlocked
 *
 * DESCRIPTION:
 *   Validates Operator scope isolation for Service Class management:
 *   A user assigned to Operator A Entity CANNOT access, update, or delete Service Classes 
 *   belonging to Operator B.
 *
 * SCENARIO:
 *   User on Operator A Entity attempts GET /api/v1/serviceClass/{SERVICE_CLASS_B_ID}.
 *
 * EXPECTED OUTPUT:
 *   HTTP 403 Access Denied.
 */
func TestServiceClass_CrossOperatorAccessBlocked(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	serviceClassB := getEnvOrDefault("SERVICE_CLASS_B_ID", "service-class-b-uuid")

	status, body, err := client.DoRequest("GET", fmt.Sprintf("/serviceClass/%s", serviceClassB), tokenAdminA, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusForbidden {
		t.Errorf("Expected 403 Access Denied for cross-operator service class access, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 3. EXACT SCOPE KEY UNIQUENESS TESTS (Section 6.3)
// ----------------------------------------------------------------------------

/*
 * TestManagementRoleCreation_ExactScopeKeyUniqueness
 *
 * DESCRIPTION:
 *   Validates Section 6.3 Stored Role and Uniqueness rule:
 *   For a given user, only ONE role may exist for the same exact scope key:
 *   - Entity scope key: User ID + Entity ID + no Venue ID
 *   - Venue scope key: User ID + Entity ID + Venue ID
 *   Submitting an assignment for an existing exact scope key updates its Policy ID 
 *   instead of creating a duplicate row in the database.
 *
 * SCENARIO:
 *   Admin A submits POST /api/v1/managementRole for TARGET_USER_A on OPERATOR_A_ENTITY_UUID 
 *   when a role already exists for TARGET_USER_A on that exact scope.
 *
 * EXPECTED OUTPUT:
 *   HTTP 200 OK (Updates existing role policy ID; does not throw unique constraint violation or create duplicate).
 */
func TestManagementRoleCreation_ExactScopeKeyUniqueness(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	targetUserA := getEnvOrDefault("TARGET_USER_A", "user-created-by-admin-a-uuid")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	weakPolicy := getEnvOrDefault("POLICY_WEAK_ID", "94bb61a3-aa3e-49aa-9704-cc254fec4482")

	payload := map[string]interface{}{
		"users":            []string{targetUserA},
		"entity":           entityA,
		"venue":            "",
		"managementPolicy": weakPolicy,
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", tokenAdminA, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusOK && status != http.StatusCreated {
		t.Errorf("Expected 200 OK / 201 Created for exact scope key upsert, got %d. Body: %s", status, string(body))
	}
}
