package rbac_tests

import (
	"fmt"
	"net/http"
	"os"
	"testing"
)

// Helper to get env var with fallback default
func getEnvOrDefault(key, fallback string) string {
	if val := os.Getenv(key); val != "" {
		return val
	}
	return fallback
}

/*
===============================================================================
ENVIRONMENT VARIABLES CHEAT SHEET & EXPECTED USER BEHAVIOR
===============================================================================
Provide these environment variables when executing the test suite:

1. TOKEN_ROOT:
   - Bearer token of ROOT user.
   - Behavior: Bypasses role checks, can assign any policy/scope.

2. TOKEN_ADMIN_OPERATOR_A:
   - Bearer token of Admin user assigned to Operator A's Entity (OPERATOR_A_ENTITY_UUID).
   - Behavior: Can create roles for TARGET_USER_A on OPERATOR_A_ENTITY_UUID or VENUE_A_UUID
     with equal or weaker policy permissions. Cannot assign roles on OPERATOR_B_ENTITY_UUID.

3. TOKEN_ADMIN_OPERATOR_B:
   - Bearer token of Admin user assigned to Operator B's Entity (OPERATOR_B_ENTITY_UUID).

4. TOKEN_NO_ACCESS:
   - Bearer token of a user with NO permissions or missing inventory/subscriber permissions.

5. TARGET_USER_A:
   - User UUID created by TOKEN_ADMIN_OPERATOR_A (createdBy == AdminA.id).

6. TARGET_USER_B:
   - User UUID created by TOKEN_ADMIN_OPERATOR_B (createdBy == AdminB.id).

7. OPERATOR_A_ENTITY_UUID / OPERATOR_B_ENTITY_UUID:
   - Entity UUIDs of Operator A and Operator B.

8. VENUE_A_UUID / VENUE_B_UUID:
   - VENUE_A_UUID belongs to OPERATOR_A_ENTITY_UUID.
   - VENUE_B_UUID belongs to OPERATOR_B_ENTITY_UUID.

9. POLICY_WEAK_ID / POLICY_STRONG_ID:
   - POLICY_WEAK_ID: Grants READ access (e.g. subscriber: READ).
   - POLICY_STRONG_ID: Grants FULL access (e.g. subscriber: FULL, inventory: FULL).
===============================================================================
*/

// ----------------------------------------------------------------------------
// 1. ADVANCED ROLE ASSIGNMENT & CONTROLLED DELEGATION TESTS (Section 8)
// ----------------------------------------------------------------------------

/*
 * TestRoleAssignment_DelegationSubsetCheck
 *
 * DESCRIPTION:
 *   Validates Section 8 controlled delegation rule: An Admin cannot delegate permissions
 *   stronger than their own effective policy permissions on the target scope.
 *
 * SCENARIO:
 *   Admin A (holding POLICY_WEAK_ID with subscriber:READ) attempts to assign
 *   POLICY_STRONG_ID (with subscriber:FULL / inventory:FULL) to TARGET_USER_A.
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request with error description:
 *   "Privilege mismatch: requester does not have FULL permission on resource..."
 */
func TestRoleAssignment_DelegationSubsetCheck(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer b6ba0d38b3a438af80a65be2dab666ad95791091d5cfb72264f2a6d1d38e82cb")
	targetUserA := getEnvOrDefault("TARGET_USER_A", "1abbeb1a-a057-44fe-96b1-92b33dc8f0fe")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	strongPolicy := getEnvOrDefault("POLICY_STRONG_ID", "94bb61a3-aa3e-49aa-9704-cc254fec4482")

	payload := map[string]interface{}{
		"users":            []string{targetUserA},
		"entity":           entityA,
		"venue":            "",
		"managementPolicy": strongPolicy,
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", tokenAdminA, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest && status != http.StatusForbidden {
		t.Errorf("Expected 400 Bad Request for delegating stronger policy, got %d. Body: %s", status, string(body))
	}
}

/*
 * TestRoleAssignment_VenueBelongsToEntityCheck
 *
 * DESCRIPTION:
 *   Validates Section 8 scope validity check: When assigning a venue-scoped role,
 *   the backend verifies that the specified Venue actually belongs to the specified Entity.
 *
 * SCENARIO:
 *   Admin A attempts to assign a role on entity = OPERATOR_A_ENTITY_UUID
 *   and venue = VENUE_B_UUID (which belongs to Operator B).
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request (Venue does not belong to specified Entity).
 */
func TestRoleAssignment_VenueBelongsToEntityCheck(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	targetUserA := getEnvOrDefault("TARGET_USER_A", "user-created-by-admin-a-uuid")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	venueB := getEnvOrDefault("VENUE_B_UUID", "venue-belonging-to-operator-b")
	weakPolicy := getEnvOrDefault("POLICY_WEAK_ID", "94bb61a3-aa3e-49aa-9704-cc254fec4482")

	payload := map[string]interface{}{
		"users":            []string{targetUserA},
		"entity":           entityA,
		"venue":            venueB, // Cross-venue mismatch!
		"managementPolicy": weakPolicy,
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", tokenAdminA, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest {
		t.Errorf("Expected 400 Bad Request when venue does not belong to entity, got %d. Body: %s", status, string(body))
	}
}

/*
 * TestRoleAssignment_CreatedByOwnershipCheck
 *
 * DESCRIPTION:
 *   Validates Section 2.2 and Section 8 IAM user ownership rule:
 *   An Admin can assign roles ONLY to users created by that Admin (createdBy == Admin.id).
 *
 * SCENARIO:
 *   Admin A attempts to assign a role to TARGET_USER_B (created by Admin B).
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request with error description:
 *   "Missing or invalid parameters" / target user ownership mismatch.
 */
func TestRoleAssignment_CreatedByOwnershipCheck(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	targetUserB := getEnvOrDefault("TARGET_USER_B", "user-created-by-admin-b-uuid")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	weakPolicy := getEnvOrDefault("POLICY_WEAK_ID", "94bb61a3-aa3e-49aa-9704-cc254fec4482")

	payload := map[string]interface{}{
		"users":            []string{targetUserB}, // Not created by Admin A!
		"entity":           entityA,
		"venue":            "",
		"managementPolicy": weakPolicy,
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", tokenAdminA, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest && status != http.StatusForbidden {
		t.Errorf("Expected 400 Bad Request for un-owned user assignment, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 2. SUBSCRIBER EDGE CASES (Section 11.2)
// ----------------------------------------------------------------------------

/*
 * TestSubscriber_MissingCreatePermission
 *
 * DESCRIPTION:
 *   Validates Section 11.2 subscriber authorization:
 *   Creating a subscriber requires subscriber:CREATE or subscriber:FULL permission.
 *   If a user has role on Operator A Entity but policy grants only subscriber:READ,
 *   POST /api/v1/subscriber must be denied.
 *
 * SCENARIO:
 *   User with subscriber:READ policy attempts POST /subscriber?registrationId=111.
 *
 * EXPECTED OUTPUT:
 *   HTTP 403 Access Denied.
 */
func TestSubscriber_MissingCreatePermission(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenReadOnlyUser := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-subscriber-token")
	regIdA := getEnvOrDefault("OPERATOR_A_REG_ID", "111")

	payload := map[string]string{
		"email":          "read-only-test@gmail.com",
		"registrationId": regIdA,
	}

	status, body, err := client.DoRequest("POST", fmt.Sprintf("/subscriber?email=read-only-test@gmail.com&registrationId=%s", regIdA), tokenReadOnlyUser, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusForbidden {
		t.Errorf("Expected 403 Access Denied for missing subscriber:CREATE permission, got %d. Body: %s", status, string(body))
	}
}
