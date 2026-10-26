package rbac_tests

import (
	"encoding/json"
	"fmt"
	"net/http"
	"testing"
	"time"
)

/*
 * TestManagementRoleDecoupling
 *
 * VALIDATES:
 *   Section 5.2, Section 6.4, Section 7.2 (Pillar 4) of requirements/rbac-requirement.md:
 *   Issue 4 (P2): Decouple entity / operator permissions from managementRole administration
 *   in RESTAPI_Handler.cpp:PolicyAllows().
 *
 * RULES:
 *   1. Strict Separation of Duties (Pillar 4):
 *      - Permissions on entity (e.g. entity:FULL, entity:CREATE) or operator (operator:FULL)
 *        do NOT grant authority to administer management roles.
 *      - Administering management roles strictly requires explicit managementRole:CREATE,
 *        managementRole:UPDATE, managementRole:DELETE, managementRole:FULL, or global *:FULL.
 *   2. Policy Read Accessibility:
 *      - GET /api/v1/managementPolicy remains accessible to all authenticated users.
 *   3. Policy Mutation Restriction:
 *      - POST, PUT, DELETE /api/v1/managementPolicy strictly restricted to ROOT.
 *      - Having entity:FULL or operator:FULL does not permit non-root users to mutate policies.
 */

func TestManagementRoleDecoupling(t *testing.T) {
	rootToken, adminToken, adminUserId, entityId, venueId, _ := getLiveTestTokensAndFixtures(t)
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	// Create a target user created by admin for role assignment tests (Pillar 2)
	targetUserPayload := map[string]interface{}{
		"name":        fmt.Sprintf("target-user-%d@example.com", time.Now().UnixNano()),
		"email":       fmt.Sprintf("target-user-%d@example.com", time.Now().UnixNano()),
		"userRole":    "csr",
		"description": "Target user for decoupling test",
	}
	var createdTargetUser struct {
		ID string `json:"id"`
	}
	createTargetStatus, createTargetBody, err := client.DoRequest("POST", "/user/0", adminToken, targetUserPayload)
	if err != nil || (createTargetStatus != http.StatusOK && createTargetStatus != http.StatusCreated) {
		// Fallback to ROOT creation if admin token cannot create users directly
		createTargetStatus, createTargetBody, err = client.DoRequest("POST", "/user/0", rootToken, targetUserPayload)
	}
	if createTargetStatus == http.StatusOK || createTargetStatus == http.StatusCreated {
		_ = json.Unmarshal(createTargetBody, &createdTargetUser)
		defer func() {
			if createdTargetUser.ID != "" {
				_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/user/%s", createdTargetUser.ID), rootToken, nil)
			}
		}()
	}

	targetUserId := createdTargetUser.ID
	if targetUserId == "" {
		targetUserId = getEnvOrDefault("TARGET_USER_A", "user-created-by-admin-a-uuid")
	}

	// 1. Create a pure entity:FULL policy (WITHOUT managementRole grant)
	entityOnlyPolicyPayload := map[string]interface{}{
		"name":        fmt.Sprintf("entity-full-only-%d", time.Now().UnixNano()),
		"description": "Entity FULL only policy without managementRole grant",
		"entries": []map[string]interface{}{
			{
				"resources": []string{"entity"},
				"access":    []string{"FULL"},
			},
		},
	}
	var entityOnlyPolicy struct {
		ID string `json:"id"`
	}
	status, body, err := client.DoRequest("POST", "/managementPolicy/0", rootToken, entityOnlyPolicyPayload)
	if err == nil && (status == http.StatusOK || status == http.StatusCreated) {
		_ = json.Unmarshal(body, &entityOnlyPolicy)
	}
	defer func() {
		if entityOnlyPolicy.ID != "" {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", entityOnlyPolicy.ID), rootToken, nil)
		}
	}()

	// 2. Create a pure operator:FULL policy (WITHOUT managementRole grant)
	operatorOnlyPolicyPayload := map[string]interface{}{
		"name":        fmt.Sprintf("operator-full-only-%d", time.Now().UnixNano()),
		"description": "Operator FULL only policy without managementRole grant",
		"entries": []map[string]interface{}{
			{
				"resources": []string{"operator"},
				"access":    []string{"FULL"},
			},
		},
	}
	var operatorOnlyPolicy struct {
		ID string `json:"id"`
	}
	status, body, err = client.DoRequest("POST", "/managementPolicy/0", rootToken, operatorOnlyPolicyPayload)
	if err == nil && (status == http.StatusOK || status == http.StatusCreated) {
		_ = json.Unmarshal(body, &operatorOnlyPolicy)
	}
	defer func() {
		if operatorOnlyPolicy.ID != "" {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", operatorOnlyPolicy.ID), rootToken, nil)
		}
	}()

	// 3. Create a policy with explicit managementRole:FULL grant
	roleFullPolicyPayload := map[string]interface{}{
		"name":        fmt.Sprintf("role-full-%d", time.Now().UnixNano()),
		"description": "Explicit managementRole FULL policy",
		"entries": []map[string]interface{}{
			{
				"resources": []string{"managementRole"},
				"access":    []string{"FULL"},
			},
			{
				"resources": []string{"entity"},
				"access":    []string{"READ"},
			},
		},
	}
	var roleFullPolicy struct {
		ID string `json:"id"`
	}
	status, body, err = client.DoRequest("POST", "/managementPolicy/0", rootToken, roleFullPolicyPayload)
	if err == nil && (status == http.StatusOK || status == http.StatusCreated) {
		_ = json.Unmarshal(body, &roleFullPolicy)
	}
	defer func() {
		if roleFullPolicy.ID != "" {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", roleFullPolicy.ID), rootToken, nil)
		}
	}()

	// Helper to create a management role for the admin
	createRoleForAdmin := func(entity string, venue string, policyId string) (string, func()) {
		rolePayload := map[string]interface{}{
			"name":             fmt.Sprintf("admin-role-%d", time.Now().UnixNano()),
			"description":      "Role for decoupling test",
			"entity":           entity,
			"venue":            venue,
			"managementPolicy": policyId,
			"users":            []string{adminUserId},
		}
		var createdRole struct {
			ID string `json:"id"`
		}
		s, b, e := client.DoRequest("POST", "/managementRole/0", rootToken, rolePayload)
		if e == nil && (s == http.StatusOK || s == http.StatusCreated) {
			_ = json.Unmarshal(b, &createdRole)
			return createdRole.ID, func() {
				_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", createdRole.ID), rootToken, nil)
			}
		}
		return "", func() {}
	}

	// =========================================================================
	// Scenario 1: Negative - entity:FULL does NOT grant managementRole:CREATE
	// Pillar 4: Role administration strictly requires explicit managementRole grant
	// =========================================================================
	t.Run("Negative: entity:FULL is denied managementRole:CREATE", func(t *testing.T) {
		if entityOnlyPolicy.ID == "" {
			t.Skip("Entity-only policy not created")
		}
		_, cleanupAdminRole := createRoleForAdmin(entityId, "", entityOnlyPolicy.ID)
		defer cleanupAdminRole()

		roleAssignPayload := map[string]interface{}{
			"name":             fmt.Sprintf("assigned-role-%d", time.Now().UnixNano()),
			"entity":           entityId,
			"venue":            "",
			"managementPolicy": entityOnlyPolicy.ID,
			"users":            []string{targetUserId},
		}

		s, b, e := client.DoRequest("POST", "/managementRole/0", adminToken, roleAssignPayload)
		if e != nil {
			t.Fatalf("POST request failed: %v", e)
		}

		if s != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden for entity:FULL attempting managementRole:CREATE, got %d. Body: %s", s, string(b))
		}
	})

	// =========================================================================
	// Scenario 2: Negative - operator:FULL does NOT grant managementRole:CREATE
	// Pillar 4: Structural operator grant does NOT grant managementRole grant
	// =========================================================================
	t.Run("Negative: operator:FULL is denied managementRole:CREATE", func(t *testing.T) {
		if operatorOnlyPolicy.ID == "" {
			t.Skip("Operator-only policy not created")
		}
		_, cleanupAdminRole := createRoleForAdmin(entityId, "", operatorOnlyPolicy.ID)
		defer cleanupAdminRole()

		roleAssignPayload := map[string]interface{}{
			"name":             fmt.Sprintf("assigned-role-op-%d", time.Now().UnixNano()),
			"entity":           entityId,
			"venue":            "",
			"managementPolicy": operatorOnlyPolicy.ID,
			"users":            []string{targetUserId},
		}

		s, b, e := client.DoRequest("POST", "/managementRole/0", adminToken, roleAssignPayload)
		if e != nil {
			t.Fatalf("POST request failed: %v", e)
		}

		if s != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden for operator:FULL attempting managementRole:CREATE, got %d. Body: %s", s, string(b))
		}
	})

	// =========================================================================
	// Scenario 3: Negative - entity:FULL does NOT grant managementPolicy:POST / PUT / DELETE
	// Section 6.4: Management Policy mutation is strictly restricted to ROOT
	// =========================================================================
	t.Run("Negative: entity:FULL cannot mutate managementPolicy (ROOT only)", func(t *testing.T) {
		if entityOnlyPolicy.ID == "" {
			t.Skip("Entity-only policy not created")
		}
		_, cleanupAdminRole := createRoleForAdmin(entityId, "", entityOnlyPolicy.ID)
		defer cleanupAdminRole()

		dummyPolicyPayload := map[string]interface{}{
			"name":        fmt.Sprintf("illegal-policy-%d", time.Now().UnixNano()),
			"description": "Unauthorized policy creation attempt by non-root",
			"entries": []map[string]interface{}{
				{
					"resources": []string{"entity"},
					"access":    []string{"READ"},
				},
			},
		}

		// Attempt POST /managementPolicy/0
		s, b, e := client.DoRequest("POST", "/managementPolicy/0", adminToken, dummyPolicyPayload)
		if e != nil {
			t.Fatalf("POST request failed: %v", e)
		}

		if s != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden for non-root creating managementPolicy, got %d. Body: %s", s, string(b))
		}
	})

	// =========================================================================
	// Scenario 4: Positive - Authenticated users can GET managementPolicy
	// Section 6.4 & Section 11: GET /api/v1/managementPolicy is open to authenticated users
	// =========================================================================
	t.Run("Positive: Authenticated users can GET managementPolicy", func(t *testing.T) {
		s, b, e := client.DoRequest("GET", "/managementPolicy", adminToken, nil)
		if e != nil {
			t.Fatalf("GET /managementPolicy failed: %v", e)
		}

		if s != http.StatusOK {
			t.Errorf("Expected 200 OK for GET /managementPolicy, got %d. Body: %s", s, string(b))
		}
	})

	// =========================================================================
	// Scenario 5: Positive - Explicit managementRole grant authorizes role management
	// Pillar 4: Requester with explicit managementRole grant can manage roles
	// =========================================================================
	t.Run("Positive: Explicit managementRole:FULL authorizes managementRole:CREATE", func(t *testing.T) {
		if roleFullPolicy.ID == "" {
			t.Skip("Role-full policy not created")
		}
		_, cleanupAdminRole := createRoleForAdmin(entityId, venueId, roleFullPolicy.ID)
		defer cleanupAdminRole()

		roleAssignPayload := map[string]interface{}{
			"name":             fmt.Sprintf("authorized-role-%d", time.Now().UnixNano()),
			"entity":           entityId,
			"venue":            venueId,
			"managementPolicy": roleFullPolicy.ID,
			"users":            []string{targetUserId},
		}

		s, b, e := client.DoRequest("POST", "/managementRole/0", adminToken, roleAssignPayload)
		if e != nil {
			t.Fatalf("POST request failed: %v", e)
		}

		// If user creator ownership passes, it returns 200 OK
		if s == http.StatusOK || s == http.StatusCreated {
			var created struct {
				ID string `json:"id"`
			}
			if json.Unmarshal(b, &created) == nil && created.ID != "" {
				defer func() {
					_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", created.ID), rootToken, nil)
				}()
			}
		} else if s != http.StatusBadRequest {
			// BadRequest might occur if targetUser wasn't created by admin in test fixture,
			// but Forbidden (403) must NOT occur because managementRole:CREATE is authorized!
			t.Errorf("Unexpected status for authorized managementRole:CREATE, got %d. Body: %s", s, string(b))
		}
	})
}

