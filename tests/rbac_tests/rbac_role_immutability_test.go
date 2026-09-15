package rbac_tests

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"testing"
)

// ----------------------------------------------------------------------------
// 3. MANAGEMENT ROLE IMMUTABILITY & POLICY VALIDATION TESTS
// ----------------------------------------------------------------------------

/*
 * TestManagementRoleImmutability
 *
 * DESCRIPTION:
 *   Validates Management Role immutability rules according to Section 6.4 of the Specification.
 *   On PUT /api/v1/managementRole/{id}:
 *   - Only managementPolicy (Policy ID) or role metadata (name/description) may be updated.
 *   - Scope fields (entity, venue, users) are strictly IMMUTABLE.
 *   - managementPolicy MUST NOT be empty or reference a non-existent policy UUID.
 *
 * SCENARIOS TESTED:
 *   1. Positive: Updating only Policy ID allowed (200 OK).
 *   2. Negative: Modifying Entity ID returns 400 Bad Request.
 *   3. Negative: Modifying Venue ID returns 400 Bad Request.
 *   4. Negative: Modifying Users array returns 400 Bad Request.
 *   5. Negative: Updating managementPolicy to empty string returns 400 Bad Request.
 *   6. Negative: Updating managementPolicy to non-existent UUID returns 400 Bad Request.
 *   7. Negative: Updating managementPolicy to invalid/malformed string returns 400 Bad Request.
 */
func TestManagementRoleImmutability(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")

	roleID := getEnvOrDefault("ROLE_ID", "ed7ff809-20d3-48f4-8fe2-c882cd681657")
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
	validVenue := ""
	if statusCheck, bodyRole, errCheck := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", roleID), rootToken, nil); errCheck == nil && statusCheck == http.StatusOK {
		if err := json.Unmarshal(bodyRole, &roleResp); err == nil {
			validEntity = roleResp.Entity
			validVenue = roleResp.Venue
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
			validVenue = rolesResp.Roles[0].Venue
			validUsers = rolesResp.Roles[0].Users
			if rolesResp.Roles[0].ManagementPolicy != "" {
				validPolicyID = rolesResp.Roles[0].ManagementPolicy
			}
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
	if statusPol, _, errPol := client.DoRequest("GET", fmt.Sprintf("/managementPolicy/%s", validPolicyID), rootToken, nil); errPol != nil || statusPol != http.StatusOK {
		polPayload := map[string]interface{}{
			"id":          validPolicyID,
			"name":        "test-immutability-policy",
			"description": "Test policy for immutability tests",
			"entries":     []map[string]interface{}{},
		}
		statusCreatePol, bodyCreatePol, _ := client.DoRequest("POST", fmt.Sprintf("/managementPolicy/%s", validPolicyID), rootToken, polPayload)
		if statusCreatePol == http.StatusOK || statusCreatePol == http.StatusCreated {
			var createdPol struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal(bodyCreatePol, &createdPol); err == nil && createdPol.ID != "" {
				validPolicyID = createdPol.ID
			}
		}
	}

	// Ensure validEntity is an existing entity in EntityDB
	if statusEnt, _, errEnt := client.DoRequest("GET", fmt.Sprintf("/entity/%s", validEntity), rootToken, nil); errEnt != nil || statusEnt != http.StatusOK {
		var entResp struct {
			Entities []struct {
				ID string `json:"id"`
			} `json:"entities"`
		}
		if statusEntList, bodyEntList, errEntList := client.DoRequest("GET", "/entity", rootToken, nil); errEntList == nil && statusEntList == http.StatusOK {
			if err := json.Unmarshal(bodyEntList, &entResp); err == nil && len(entResp.Entities) > 0 {
				validEntity = entResp.Entities[0].ID
			}
		}
	}

	// Ensure roleID exists in ManagementRoleDB
	if statusRole, _, errRole := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", roleID), rootToken, nil); errRole != nil || statusRole != http.StatusOK {
		rolePayload := map[string]interface{}{
			"id":               roleID,
			"name":             "test-immutability-role",
			"entity":           validEntity,
			"venue":            validVenue,
			"users":            validUsers,
			"managementPolicy": validPolicyID,
		}
		statusCreateRole, bodyCreateRole, _ := client.DoRequest("POST", fmt.Sprintf("/managementRole/%s", roleID), rootToken, rolePayload)
		if statusCreateRole == http.StatusOK || statusCreateRole == http.StatusCreated {
			var createdRole struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal(bodyCreateRole, &createdRole); err == nil && createdRole.ID != "" {
				roleID = createdRole.ID
			}
		}
	}

	t.Logf("Setup resolved: roleID=%s, validEntity=%s, validVenue=%s, validPolicyID=%s", roleID, validEntity, validVenue, validPolicyID)

	t.Run("Positive: Updating only Policy ID allowed", func(t *testing.T) {
		payload := map[string]interface{}{
			"id":               roleID,
			"entity":           validEntity,
			"venue":            validVenue,
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
			"venue":            validVenue,
			"users":            validUsers,
			"managementPolicy": validPolicyID,
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
			"venue":            validVenue + "-modified", // Modified!
			"users":            validUsers,
			"managementPolicy": validPolicyID,
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
			"venue":            validVenue,
			"users":            newUsers, // Modified!
			"managementPolicy": validPolicyID,
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
			"venue":            validVenue,
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
			"venue":            validVenue,
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

	t.Run("Negative: Updating managementPolicy with malformed string returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"id":               roleID,
			"entity":           validEntity,
			"venue":            validVenue,
			"users":            validUsers,
			"managementPolicy": "invalid!!uuid##@@$$", // Malformed
		}

		status, body, err := client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), rootToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for malformed managementPolicy, got %d. Body: %s", status, string(body))
		}
	})
}

// ----------------------------------------------------------------------------
// 4. MANAGEMENT ROLE POLICY CREATION VALIDATION (POST /api/v1/managementRole/{id})
// ----------------------------------------------------------------------------

func TestManagementRoleCreationPolicyValidation(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	validEntity := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	validUsers := []string{getEnvOrDefault("TARGET_USER_A", "e6885f03-63db-4e0d-aad4-2b8d1a79a887")}

	// Ensure validEntity is an existing entity in EntityDB
	if statusEnt, _, errEnt := client.DoRequest("GET", fmt.Sprintf("/entity/%s", validEntity), rootToken, nil); errEnt != nil || statusEnt != http.StatusOK {
		var entResp struct {
			Entities []struct {
				ID string `json:"id"`
			} `json:"entities"`
		}
		if statusEntList, bodyEntList, errEntList := client.DoRequest("GET", "/entity", rootToken, nil); errEntList == nil && statusEntList == http.StatusOK {
			if err := json.Unmarshal(bodyEntList, &entResp); err == nil && len(entResp.Entities) > 0 {
				validEntity = entResp.Entities[0].ID
			}
		}
	}

	t.Run("Negative: Creating role with empty managementPolicy returns 400 Bad Request", func(t *testing.T) {
		newRoleID := "ffffffff-1111-2222-3333-000000000001"
		payload := map[string]interface{}{
			"id":               newRoleID,
			"name":             "test-empty-policy-role",
			"entity":           validEntity,
			"venue":            "",
			"users":            validUsers,
			"managementPolicy": "", // Empty policy!
		}

		status, body, err := client.DoRequest("POST", fmt.Sprintf("/managementRole/%s", newRoleID), rootToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for role creation with empty managementPolicy, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Creating role with non-existent managementPolicy returns 400 Bad Request", func(t *testing.T) {
		newRoleID := "ffffffff-1111-2222-3333-000000000002"
		payload := map[string]interface{}{
			"id":               newRoleID,
			"name":             "test-nonexistent-policy-role",
			"entity":           validEntity,
			"venue":            "",
			"users":            validUsers,
			"managementPolicy": "00000000-0000-0000-0000-000000000000", // Non-existent!
		}

		status, body, err := client.DoRequest("POST", fmt.Sprintf("/managementRole/%s", newRoleID), rootToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for role creation with non-existent managementPolicy, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Creating role with omitted managementPolicy field returns 400 Bad Request", func(t *testing.T) {
		newRoleID := "ffffffff-1111-2222-3333-000000000003"
		payload := map[string]interface{}{
			"id":     newRoleID,
			"name":   "test-omitted-policy-role",
			"entity": validEntity,
			"venue":  "",
			"users":  validUsers,
			// managementPolicy is completely omitted!
		}

		status, body, err := client.DoRequest("POST", fmt.Sprintf("/managementRole/%s", newRoleID), rootToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for role creation with omitted managementPolicy, got %d. Body: %s", status, string(body))
		}
	})
}

// ----------------------------------------------------------------------------
// 5. MANAGEMENT POLICY DELETION PROTECTION TESTS (DELETE /api/v1/managementPolicy/{id})
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
 *   - Non-root users cannot delete management policies (401/403).
 *   - Deleting a non-existent policy UUID returns 404 Not Found.
 *
 * SCENARIOS TESTED:
 *   1. Positive: Deleting an unreferenced policy succeeds (200 OK).
 *   2. Negative: Deleting a policy assigned to an active Management Role is rejected (400 Bad Request).
 *   3. Negative: Deleting an in-use policy with non-root token returns 401/403.
 *   4. Negative: Deleting non-existent policy returns 404 Not Found.
 *   5. Positive: Deleting policy after reassigning role to another policy succeeds (200 OK).
 */
func TestManagementPolicyDeletionProtection(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	tokenNoAccess := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-token")

	roleID := getEnvOrDefault("ROLE_ID", "ed7ff809-20d3-48f4-8fe2-c882cd681657")
	validEntity := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	validVenue := ""
	validUsers := []string{getEnvOrDefault("TARGET_USER_A", "e6885f03-63db-4e0d-aad4-2b8d1a79a887")}
	assignedPolicyID := getEnvOrDefault("POLICY_STRONG_ID", "6f0e350a-8b7b-4ae1-bbd7-5f559792bc95")

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
			validVenue = roleResp.Venue
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
			validVenue = rolesResp.Roles[0].Venue
			validUsers = rolesResp.Roles[0].Users
			if rolesResp.Roles[0].ManagementPolicy != "" {
				assignedPolicyID = rolesResp.Roles[0].ManagementPolicy
			}
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
	if statusPol, _, errPol := client.DoRequest("GET", fmt.Sprintf("/managementPolicy/%s", assignedPolicyID), rootToken, nil); errPol != nil || statusPol != http.StatusOK {
		polPayload := map[string]interface{}{
			"id":          assignedPolicyID,
			"name":        "test-deletion-protection-policy",
			"description": "Test policy for deletion protection tests",
			"entries":     []map[string]interface{}{},
		}
		statusCreatePol, bodyCreatePol, _ := client.DoRequest("POST", fmt.Sprintf("/managementPolicy/%s", assignedPolicyID), rootToken, polPayload)
		if statusCreatePol == http.StatusOK || statusCreatePol == http.StatusCreated {
			var createdPol struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal(bodyCreatePol, &createdPol); err == nil && createdPol.ID != "" {
				assignedPolicyID = createdPol.ID
			}
		}
	}

	// Ensure validEntity is an existing entity in EntityDB
	if statusEnt, _, errEnt := client.DoRequest("GET", fmt.Sprintf("/entity/%s", validEntity), rootToken, nil); errEnt != nil || statusEnt != http.StatusOK {
		var entResp struct {
			Entities []struct {
				ID string `json:"id"`
			} `json:"entities"`
		}
		if statusEntList, bodyEntList, errEntList := client.DoRequest("GET", "/entity", rootToken, nil); errEntList == nil && statusEntList == http.StatusOK {
			if err := json.Unmarshal(bodyEntList, &entResp); err == nil && len(entResp.Entities) > 0 {
				validEntity = entResp.Entities[0].ID
			}
		}
	}

	// Ensure roleID exists in ManagementRoleDB
	if statusRole, _, errRole := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", roleID), rootToken, nil); errRole != nil || statusRole != http.StatusOK {
		rolePayload := map[string]interface{}{
			"id":               roleID,
			"name":             "test-deletion-protection-role",
			"entity":           validEntity,
			"venue":            validVenue,
			"users":            validUsers,
			"managementPolicy": assignedPolicyID,
		}
		statusCreateRole, bodyCreateRole, _ := client.DoRequest("POST", fmt.Sprintf("/managementRole/%s", roleID), rootToken, rolePayload)
		if statusCreateRole == http.StatusOK || statusCreateRole == http.StatusCreated {
			var createdRole struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal(bodyCreateRole, &createdRole); err == nil && createdRole.ID != "" {
				roleID = createdRole.ID
			}
		}
	}

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
		// 1. Ensure the active role is explicitly linked to assignedPolicyID
		rolePayload := map[string]interface{}{
			"id":               roleID,
			"entity":           validEntity,
			"venue":            validVenue,
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

	t.Run("Negative: Non-root user cannot delete management policy", func(t *testing.T) {
		status, _, err := client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", assignedPolicyID), tokenNoAccess, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusUnauthorized && status != http.StatusForbidden {
			t.Errorf("Expected 401 or 403 for non-root policy deletion, got %d", status)
		}
	})

	t.Run("Negative: Deleting non-existent policy returns 404 Not Found", func(t *testing.T) {
		nonExistentUUID := "00000000-dead-beef-0000-000000000000"
		status, _, err := client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", nonExistentUUID), rootToken, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusNotFound {
			t.Errorf("Expected 404 Not Found for non-existent policy deletion, got %d", status)
		}
	})

	t.Run("Positive: Deleting policy after reassigning role succeeds", func(t *testing.T) {
		tempPolicyID := "e5f6a7b8-c9d0-4123-aef0-234567890def"
		fallbackPolicyID := assignedPolicyID

		// 1. Create a temporary policy
		policyPayload := map[string]interface{}{
			"id":          tempPolicyID,
			"name":        "test-temporary-assigned-policy",
			"description": "Policy temporarily assigned then reassigned",
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

		// 2. Assign temporary policy to role
		rolePayload := map[string]interface{}{
			"id":               roleID,
			"entity":           validEntity,
			"venue":            validVenue,
			"users":            validUsers,
			"managementPolicy": tempPolicyID,
		}
		status, body, err = client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), rootToken, rolePayload)
		if err != nil || status != http.StatusOK {
			t.Fatalf("Failed to assign temporary policy to role: status=%d, body=%s, err=%v", status, string(body), err)
		}

		// 3. Verify policy cannot be deleted while assigned
		status, _, err = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", tempPolicyID), rootToken, nil)
		if err != nil || status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request while assigned, got %d", status)
		}

		// 4. Reassign role back to fallbackPolicyID
		rolePayload["managementPolicy"] = fallbackPolicyID
		status, body, err = client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), rootToken, rolePayload)
		if err != nil || status != http.StatusOK {
			t.Fatalf("Failed to reassign role to fallback policy: status=%d, body=%s, err=%v", status, string(body), err)
		}

		// 5. Now delete the unlinked temporary policy -> must succeed with 200 OK
		status, body, err = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", tempPolicyID), rootToken, nil)
		if err != nil {
			t.Fatalf("DELETE request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK after role reassignment, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Policy assigned to multiple roles cannot be deleted until all roles detached", func(t *testing.T) {
		multiPolicyID := "b1b2b3b4-c5c6-4789-9abc-0123456789ab"
		roleA_ID := "aaaaaaaa-1111-2222-3333-000000000001"
		roleB_ID := "bbbbbbbb-1111-2222-3333-000000000002"

		// 1. Create a policy
		policyPayload := map[string]interface{}{
			"id":          multiPolicyID,
			"name":        "test-multi-role-policy",
			"description": "Policy referenced by two distinct roles",
			"entries":     []map[string]interface{}{},
		}
		status, body, err := client.DoRequest("POST", fmt.Sprintf("/managementPolicy/%s", multiPolicyID), rootToken, policyPayload)
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			t.Fatalf("Failed to create multi-role test policy: status=%d, body=%s, err=%v", status, string(body), err)
		}

		var createdMultiPolicy struct {
			ID string `json:"id"`
		}
		if err := json.Unmarshal(body, &createdMultiPolicy); err == nil && createdMultiPolicy.ID != "" {
			multiPolicyID = createdMultiPolicy.ID
		}

		// 2. Create Role A using this policy
		roleA := map[string]interface{}{
			"id":               roleA_ID,
			"name":             "role-a-multi",
			"entity":           validEntity,
			"venue":            validVenue,
			"users":            validUsers,
			"managementPolicy": multiPolicyID,
		}
		status, body, err = client.DoRequest("POST", fmt.Sprintf("/managementRole/%s", roleA_ID), rootToken, roleA)
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", multiPolicyID), rootToken, nil)
			t.Fatalf("Failed to create Role A for multi-role test: status=%d, body=%s, err=%v", status, string(body), err)
		}
		var createdRoleA struct {
			ID string `json:"id"`
		}
		if err := json.Unmarshal(body, &createdRoleA); err == nil && createdRoleA.ID != "" {
			roleA_ID = createdRoleA.ID
		}

		// 3. Create Role B using this policy (with different user or scope)
		roleB := map[string]interface{}{
			"id":               roleB_ID,
			"name":             "role-b-multi",
			"entity":           validEntity,
			"venue":            validVenue,
			"users":            []string{"user-secondary-uuid-888"},
			"managementPolicy": multiPolicyID,
		}
		status, body, err = client.DoRequest("POST", fmt.Sprintf("/managementRole/%s", roleB_ID), rootToken, roleB)
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", roleA_ID), rootToken, nil)
			client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", multiPolicyID), rootToken, nil)
			t.Fatalf("Failed to create Role B for multi-role test: status=%d, body=%s, err=%v", status, string(body), err)
		}
		var createdRoleB struct {
			ID string `json:"id"`
		}
		if err := json.Unmarshal(body, &createdRoleB); err == nil && createdRoleB.ID != "" {
			roleB_ID = createdRoleB.ID
		}

		// 4. Verify policy cannot be deleted while both roles exist
		status, _, err = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", multiPolicyID), rootToken, nil)
		if err != nil || status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request when 2 roles reference policy, got %d", status)
		}

		// 5. Delete Role A
		client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", roleA_ID), rootToken, nil)

		// 6. Verify policy STILL cannot be deleted because Role B still references it
		status, _, err = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", multiPolicyID), rootToken, nil)
		if err != nil || status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request when Role B still references policy, got %d", status)
		}

		// 7. Delete Role B
		client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", roleB_ID), rootToken, nil)

		// 8. Now deletion of the policy MUST succeed
		status, body, err = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", multiPolicyID), rootToken, nil)
		if err != nil || status != http.StatusOK {
			t.Errorf("Expected 200 OK after all referencing roles deleted, got %d. Body: %s", status, string(body))
		}
	})
}
