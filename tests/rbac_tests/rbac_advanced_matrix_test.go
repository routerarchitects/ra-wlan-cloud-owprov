package rbac_tests

import (
	"fmt"
	"net/http"
	"testing"
)

// ----------------------------------------------------------------------------
// 1. VENUE SCOPE PRECEDENCE & SHADOWING TESTS (Section 7.2)
// ----------------------------------------------------------------------------

/*
 * TestVenueScopePrecedence_ExactVenueShadowsEntity
 *
 * DESCRIPTION:
 *   Validates Section 7.2 Effective Role Precedence:
 *   If an exact Venue-scoped role exists for a user on Venue A1, it is the ONLY effective role.
 *   The system MUST NOT fall back to the owning Entity role after selecting an exact Venue role.
 *
 * SCENARIO:
 *   User has an Entity-scoped role granting inventory:READ on Entity A.
 *   User also has an exact Venue-scoped role on Venue A1 granting inventory:NOACCESS.
 *   User requests subscriber devices / inventory on Venue A1.
 *
 * EXPECTED OUTPUT:
 *   HTTP 403 Access Denied (Exact Venue A1 role is authoritative and shadows Entity A role).
 */
func TestVenueScopePrecedence_ExactVenueShadowsEntity(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenShadowedUser := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-venue-shadowed-token")
	venueA1 := getEnvOrDefault("VENUE_A_UUID", "venue-a1-uuid")

	status, body, err := client.DoRequest("GET", fmt.Sprintf("/inventory?venue=%s", venueA1), tokenShadowedUser, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusForbidden {
		t.Errorf("Expected 403 Access Denied (Exact Venue role shadows Entity role), got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 2. POLICY DELETION CONSTRAINT TESTS (Section 5.1)
// ----------------------------------------------------------------------------

/*
 * TestPolicyDelete_ReferencedPolicyCannotBeDeleted
 *
 * DESCRIPTION:
 *   Validates Section 5.1 policy administration rule:
 *   A Management Policy that is actively referenced by any Management Role CANNOT be deleted.
 *
 * SCENARIO:
 *   ROOT or Admin attempts DELETE /api/v1/managementPolicy/{POLICY_ADMIN_ID} 
 *   where POLICY_ADMIN_ID is assigned to active management roles.
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request or 409 Conflict with error description: "Policy in use / referenced by role".
 */
func TestPolicyDelete_ReferencedPolicyCannotBeDeleted(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenRoot := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	activePolicyID := getEnvOrDefault("POLICY_STRONG_ID", "6f0e350a-8b7b-4ae1-bbd7-5f559792bc95")

	status, body, err := client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", activePolicyID), tokenRoot, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest && status != http.StatusConflict && status != http.StatusForbidden {
		t.Errorf("Expected 400/409/403 for deleting in-use policy, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 3. ENTITY HIERARCHY NESTING CONSTRAINT TESTS (Section 4.4)
// ----------------------------------------------------------------------------

/*
 * TestEntityCreation_NormalEntityUnderNormalEntityDisallowed
 *
 * DESCRIPTION:
 *   Validates Section 4.4 Entity type hierarchy rule:
 *   A Normal Entity CANNOT be created beneath another Normal Entity.
 *   (Supported levels: Root Entity -> Operator Entity -> Normal Entity, or Root Entity -> Normal Entity).
 *
 * SCENARIO:
 *   User attempts to create a new Normal Entity specifying parent entity = NormalEntity_UUID.
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request (Invalid parent entity type).
 */
func TestEntityCreation_NormalEntityUnderNormalEntityDisallowed(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenRoot := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	normalEntityUUID := getEnvOrDefault("NORMAL_ENTITY_UUID", "normal-entity-parent-uuid")

	payload := map[string]interface{}{
		"name":   "Invalid Nested Normal Entity",
		"parent": normalEntityUUID, // Normal entity parent is disallowed for another normal entity!
	}

	status, body, err := client.DoRequest("POST", "/entity/0", tokenRoot, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest {
		t.Errorf("Expected 400 Bad Request for creating Normal Entity under Normal Entity, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 4. UNOWNED ROLE DELETION TESTS (Section 6.5)
// ----------------------------------------------------------------------------

/*
 * TestManagementRoleDelete_UnownedUserRoleDeletionBlocked
 *
 * DESCRIPTION:
 *   Validates Section 6.5 lifecycle rule for Admins:
 *   An Admin can delete roles ONLY for users created by that Admin (createdBy == Admin.id).
 *
 * SCENARIO:
 *   Admin A attempts DELETE /api/v1/managementRole/{ROLE_OF_TARGET_USER_B_ID}.
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request or 403 Access Denied.
 */
func TestManagementRoleDelete_UnownedUserRoleDeletionBlocked(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	targetUserBRoleID := getEnvOrDefault("TARGET_USER_B_ROLE_ID", "role-assigned-to-user-b")

	status, body, err := client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", targetUserBRoleID), tokenAdminA, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest && status != http.StatusForbidden {
		t.Errorf("Expected 400/403 when Admin A attempts deleting role of User B, got %d. Body: %s", status, string(body))
	}
}
