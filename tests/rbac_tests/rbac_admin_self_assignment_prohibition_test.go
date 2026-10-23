package rbac_tests

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os/exec"
	"strings"
	"testing"
	"time"
)

/*
 * TestAdminSelfAssignmentProhibition
 *
 * VALIDATES:
 *   Section 7.1, 7.2 of requirements/rbac-requirement.md
 *   P1: Admin Self-Assignment, Self-Modification, and Self-Deletion Prohibition.
 *
 * RULES:
 *   - An Admin is strictly forbidden from assigning, modifying, or deleting their own Management Roles.
 *   - TargetUser.id == Requester.id is prohibited.
 *   - owprov rejects self-targeting with 400 Bad Request:
 *     "You are not authorized to assign or modify roles for users you did not create."
 *   - ROOT is exempted and can manage roles for any user.
 */

// Helper to auto-discover live tokens and fixtures from PostgreSQL if environment variables are not set
func getLiveTestTokensAndFixtures(t *testing.T) (rootToken string, adminToken string, adminUserId string, entityId string, venueId string, policyId string) {
	rootToken = getEnvOrDefault("TOKEN_ROOT", "")
	adminToken = getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "")
	adminUserId = getEnvOrDefault("ADMIN_USER_ID", "")
	entityId = getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "")
	venueId = getEnvOrDefault("VENUE_A_UUID", "")
	policyId = getEnvOrDefault("POLICY_WEAK_ID", "")

	// Fallback to querying local postgres container if tokens or user IDs are not set
	if rootToken == "" {
		cmd := exec.Command("docker", "exec", "openwifi-postgresql-1", "psql", "-U", "owsec", "-d", "owsec", "-t", "-A", "-c",
			"SELECT 'Bearer ' || token FROM tokens WHERE username = '11111111-0000-0000-6666-999999999999' ORDER BY created DESC LIMIT 1;")
		out, err := cmd.Output()
		if err == nil && len(strings.TrimSpace(string(out))) > 10 {
			rootToken = strings.TrimSpace(string(out))
		} else {
			rootToken = "Bearer root-test-token"
		}
	}

	if adminToken == "" || adminUserId == "" {
		cmd := exec.Command("docker", "exec", "openwifi-postgresql-1", "psql", "-U", "owsec", "-d", "owsec", "-t", "-A", "-c",
			"SELECT 'Bearer ' || t.token, u.id FROM tokens t JOIN users u ON t.username = u.id WHERE u.userrole = 'admin' ORDER BY t.created DESC LIMIT 1;")
		out, err := cmd.Output()
		if err == nil {
			lines := strings.Split(strings.TrimSpace(string(out)), "|")
			if len(lines) == 2 {
				if adminToken == "" {
					adminToken = lines[0]
				}
				if adminUserId == "" {
					adminUserId = lines[1]
				}
			}
		}
		if adminToken == "" {
			adminToken = "Bearer user-admin-operator-a-token"
		}
		if adminUserId == "" {
			adminUserId = "741f7ce6-7053-40e5-8772-f2197db06853"
		}
	}

	// Auto-discover entity, venue, and policy from owprov if not set
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	if policyId == "" {
		var polResp struct {
			ManagementPolicies []struct {
				ID string `json:"id"`
			} `json:"managementPolicies"`
		}
		if status, body, err := client.DoRequest("GET", "/managementPolicy", rootToken, nil); err == nil && status == http.StatusOK {
			_ = json.Unmarshal(body, &polResp)
			if len(polResp.ManagementPolicies) > 0 {
				policyId = polResp.ManagementPolicies[0].ID
			}
		}
		if policyId == "" {
			policyId = "69a6471a-b50f-4959-bc8f-8a0b8b7459ce"
		}
	}

	if entityId == "" || venueId == "" {
		var entResp struct {
			Entities []struct {
				ID     string   `json:"id"`
				Venues []string `json:"venues"`
			} `json:"entities"`
		}
		if status, body, err := client.DoRequest("GET", "/entity", rootToken, nil); err == nil && status == http.StatusOK {
			_ = json.Unmarshal(body, &entResp)
			for _, e := range entResp.Entities {
				if len(e.Venues) > 0 {
					entityId = e.ID
					venueId = e.Venues[0]
					break
				}
			}
		}
		if entityId == "" {
			entityId = "c59573d8-54be-42d4-b087-b372317203af"
		}
		if venueId == "" {
			venueId = "bb52f7ad-b7a6-4427-a493-65ff206e7bd1"
		}
	}

	return
}

func TestAdmin_SelfAssignment_Prohibition_V1(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	_, adminToken, adminUserId, entityId, venueId, policyId := getLiveTestTokensAndFixtures(t)

	t.Run("Negative: Admin attempting V1 POST to assign role to self is rejected", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             fmt.Sprintf("self-assign-v1-%d", time.Now().UnixNano()),
			"description":      "Admin self-assignment attempt",
			"entity":           entityId,
			"venue":            venueId,
			"managementPolicy": policyId,
			"users":            []string{adminUserId},
		}

		status, body, err := client.DoRequest("POST", "/managementRole/0", adminToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}

		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for Admin self-assignment, got %d. Body: %s", status, string(body))
		}

		expectedError := "You are not authorized to assign or modify roles for users you did not create."
		if !strings.Contains(string(body), expectedError) {
			t.Errorf("Expected error message to contain '%s', got: %s", expectedError, string(body))
		}
	})
}

func TestAdmin_SelfAssignment_Prohibition_V2(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	_, adminToken, adminUserId, entityId, venueId, policyId := getLiveTestTokensAndFixtures(t)

	t.Run("Negative: Admin attempting V2 POST to assign role to self is rejected", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             fmt.Sprintf("self-assign-v2-%d", time.Now().UnixNano()),
			"description":      "Admin V2 self-assignment attempt",
			"entity":           entityId,
			"venueIds":         []string{venueId},
			"managementPolicy": policyId,
			"users":            []string{adminUserId},
		}

		status, body, err := client.DoRequest("POST", "/managementRole/0", adminToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}

		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for Admin V2 self-assignment, got %d. Body: %s", status, string(body))
		}

		expectedError := "You are not authorized to assign or modify roles for users you did not create."
		if !strings.Contains(string(body), expectedError) {
			t.Errorf("Expected error message to contain '%s', got: %s", expectedError, string(body))
		}
	})
}

func TestAdmin_SelfModification_And_SelfDeletion_Prohibition(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken, adminToken, adminUserId, entityId, venueId, policyId := getLiveTestTokensAndFixtures(t)

	// Step 1: ROOT provisions a Management Role for the Admin (allowed under governance model)
	roleName := fmt.Sprintf("admin-gov-role-%d", time.Now().UnixNano())
	createPayload := map[string]interface{}{
		"name":             roleName,
		"description":      "Governed Admin role created by ROOT",
		"entity":           entityId,
		"venue":            venueId,
		"managementPolicy": policyId,
		"users":            []string{adminUserId},
	}

	createStatus, createBody, err := client.DoRequest("POST", "/managementRole/0", rootToken, createPayload)
	if err != nil {
		t.Fatalf("ROOT setup role creation failed: %v", err)
	}
	if createStatus != http.StatusOK && createStatus != http.StatusCreated {
		t.Fatalf("ROOT could not provision setup role for admin, got status %d. Body: %s", createStatus, string(createBody))
	}

	var createdRole struct {
		ID string `json:"id"`
	}
	if err := json.Unmarshal(createBody, &createdRole); err != nil || createdRole.ID == "" {
		t.Fatalf("Failed to extract ID from created setup role: %v", err)
	}

	roleId := createdRole.ID
	defer func() {
		// Cleanup using ROOT token
		_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", roleId), rootToken, nil)
	}()

	// Step 2: Negative test - Admin attempts PUT (modify) on their OWN Management Role
	t.Run("Negative: Admin attempting PUT to modify own role is rejected", func(t *testing.T) {
		putPayload := map[string]interface{}{
			"name":             roleName + "-mod",
			"description":      "Admin self-modification attempt",
			"managementPolicy": policyId,
		}

		status, body, err := client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleId), adminToken, putPayload)
		if err != nil {
			t.Fatalf("PUT request failed: %v", err)
		}

		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for Admin self-modification, got %d. Body: %s", status, string(body))
		}

		expectedError := "You are not authorized to assign or modify roles for users you did not create."
		if !strings.Contains(string(body), expectedError) {
			t.Errorf("Expected error message to contain '%s', got: %s", expectedError, string(body))
		}
	})

	// Step 3: Negative test - Admin attempts DELETE on their OWN Management Role
	t.Run("Negative: Admin attempting DELETE on own role is rejected", func(t *testing.T) {
		status, body, err := client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", roleId), adminToken, nil)
		if err != nil {
			t.Fatalf("DELETE request failed: %v", err)
		}

		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for Admin self-deletion, got %d. Body: %s", status, string(body))
		}

		expectedError := "You are not authorized to assign or modify roles for users you did not create."
		if !strings.Contains(string(body), expectedError) {
			t.Errorf("Expected error message to contain '%s', got: %s", expectedError, string(body))
		}
	})

	// Step 4: Positive control - Admin self-inspection exemption (GET) remains functional
	t.Run("Positive: Admin self-inspection exemption (GET) is allowed", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", roleId), adminToken, nil)
		if err != nil {
			t.Fatalf("GET request failed: %v", err)
		}

		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for Admin self-inspection exemption GET, got %d. Body: %s", status, string(body))
		}
	})

	// Step 5: Positive control - ROOT can modify and delete the role
	t.Run("Positive: ROOT can modify and delete the Admin's role", func(t *testing.T) {
		// Update by ROOT
		putPayload := map[string]interface{}{
			"name":             roleName + "-root-mod",
			"description":      "ROOT modification of admin role",
			"managementPolicy": policyId,
		}
		status, body, err := client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleId), rootToken, putPayload)
		if err != nil {
			t.Fatalf("ROOT PUT request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for ROOT modifying admin role, got %d. Body: %s", status, string(body))
		}

		// Delete by ROOT
		delStatus, delBody, err := client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", roleId), rootToken, nil)
		if err != nil {
			t.Fatalf("ROOT DELETE request failed: %v", err)
		}
		if delStatus != http.StatusOK {
			t.Errorf("Expected 200 OK for ROOT deleting admin role, got %d. Body: %s", delStatus, string(delBody))
		}
	})
}
