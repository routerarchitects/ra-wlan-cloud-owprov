package rbac_tests

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
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
 *   5. Negative: Request attempts to clear managementPolicy (empty string) -> Rejected.
 *      Expected Status: 400 Bad Request.
 *   6. Negative: Request attempts to set non-existent managementPolicy UUID -> Rejected.
 *      Expected Status: 400 Bad Request.
 */
func TestManagementRoleImmutability(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	roleID := getEnvOrDefault("ROLE_ID", "ed7ff809-20d3-48f4-8fe2-c882cd681657")
	rootToken := getEnvOrDefault("TEST_TOKEN", getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token"))

	validEntity := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	newEntity := getEnvOrDefault("OPERATOR_B_ENTITY_UUID", "8ab2b291-d04d-5c4c-b4bd-c4gcca1gb108")

	validUsers := []string{getEnvOrDefault("TARGET_USER_A", "e6885f03-63db-4e0d-aad4-2b8d1a79a887")}
	newUsers := []string{"different-user-uuid-999"}
	validPolicyID := getEnvOrDefault("POLICY_STRONG_ID", "6f0e350a-8b7b-4ae1-bbd7-5f559792bc95")

	// Resolve active role and valid policy from environment
	var roleResp struct {
		ID               string   `json:"id"`
		Entity           string   `json:"entity"`
		Venue            string   `json:"venue"`
		Users            []string `json:"users"`
		ManagementPolicy string   `json:"managementPolicy"`
	}
	if statusCheck, bodyRole, errCheck := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", roleID), rootToken, nil); errCheck == nil && statusCheck == http.StatusOK {
		if err := json.Unmarshal(bodyRole, &roleResp); err == nil {
			validEntity = roleResp.Entity
			validUsers = roleResp.Users
			if roleResp.ManagementPolicy != "" {
				validPolicyID = roleResp.ManagementPolicy
			}
		}
	} else if statusRoles, bodyRoles, errRoles := client.DoRequest("GET", "/managementRole", rootToken, nil); errRoles == nil && statusRoles == http.StatusOK {
		var rolesResp struct {
			Roles []struct {
				ID               string   `json:"id"`
				Entity           string   `json:"entity"`
				Venue            string   `json:"venue"`
				Users            []string `json:"users"`
				ManagementPolicy string   `json:"managementPolicy"`
			} `json:"roles"`
		}
		if err := json.Unmarshal(bodyRoles, &rolesResp); err == nil && len(rolesResp.Roles) > 0 {
			roleID = rolesResp.Roles[0].ID
			validEntity = rolesResp.Roles[0].Entity
			validUsers = rolesResp.Roles[0].Users
		}
	}

	// Ensure validPolicyID is an existing policy in PolicyDB
	if statusPolicies, bodyPolicies, errPolicies := client.DoRequest("GET", "/managementPolicy", rootToken, nil); errPolicies == nil && statusPolicies == http.StatusOK {
		var polResp struct {
			Policies []struct {
				ID string `json:"id"`
			} `json:"managementPolicies"`
		}
		if err := json.Unmarshal(bodyPolicies, &polResp); err == nil && len(polResp.Policies) > 0 {
			validPolicyID = polResp.Policies[0].ID
		}
	}
	t.Logf("Setup resolved: roleID=%s, validEntity=%s, validPolicyID=%s", roleID, validEntity, validPolicyID)

	t.Run("Positive: Updating only Policy ID allowed", func(t *testing.T) {
		payload := map[string]interface{}{
			"id":               roleID,
			"entity":           validEntity,
			"venue":            "",
			"users":            validUsers,
			"managementPolicy": validPolicyID,
		}

		status, body, err := client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), rootToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for policy ID update, got %d. Body: %s", status, string(body))
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

	t.Run("Negative: Updating managementPolicy to empty string returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"id":               roleID,
			"entity":           validEntity,
			"venue":            "",
			"users":            validUsers,
			"managementPolicy": "", // Empty!
		}

		status, body, err := client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), rootToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for empty managementPolicy, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Updating managementPolicy to non-existent UUID returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"id":               roleID,
			"entity":           validEntity,
			"venue":            "",
			"users":            validUsers,
			"managementPolicy": "00000000-0000-0000-0000-000000000000", // Non-existent!
		}

		status, body, err := client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), rootToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for non-existent managementPolicy, got %d. Body: %s", status, string(body))
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
 *   - If the policy is unreferenced (not assigned to any role), deletion succeeds with 200 OK.
 *   - If the policy is currently assigned to one or more Management Roles,
 *     the backend MUST reject the deletion request with 400 Bad Request (StillInUse).
 *
 * SCENARIOS TESTED:
 *   1. Positive: Deleting an unreferenced policy succeeds (200 OK).
 *   2. Negative: Deleting a policy assigned to an active Management Role is rejected (400 Bad Request).
 */
func TestManagementPolicyDeletionProtection(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	roleID := getEnvOrDefault("ROLE_ID", "ed7ff809-20d3-48f4-8fe2-c882cd681657")
	validEntity := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	validUsers := []string{getEnvOrDefault("TARGET_USER_A", "e6885f03-63db-4e0d-aad4-2b8d1a79a887")}
	assignedPolicyID := getEnvOrDefault("POLICY_STRONG_ID", "6f0e350a-8b7b-4ae1-bbd7-5f559792bc95")

	t.Run("Positive: Deleting unreferenced policy succeeds", func(t *testing.T) {
		tempPolicyID := "d4e5f6a7-b8c9-4012-9def-123456789abc"

		// 1. Create a standalone unreferenced policy
		policyPayload := map[string]interface{}{
			"id":          tempPolicyID,
			"name":        "test-unreferenced-policy",
			"description": "Temporary policy for testing deletion of unreferenced policy",
			"entries":     []map[string]interface{}{},
		}

		status, body, err := client.DoRequest("POST", fmt.Sprintf("/managementPolicy/%s", tempPolicyID), rootToken, policyPayload)
		if err != nil {
			t.Fatalf("Failed to create temporary policy: %v", err)
		}
		if status != http.StatusOK && status != http.StatusCreated {
			t.Fatalf("Setup failed: expected 200/201 on policy creation, got %d. Body: %s", status, string(body))
		}

		var createdPolicy struct {
			ID string `json:"id"`
		}
		if err := json.Unmarshal(body, &createdPolicy); err == nil && createdPolicy.ID != "" {
			tempPolicyID = createdPolicy.ID
		}

		// 2. Delete the unreferenced policy -> must succeed with 200 OK
		status, body, err = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", tempPolicyID), rootToken, nil)
		if err != nil {
			t.Fatalf("DELETE request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK when deleting unreferenced policy, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Deleting in-use policy returns 400 Bad Request", func(t *testing.T) {
		// Discover active role and policy from environment
		var roleResp struct {
			ID               string   `json:"id"`
			Entity           string   `json:"entity"`
			Venue            string   `json:"venue"`
			Users            []string `json:"users"`
			ManagementPolicy string   `json:"managementPolicy"`
		}
		if statusCheck, bodyRole, errCheck := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", roleID), rootToken, nil); errCheck == nil && statusCheck == http.StatusOK {
			if err := json.Unmarshal(bodyRole, &roleResp); err == nil {
				validEntity = roleResp.Entity
				validUsers = roleResp.Users
				if roleResp.ManagementPolicy != "" {
					assignedPolicyID = roleResp.ManagementPolicy
				}
			}
		} else if statusRoles, bodyRoles, errRoles := client.DoRequest("GET", "/managementRole", rootToken, nil); errRoles == nil && statusRoles == http.StatusOK {
			var rolesResp struct {
				Roles []struct {
					ID               string   `json:"id"`
					Entity           string   `json:"entity"`
					Venue            string   `json:"venue"`
					Users            []string `json:"users"`
					ManagementPolicy string   `json:"managementPolicy"`
				} `json:"roles"`
			}
			if err := json.Unmarshal(bodyRoles, &rolesResp); err == nil && len(rolesResp.Roles) > 0 {
				roleID = rolesResp.Roles[0].ID
				validEntity = rolesResp.Roles[0].Entity
				validUsers = rolesResp.Roles[0].Users
			}
		}

		// Ensure assignedPolicyID is an existing policy in PolicyDB
		if statusPolicies, bodyPolicies, errPolicies := client.DoRequest("GET", "/managementPolicy", rootToken, nil); errPolicies == nil && statusPolicies == http.StatusOK {
			var polResp struct {
				Policies []struct {
					ID string `json:"id"`
				} `json:"managementPolicies"`
			}
			if err := json.Unmarshal(bodyPolicies, &polResp); err == nil && len(polResp.Policies) > 0 {
				assignedPolicyID = polResp.Policies[0].ID
			}
		}

		// 1. Ensure the active role is explicitly linked to assignedPolicyID
		rolePayload := map[string]interface{}{
			"id":               roleID,
			"entity":           validEntity,
			"venue":            "",
			"users":            validUsers,
			"managementPolicy": assignedPolicyID,
		}
		status, body, err := client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), rootToken, rolePayload)
		if err != nil {
			t.Fatalf("Failed to link policy to role: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Setup failed: expected 200 OK linking policy to role, got %d. Body: %s", status, string(body))
		}

		// 2. Attempt to delete the policy currently in use -> must be rejected with 400 Bad Request
		status, body, err = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", assignedPolicyID), rootToken, nil)
		if err != nil {
			t.Fatalf("DELETE request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Fatalf("Expected 400 Bad Request for in-use policy deletion, got %d. Body: %s", status, string(body))
		}

		// Acceptance Criteria (Issue #72): Verify explicit error description for in-use policy deletion
		var errResp struct {
			ErrorCode        int    `json:"ErrorCode"`
			ErrorDescription string `json:"ErrorDescription"`
		}
		if err := json.Unmarshal(body, &errResp); err != nil {
			t.Fatalf("Failed to parse error response JSON: %v. Body: %s", err, string(body))
		}
		expectedErrSubstr := "Management policy is currently assigned to one or more management roles"
		if !strings.Contains(errResp.ErrorDescription, expectedErrSubstr) {
			t.Errorf("Expected ErrorDescription to contain %q, got: %q", expectedErrSubstr, errResp.ErrorDescription)
		}
	})
}
