package rbac_tests

import (
	"fmt"
	"net/http"
	"testing"
)

// ----------------------------------------------------------------------------
// 3. MANAGEMENT ROLE IMMUTABILITY TESTS (PUT /api/v1/managementRole/{id})
// ----------------------------------------------------------------------------

/*
 * TestManagementRoleImmutability
 *
 * DESCRIPTION:
 *   Validates Management Role immutability rules according to Section 6.4 of the Specification.
 *   On PUT /api/v1/managementRole/{id}:
 *   - Only managementPolicy (Policy ID) or role metadata (name/description) may be updated.
 *   - Scope fields (entity, venue, users) are strictly IMMUTABLE.
 *
 * SCENARIOS TESTED:
 *   1. Positive: Request updates only managementPolicy ID -> Operation succeeds.
 *      Expected Status: 200 OK.
 *   2. Negative: Request attempts to modify entity UUID -> Rejected with error message.
 *      Expected Status: 400 Bad Request ("Entity ID, Venue ID, and User ID are immutable").
 *   3. Negative: Request attempts to modify venue UUID -> Rejected with error message.
 *      Expected Status: 400 Bad Request.
 *   4. Negative: Request attempts to modify users list -> Rejected with error message.
 *      Expected Status: 400 Bad Request.
 */
func TestManagementRoleImmutability(t *testing.T) {
	client := NewTestClient("https://openwifi.wlan.local:16005/api/v1")

	roleID := "ed7ff809-20d3-48f4-8fe2-c882cd681657"
	rootToken := "Bearer root-test-token"

	validEntity := "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097"
	newEntity := "8ab2b291-d04d-5c4c-b4bd-c4gcca1gb108"

	validUsers := []string{"e6885f03-63db-4e0d-aad4-2b8d1a79a887"}
	newUsers := []string{"different-user-uuid-999"}

	t.Run("Positive: Updating only Policy ID allowed", func(t *testing.T) {
		payload := map[string]interface{}{
			"id":               roleID,
			"entity":           validEntity,
			"venue":            "",
			"users":            validUsers,
			"managementPolicy": "6f0e350a-8b7b-4ae1-bbd7-5f559792bc95",
		}

		status, _, err := client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), rootToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for policy ID update, got %d", status)
		}
	})

	t.Run("Negative: Modifying Entity ID returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"id":               roleID,
			"entity":           newEntity, // Modified!
			"venue":            "",
			"users":            validUsers,
			"managementPolicy": "6f0e350a-8b7b-4ae1-bbd7-5f559792bc95",
		}

		status, body, err := client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), rootToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for immutable entity modification, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Modifying Venue ID returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"id":               roleID,
			"entity":           validEntity,
			"venue":            "new-venue-uuid-123", // Modified!
			"users":            validUsers,
			"managementPolicy": "6f0e350a-8b7b-4ae1-bbd7-5f559792bc95",
		}

		status, body, err := client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), rootToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for immutable venue modification, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Modifying Users array returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"id":               roleID,
			"entity":           validEntity,
			"venue":            "",
			"users":            newUsers, // Modified!
			"managementPolicy": "6f0e350a-8b7b-4ae1-bbd7-5f559792bc95",
		}

		status, body, err := client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), rootToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for immutable users modification, got %d. Body: %s", status, string(body))
		}
	})
}

// ----------------------------------------------------------------------------
// 4. MANAGEMENT POLICY DELETION PROTECTION TESTS (DELETE /api/v1/managementPolicy/{id})
// ----------------------------------------------------------------------------

/*
 * TestManagementPolicyDeletionProtection
 *
 * DESCRIPTION:
 *   Validates Management Policy deletion protection rules.
 *   On DELETE /api/v1/managementPolicy/{id}:
 *   - If the policy is currently assigned to one or more Management Roles,
 *     the backend MUST reject the deletion request with 400 Bad Request (StillInUse).
 *   - Policies can only be safely deleted when no Management Roles reference them.
 *
 * SCENARIOS TESTED:
 *   1. Negative: Attempting to delete a policy assigned to an active Management Role
 *      Expected Status: 400 Bad Request ("Management policy is currently assigned to one or more management roles.").
 */
func TestManagementPolicyDeletionProtection(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	assignedPolicyID := getEnvOrDefault("POLICY_STRONG_ID", "6f0e350a-8b7b-4ae1-bbd7-5f559792bc95")

	t.Run("Negative: Deleting in-use policy returns 400 Bad Request", func(t *testing.T) {
		status, body, err := client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", assignedPolicyID), rootToken, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for in-use policy deletion, got %d. Body: %s", status, string(body))
		}
	})
}
