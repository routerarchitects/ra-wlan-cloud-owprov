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
 * TestSelfRoleInspectionExemption
 *
 * VALIDATES:
 *   Section 6.4 (Self-Role Inspection Exemption) of requirements/rbac-requirement.md:
 *   Issue 6 (P2): Self-role inspection exemption for functional users in
 *   RESTAPI_managementRole_list_handler.cpp and RESTAPI_Handler.cpp.
 *
 * RULES:
 *   1. Any authenticated user is inherently authorized to list and retrieve their own assigned
 *      Management Roles (where role.users contains Requester.id), regardless of whether their
 *      policy contains an explicit managementRole:READ grant.
 *   2. On collection listing (GET /api/v1/managementRole), if the caller lacks managementRole:READ,
 *      the endpoint automatically filters the result set to return ONLY roles assigned to that caller.
 *   3. Passing ?user=<own_id> returns the caller's assigned roles.
 *   4. Passing ?user=<other_id> by a functional user lacking managementRole:READ returns an empty array.
 *   5. Direct role fetch (GET /api/v1/managementRole/{id} or /api/v2/managementRole/{id}) is granted
 *      under the Self-Inspection Exemption if the target role's users contains Requester.id.
 *   6. Direct role fetch of another user's role without managementRole:READ is denied (403 Forbidden).
 */

func getSelfRoleTestTokensAndFixtures(t *testing.T) (rootToken string, user1Token string, user1Id string, user2Token string, user2Id string, entityId string, venueId string) {
	rootToken = getEnvOrDefault("TOKEN_ROOT", "")
	user1Token = getEnvOrDefault("TOKEN_FUNCTIONAL_USER", getEnvOrDefault("TOKEN_NO_ACCESS", ""))
	user1Id = getEnvOrDefault("FUNCTIONAL_USER_ID", getEnvOrDefault("TARGET_USER_A", ""))
	user2Token = getEnvOrDefault("TOKEN_OTHER_USER", getEnvOrDefault("TOKEN_ADMIN_OPERATOR_B", ""))
	user2Id = getEnvOrDefault("OTHER_USER_ID", getEnvOrDefault("TARGET_USER_B", ""))
	entityId = getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "")
	venueId = getEnvOrDefault("VENUE_A_UUID", "")

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

	if user1Token == "" || user1Id == "" {
		cmd := exec.Command("docker", "exec", "openwifi-postgresql-1", "psql", "-U", "owsec", "-d", "owsec", "-t", "-A", "-c",
			"SELECT 'Bearer ' || t.token, u.id FROM tokens t JOIN users u ON t.username = u.id WHERE u.userrole != 'root' ORDER BY t.created DESC LIMIT 1;")
		out, err := cmd.Output()
		if err == nil {
			lines := strings.Split(strings.TrimSpace(string(out)), "|")
			if len(lines) == 2 {
				if user1Token == "" {
					user1Token = lines[0]
				}
				if user1Id == "" {
					user1Id = lines[1]
				}
			}
		}
		if user1Token == "" {
			user1Token = "Bearer user-functional-1-token"
		}
		if user1Id == "" {
			user1Id = "11111111-1111-1111-1111-111111111111"
		}
	}

	if user2Id == "" {
		cmd := exec.Command("docker", "exec", "openwifi-postgresql-1", "psql", "-U", "owsec", "-d", "owsec", "-t", "-A", "-c",
			"SELECT u.id FROM users u WHERE u.id != '"+user1Id+"' AND u.userrole != 'root' LIMIT 1;")
		out, err := cmd.Output()
		if err == nil && len(strings.TrimSpace(string(out))) > 10 {
			user2Id = strings.TrimSpace(string(out))
		} else {
			user2Id = "22222222-2222-2222-2222-222222222222"
		}
	}
	if user2Token == "" {
		user2Token = "Bearer user-functional-2-token"
	}

	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
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

func TestSelfRoleInspectionExemption(t *testing.T) {
	rootToken, user1Token, user1Id, _, user2Id, entityId, venueId := getSelfRoleTestTokensAndFixtures(t)
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	// 1. Create a functional policy that explicitly lacks managementRole permissions (e.g. CSR/NOC profile)
	functionalPolicyPayload := map[string]interface{}{
		"name":        fmt.Sprintf("functional-no-role-perm-%d", time.Now().UnixNano()),
		"description": "Functional policy without managementRole permissions",
		"entries": []map[string]interface{}{
			{
				"resources": []string{"entity", "venue", "inventory"},
				"access":    []string{"READ"},
			},
		},
	}
	var createdPolicy struct {
		ID string `json:"id"`
	}
	pStatus, pBody, pErr := client.DoRequest("POST", "/managementPolicy/0", rootToken, functionalPolicyPayload)
	if pErr != nil || (pStatus != http.StatusOK && pStatus != http.StatusCreated) {
		t.Logf("Notice: Could not create test functional policy (status %d, err %v). Proceeding with existing state.", pStatus, pErr)
	} else {
		_ = json.Unmarshal(pBody, &createdPolicy)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", createdPolicy.ID), rootToken, nil)
		}()
	}

	policyIdToUse := createdPolicy.ID
	if policyIdToUse == "" {
		policyIdToUse = getEnvOrDefault("POLICY_WEAK_ID", "94bb61a3-aa3e-49aa-9704-cc254fec4482")
	}

	// 2. Assign a management role to User 1
	role1Payload := map[string]interface{}{
		"name":             fmt.Sprintf("user1-functional-role-%d", time.Now().UnixNano()),
		"description":      "Functional role for user 1",
		"entity":           entityId,
		"venue":            venueId,
		"managementPolicy": policyIdToUse,
		"users":            []string{user1Id},
	}
	var createdRole1 struct {
		ID string `json:"id"`
	}
	r1Status, r1Body, r1Err := client.DoRequest("POST", "/managementRole/0", rootToken, role1Payload)
	if r1Err == nil && (r1Status == http.StatusOK || r1Status == http.StatusCreated) {
		_ = json.Unmarshal(r1Body, &createdRole1)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", createdRole1.ID), rootToken, nil)
		}()
	} else {
		t.Logf("Notice: Could not create test role 1 (status %d, err %v)", r1Status, r1Err)
	}

	// 3. Assign a management role to User 2
	role2Payload := map[string]interface{}{
		"name":             fmt.Sprintf("user2-functional-role-%d", time.Now().UnixNano()),
		"description":      "Functional role for user 2",
		"entity":           entityId,
		"venue":            venueId,
		"managementPolicy": policyIdToUse,
		"users":            []string{user2Id},
	}
	var createdRole2 struct {
		ID string `json:"id"`
	}
	r2Status, r2Body, r2Err := client.DoRequest("POST", "/managementRole/0", rootToken, role2Payload)
	if r2Err == nil && (r2Status == http.StatusOK || r2Status == http.StatusCreated) {
		_ = json.Unmarshal(r2Body, &createdRole2)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", createdRole2.ID), rootToken, nil)
		}()
	} else {
		t.Logf("Notice: Could not create test role 2 (status %d, err %v)", r2Status, r2Err)
	}

	// =========================================================================
	// Scenario 1: Positive - Functional user without managementRole:READ can list own roles
	// Section 6.4: Collection query returns ONLY caller's assigned roles
	// =========================================================================
	t.Run("Positive: Functional user can list own roles without managementRole:READ", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", "/managementRole", user1Token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK for self-role listing, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []struct {
				ID    string   `json:"id"`
				Users []string `json:"users"`
			} `json:"roles"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse response: %v", err)
		}

		// Ensure all returned roles belong to user 1 and NEVER include user 2's role
		for _, r := range resp.Roles {
			foundUser1 := false
			for _, u := range r.Users {
				if u == user1Id {
					foundUser1 = true
				}
				if u == user2Id && user2Id != user1Id {
					t.Errorf("Security Violation: User 2's role %s was leaked to User 1!", r.ID)
				}
			}
			if !foundUser1 {
				t.Errorf("Role %s returned to User 1 does not belong to User 1!", r.ID)
			}
		}
	})

	// =========================================================================
	// Scenario 2: Positive - Functional user passing ?user=<own_id> returns own roles
	// =========================================================================
	t.Run("Positive: Functional user query with ?user=<own_id> returns own roles", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?user=%s", user1Id), user1Token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?user=own failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []struct {
				ID string `json:"id"`
			} `json:"roles"`
		}
		_ = json.Unmarshal(body, &resp)
		if createdRole1.ID != "" {
			found := false
			for _, r := range resp.Roles {
				if r.ID == createdRole1.ID {
					found = true
					break
				}
			}
			if !found {
				t.Errorf("Expected role %s returned for ?user=<own_id>", createdRole1.ID)
			}
		}
	})

	// =========================================================================
	// Scenario 3: Positive - Functional user can fetch own role directly by ID
	// Section 6.4: Direct role fetch granted under Self-Inspection Exemption
	// =========================================================================
	t.Run("Positive: Functional user can fetch own role directly by ID", func(t *testing.T) {
		if createdRole1.ID == "" {
			t.Skip("createdRole1 was not created; skipping direct ID fetch")
		}
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", createdRole1.ID), user1Token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole/{id} failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for fetching own role by ID, got %d. Body: %s", status, string(body))
		}

		var role struct {
			ID string `json:"id"`
		}
		_ = json.Unmarshal(body, &role)
		if role.ID != createdRole1.ID {
			t.Errorf("Expected role ID %s, got %s", createdRole1.ID, role.ID)
		}
	})

	// =========================================================================
	// Scenario 4: Positive - Functional user can fetch own role via ?select=<id>
	// =========================================================================
	t.Run("Positive: Functional user can fetch own role via ?select=<own_role_id>", func(t *testing.T) {
		if createdRole1.ID == "" {
			t.Skip("createdRole1 was not created; skipping ?select test")
		}
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?select=%s", createdRole1.ID), user1Token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?select=own failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for ?select=<own_role_id>, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []struct {
				ID string `json:"id"`
			} `json:"roles"`
		}
		_ = json.Unmarshal(body, &resp)
		if len(resp.Roles) != 1 || resp.Roles[0].ID != createdRole1.ID {
			t.Errorf("Expected exactly 1 role matching %s, got %+v", createdRole1.ID, resp.Roles)
		}
	})

	// =========================================================================
	// Scenario 5: Positive - Functional user countOnly query returns accurate count
	// =========================================================================
	t.Run("Positive: Functional user countOnly query returns count of own roles", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", "/managementRole?countOnly=true", user1Token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?countOnly=true failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for countOnly, got %d. Body: %s", status, string(body))
		}

		var countResp struct {
			Count int `json:"count"`
		}
		_ = json.Unmarshal(body, &countResp)
		if createdRole1.ID != "" && countResp.Count <= 0 {
			t.Errorf("Expected count > 0 for own roles, got %d", countResp.Count)
		}
	})

	// =========================================================================
	// Scenario 6: Negative - Functional user querying ?user=<other_user_id> returns empty
	// Section 6.4: Functional users cannot inspect other users' roles
	// =========================================================================
	t.Run("Negative: Functional user querying ?user=<other_user_id> returns empty list", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?user=%s", user2Id), user1Token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?user=other failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK with empty array for out-of-scope user inspection, got %d", status)
		}

		var resp struct {
			Roles []interface{} `json:"roles"`
		}
		_ = json.Unmarshal(body, &resp)
		if len(resp.Roles) != 0 {
			t.Errorf("Expected 0 roles returned when querying other user, got %d", len(resp.Roles))
		}
	})

	// =========================================================================
	// Scenario 7: Negative - Functional user fetching other user's role by ID is denied
	// Section 6.4: Fetching another user's role without managementRole:READ is 403 Forbidden
	// =========================================================================
	t.Run("Negative: Functional user fetching other user's role by ID is denied (403)", func(t *testing.T) {
		if createdRole2.ID == "" {
			t.Skip("createdRole2 was not created; skipping other role fetch test")
		}
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", createdRole2.ID), user1Token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole/{other_role_id} failed: %v", err)
		}
		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden for fetching another user's role, got %d. Body: %s", status, string(body))
		}
	})

	// =========================================================================
	// Scenario 8: Edge Case - Querying with non-matching entity filter returns empty
	// =========================================================================
	t.Run("Edge Case: Functional user filtering by non-matching entity returns empty list", func(t *testing.T) {
		fakeEntity := "ffffffff-ffff-ffff-ffff-ffffffffffff"
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?entity=%s", fakeEntity), user1Token, nil)
		if err != nil {
			t.Fatalf("GET with non-matching entity filter failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK with empty array, got %d", status)
		}

		var resp struct {
			Roles []interface{} `json:"roles"`
		}
		_ = json.Unmarshal(body, &resp)
		if len(resp.Roles) != 0 {
			t.Errorf("Expected 0 roles for non-matching entity filter, got %d", len(resp.Roles))
		}
	})
}

/*
 * TestSelfRoleInspection_MixedPermissions_Regression
 *
 * REGRESSION TEST FOR REVIEW COMMENT:
 *   "P2 — Self-role inspection fails when the requester has mixed ManagementRole permissions
 *    Required change: For all non-root users, return the union of:
 *      1. Management roles directly assigned to the requesting user.
 *      2. Management roles visible through explicit managementRole:READ permission within applicable scope.
 *    Deduplicate the results by role ID and apply all query filters, pagination, and countOnly consistently.
 *    Required regression test: A user with an Installer role in Venue A and an Admin role in Venue B
 *    must see both assigned roles, while seeing other users' roles only where explicitly authorized."
 */
func TestSelfRoleInspection_MixedPermissions_Regression(t *testing.T) {
	rootToken, user1Token, user1Id, _, user2Id, entityId, _ := getSelfRoleTestTokensAndFixtures(t)
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	// 1. Create two test venues under entity
	createVenue := func(name string) (string, func()) {
		payload := map[string]interface{}{
			"name":        fmt.Sprintf("%s-%d", name, time.Now().UnixNano()),
			"description": "Venue for mixed permissions regression test",
			"entity":      entityId,
		}
		var created struct {
			ID string `json:"id"`
		}
		status, body, err := client.DoRequest("POST", "/venue/0", rootToken, payload)
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			t.Logf("Notice: Could not create venue %s: status %d, err %v", name, status, err)
			return "", func() {}
		}
		_ = json.Unmarshal(body, &created)
		cleanup := func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", created.ID), rootToken, nil)
		}
		return created.ID, cleanup
	}

	venueA, cleanupVenueA := createVenue("venue-a-installer")
	defer cleanupVenueA()
	venueB, cleanupVenueB := createVenue("venue-b-admin")
	defer cleanupVenueB()

	// 2. Create Policy 1: Installer policy (inventory:READ, NO managementRole permissions)
	installerPolicyPayload := map[string]interface{}{
		"name":        fmt.Sprintf("installer-policy-%d", time.Now().UnixNano()),
		"description": "Installer policy without managementRole permissions",
		"entries": []map[string]interface{}{
			{
				"resources": []string{"inventory", "device"},
				"access":    []string{"READ"},
			},
		},
	}
	var installerPolicy struct {
		ID string `json:"id"`
	}
	p1Status, p1Body, p1Err := client.DoRequest("POST", "/managementPolicy/0", rootToken, installerPolicyPayload)
	if p1Err == nil && (p1Status == http.StatusOK || p1Status == http.StatusCreated) {
		_ = json.Unmarshal(p1Body, &installerPolicy)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", installerPolicy.ID), rootToken, nil)
		}()
	}

	// 3. Create Policy 2: Admin policy (has explicit managementRole:READ)
	adminPolicyPayload := map[string]interface{}{
		"name":        fmt.Sprintf("venue-admin-policy-%d", time.Now().UnixNano()),
		"description": "Admin policy with managementRole READ permissions",
		"entries": []map[string]interface{}{
			{
				"resources": []string{"managementRole", "venue"},
				"access":    []string{"READ"},
			},
		},
	}
	var adminPolicy struct {
		ID string `json:"id"`
	}
	p2Status, p2Body, p2Err := client.DoRequest("POST", "/managementPolicy/0", rootToken, adminPolicyPayload)
	if p2Err == nil && (p2Status == http.StatusOK || p2Status == http.StatusCreated) {
		_ = json.Unmarshal(p2Body, &adminPolicy)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", adminPolicy.ID), rootToken, nil)
		}()
	}

	if venueA == "" || venueB == "" || installerPolicy.ID == "" || adminPolicy.ID == "" {
		t.Skip("Prerequisites for live mixed-permissions regression test could not be provisioned; skipping")
	}

	// 4. Create Role 1: User 1 in Venue A with Installer Policy (no managementRole:READ)
	role1Payload := map[string]interface{}{
		"name":             fmt.Sprintf("user1-venueA-installer-%d", time.Now().UnixNano()),
		"description":      "Installer role in Venue A",
		"entity":           entityId,
		"venue":            venueA,
		"managementPolicy": installerPolicy.ID,
		"users":            []string{user1Id},
	}
	var role1 struct {
		ID string `json:"id"`
	}
	r1Status, r1Body, r1Err := client.DoRequest("POST", "/managementRole/0", rootToken, role1Payload)
	if r1Err == nil && (r1Status == http.StatusOK || r1Status == http.StatusCreated) {
		_ = json.Unmarshal(r1Body, &role1)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", role1.ID), rootToken, nil)
		}()
	}

	// 5. Create Role 2: User 1 in Venue B with Admin Policy (has managementRole:READ)
	role2Payload := map[string]interface{}{
		"name":             fmt.Sprintf("user1-venueB-admin-%d", time.Now().UnixNano()),
		"description":      "Admin role in Venue B",
		"entity":           entityId,
		"venue":            venueB,
		"managementPolicy": adminPolicy.ID,
		"users":            []string{user1Id},
	}
	var role2 struct {
		ID string `json:"id"`
	}
	r2Status, r2Body, r2Err := client.DoRequest("POST", "/managementRole/0", rootToken, role2Payload)
	if r2Err == nil && (r2Status == http.StatusOK || r2Status == http.StatusCreated) {
		_ = json.Unmarshal(r2Body, &role2)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", role2.ID), rootToken, nil)
		}()
	}

	// 6. Create Role 3: User 2 (third party) in Venue A (User 1 has NO managementRole:READ in Venue A)
	role3Payload := map[string]interface{}{
		"name":             fmt.Sprintf("user2-venueA-installer-%d", time.Now().UnixNano()),
		"description":      "User 2 role in Venue A",
		"entity":           entityId,
		"venue":            venueA,
		"managementPolicy": installerPolicy.ID,
		"users":            []string{user2Id},
	}
	var role3 struct {
		ID string `json:"id"`
	}
	r3Status, r3Body, r3Err := client.DoRequest("POST", "/managementRole/0", rootToken, role3Payload)
	if r3Err == nil && (r3Status == http.StatusOK || r3Status == http.StatusCreated) {
		_ = json.Unmarshal(r3Body, &role3)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", role3.ID), rootToken, nil)
		}()
	}

	// 7. Create Role 4: User 2 (third party) in Venue B (User 1 HAS managementRole:READ in Venue B)
	role4Payload := map[string]interface{}{
		"name":             fmt.Sprintf("user2-venueB-installer-%d", time.Now().UnixNano()),
		"description":      "User 2 role in Venue B",
		"entity":           entityId,
		"venue":            venueB,
		"managementPolicy": installerPolicy.ID,
		"users":            []string{user2Id},
	}
	var role4 struct {
		ID string `json:"id"`
	}
	r4Status, r4Body, r4Err := client.DoRequest("POST", "/managementRole/0", rootToken, role4Payload)
	if r4Err == nil && (r4Status == http.StatusOK || r4Status == http.StatusCreated) {
		_ = json.Unmarshal(r4Body, &role4)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", role4.ID), rootToken, nil)
		}()
	}

	// =========================================================================
	// Scenario 1: User 1 lists all roles: must see both own roles (Role 1 & 2)
	// and Role 4 (authorized in Venue B), but NEVER Role 3 (unauthorized in Venue A)
	// =========================================================================
	t.Run("Regression: Mixed user sees both assigned roles and only authorized scope roles", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", "/managementRole", user1Token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []struct {
				ID    string   `json:"id"`
				Venue string   `json:"venue"`
				Users []string `json:"users"`
			} `json:"roles"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse JSON response: %v", err)
		}

		foundRole1 := false
		foundRole2 := false
		foundRole4 := false
		foundRole3 := false

		for _, r := range resp.Roles {
			if r.ID == role1.ID {
				foundRole1 = true
			}
			if r.ID == role2.ID {
				foundRole2 = true
			}
			if r.ID == role4.ID {
				foundRole4 = true
			}
			if r.ID == role3.ID {
				foundRole3 = true
			}
		}

		if !foundRole1 {
			t.Errorf("Violation: User 1 cannot see their own Installer role in Venue A (%s)", role1.ID)
		}
		if !foundRole2 {
			t.Errorf("Violation: User 1 cannot see their own Admin role in Venue B (%s)", role2.ID)
		}
		if !foundRole4 {
			t.Errorf("Violation: User 1 cannot see authorized other user's role in Venue B (%s)", role4.ID)
		}
		if foundRole3 {
			t.Errorf("Security Violation: User 1 was leaked unauthorized other user's role in Venue A (%s)!", role3.ID)
		}
	})

	// =========================================================================
	// Scenario 2: Filtering by Venue A (?venue=VenueA) returns own Installer role
	// but excludes other users' roles in Venue A
	// =========================================================================
	t.Run("Regression: Filtering by Venue A returns own role but excludes other users", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?venue=%s", venueA), user1Token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?venue=VenueA failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []struct {
				ID string `json:"id"`
			} `json:"roles"`
		}
		_ = json.Unmarshal(body, &resp)

		foundRole1 := false
		foundRole3 := false
		for _, r := range resp.Roles {
			if r.ID == role1.ID {
				foundRole1 = true
			}
			if r.ID == role3.ID {
				foundRole3 = true
			}
		}

		if !foundRole1 {
			t.Errorf("Expected to see own Role 1 in Venue A, but was omitted")
		}
		if foundRole3 {
			t.Errorf("Security Violation: User 2's role in Venue A leaked under ?venue filter!")
		}
	})

	// =========================================================================
	// Scenario 3: Querying other user (?user=User2) returns role in Venue B but not Venue A
	// =========================================================================
	t.Run("Regression: Querying other user returns roles in Venue B but not Venue A", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?user=%s", user2Id), user1Token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?user=user2 failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []struct {
				ID string `json:"id"`
			} `json:"roles"`
		}
		_ = json.Unmarshal(body, &resp)

		foundRole4 := false
		foundRole3 := false
		for _, r := range resp.Roles {
			if r.ID == role4.ID {
				foundRole4 = true
			}
			if r.ID == role3.ID {
				foundRole3 = true
			}
		}

		if !foundRole4 {
			t.Errorf("Expected to see User 2's role in authorized Venue B (%s)", role4.ID)
		}
		if foundRole3 {
			t.Errorf("Security Violation: User 2's role in unauthorized Venue A (%s) was returned!", role3.ID)
		}
	})

	// =========================================================================
	// Scenario 4: Direct fetch by ID: User 1 can fetch other user's role in Venue B (200)
	// but direct fetch of other user's role in Venue A is denied (403)
	// =========================================================================
	t.Run("Regression: Direct ID fetch of other user role in Venue B (200) vs Venue A (403)", func(t *testing.T) {
		// Venue B: User 1 has managementRole:READ -> 200 OK
		statusB, bodyB, errB := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", role4.ID), user1Token, nil)
		if errB != nil {
			t.Fatalf("GET /managementRole/{role4} failed: %v", errB)
		}
		if statusB != http.StatusOK {
			t.Errorf("Expected 200 OK for other user's role in authorized Venue B, got %d. Body: %s", statusB, string(bodyB))
		}

		// Venue A: User 1 does NOT have managementRole:READ -> 403 Forbidden
		statusA, bodyA, errA := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", role3.ID), user1Token, nil)
		if errA != nil {
			t.Fatalf("GET /managementRole/{role3} failed: %v", errA)
		}
		if statusA != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden for other user's role in unauthorized Venue A, got %d. Body: %s", statusA, string(bodyA))
		}
	})
}


