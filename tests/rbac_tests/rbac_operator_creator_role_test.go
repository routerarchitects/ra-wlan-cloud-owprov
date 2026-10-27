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
 * TestOperatorCreatorRole
 *
 * VALIDATES:
 *   Section 8.3 & Section 9.4 of requirements/rbac-requirement.md:
 *   Issue 2 (P2): Auto-create creator role on companion Operator Entity in RESTAPI_operators_handler.cpp:DoPost()
 *
 * RULES:
 *   1. When an Operator is created (POST /api/v1/operator/0), an Operator Entity is automatically created.
 *   2. For non-ROOT creators, a Management Role must be automatically created on that companion Operator Entity
 *      inheriting the authorizing policy held on the parent (Root Entity).
 *   3. This auto-created role guarantees visibility continuity (allowing the creator to GET /operator/{id}).
 *   4. ROOT is exempted (no redundant scoped role needed).
 *   5. When the Operator is deleted, all management roles on the companion entity are cascadingly deleted.
 *   6. Cross-tenant isolation prevents unassigned users from accessing the Operator.
 */

type operatorResponse struct {
	ID             string `json:"id"`
	Name           string `json:"name"`
	Description    string `json:"description"`
	EntityID       string `json:"entityId"`
	RegistrationID string `json:"registrationId"`
}

type managementRoleListResponse struct {
	Roles []struct {
		ID               string   `json:"id"`
		Name             string   `json:"name"`
		Entity           string   `json:"entity"`
		Venue            string   `json:"venue"`
		ManagementPolicy string   `json:"managementPolicy"`
		Users            []string `json:"users"`
	} `json:"roles"`
}

// Helper to set up or find a non-root admin with operator:FULL on Root Entity (0000-0000-0000)
func getAdminWithRootOperatorPolicy(t *testing.T, client *TestClient, rootToken string) (adminToken string, adminUserId string, fullPolicyId string, teardown func()) {
	// 1. Discover full operator policy in DB
	cmd := exec.Command("docker", "exec", "openwifi-postgresql-1", "psql", "-U", "owprov", "-d", "owprov", "-t", "-A", "-c",
		"SELECT id FROM policies WHERE entries LIKE '%operator%' AND entries LIKE '%FULL%' LIMIT 1;")
	out, err := cmd.Output()
	if err == nil && len(strings.TrimSpace(string(out))) > 10 {
		fullPolicyId = strings.TrimSpace(string(out))
	} else {
		fullPolicyId = "c5d4ea77-26e2-4c4f-b403-5fa906c7635d"
	}

	// 2. Discover an admin user and token
	cmd = exec.Command("docker", "exec", "openwifi-postgresql-1", "psql", "-U", "owsec", "-d", "owsec", "-t", "-A", "-c",
		"SELECT 'Bearer ' || t.token, u.id FROM tokens t JOIN users u ON t.username = u.id WHERE u.userrole = 'admin' ORDER BY t.created DESC LIMIT 1;")
	out, err = cmd.Output()
	if err == nil {
		parts := strings.Split(strings.TrimSpace(string(out)), "|")
		if len(parts) == 2 {
			adminToken = parts[0]
			adminUserId = parts[1]
		}
	}
	if adminToken == "" {
		adminToken = "Bearer user-admin-operator-a-token"
		adminUserId = "741f7ce6-7053-40e5-8772-f2197db06853"
	}

	// 3. Ensure the admin has a role on the Root Entity (0000-0000-0000) with fullPolicyId so they have operator:CREATE authority
	rootRolePayload := map[string]interface{}{
		"name":             fmt.Sprintf("root-operator-auth-role-%d", time.Now().UnixNano()),
		"description":      "Authorizing role on Root Entity for operator creation",
		"entity":           "0000-0000-0000",
		"venue":            "",
		"managementPolicy": fullPolicyId,
		"users":            []string{adminUserId},
	}

	var createdRootRoleId string
	status, body, err := client.DoRequest("POST", "/managementRole/0", rootToken, rootRolePayload)
	if err == nil && (status == http.StatusOK || status == http.StatusCreated) {
		var resp struct {
			ID string `json:"id"`
		}
		if json.Unmarshal(body, &resp) == nil && resp.ID != "" {
			createdRootRoleId = resp.ID
		}
	}

	teardown = func() {
		if createdRootRoleId != "" {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", createdRootRoleId), rootToken, nil)
		}
	}

	return
}

func TestOperator_CreatorRole_AutoCreated_Positive(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken, _, _, _, _, _ := getLiveTestTokensAndFixtures(t)
	adminToken, adminUserId, fullPolicyId, teardown := getAdminWithRootOperatorPolicy(t, client, rootToken)
	defer teardown()

	uniqueSuffix := fmt.Sprintf("%d", time.Now().UnixNano()%1000000)
	regId := fmt.Sprintf("reg-%s", uniqueSuffix)
	opName := fmt.Sprintf("Op-%s", uniqueSuffix)

	createPayload := map[string]interface{}{
		"name":           opName,
		"description":    "Test operator for auto-created creator role verification",
		"registrationId": regId,
	}

	// 1. Non-root Admin creates an Operator
	status, body, err := client.DoRequest("POST", "/operator/0", adminToken, createPayload)
	if err != nil {
		t.Fatalf("POST /operator/0 failed: %v", err)
	}
	if status != http.StatusOK && status != http.StatusCreated {
		t.Fatalf("Expected 200/201 on Operator creation, got %d. Body: %s", status, string(body))
	}

	var createdOp operatorResponse
	if err := json.Unmarshal(body, &createdOp); err != nil {
		t.Fatalf("Failed to decode Operator response: %v", err)
	}
	if createdOp.ID == "" || createdOp.EntityID == "" {
		t.Fatalf("Operator created without ID or EntityID: %+v", createdOp)
	}

	defer func() {
		// Cleanup Operator
		_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/operator/%s", createdOp.ID), rootToken, nil)
	}()

	// 2. Verify companion Operator Entity was created
	entStatus, entBody, err := client.DoRequest("GET", fmt.Sprintf("/entity/%s", createdOp.EntityID), rootToken, nil)
	if err != nil || entStatus != http.StatusOK {
		t.Fatalf("Failed to fetch companion entity %s: status %d, body %s", createdOp.EntityID, entStatus, string(entBody))
	}

	// 3. Verify that a Management Role was AUTO-CREATED for the creator on the companion Operator Entity
	roleStatus, roleBody, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?entity=%s", createdOp.EntityID), rootToken, nil)
	if err != nil || roleStatus != http.StatusOK {
		t.Fatalf("Failed to query management roles on entity %s: status %d, body %s", createdOp.EntityID, roleStatus, string(roleBody))
	}

	var roleList managementRoleListResponse
	if err := json.Unmarshal(roleBody, &roleList); err != nil {
		t.Fatalf("Failed to parse managementRole response: %v", err)
	}

	var autoCreatedRoleId string
	foundRole := false
	for _, r := range roleList.Roles {
		if r.Entity == createdOp.EntityID && r.Venue == "" {
			for _, u := range r.Users {
				if u == adminUserId {
					foundRole = true
					autoCreatedRoleId = r.ID
					if r.ManagementPolicy != fullPolicyId {
						t.Errorf("Expected auto-created role policy %s, got %s", fullPolicyId, r.ManagementPolicy)
					}
					break
				}
			}
		}
	}

	if !foundRole {
		t.Fatalf("Expected auto-created role for user %s on entity %s, but none was found in: %s",
			adminUserId, createdOp.EntityID, string(roleBody))
	}

	// 4. Verify Tenancy Visibility Continuity:
	// Because the role was created, the creator can GET /operator/{id}
	getOpStatus, getOpBody, err := client.DoRequest("GET", fmt.Sprintf("/operator/%s", createdOp.ID), adminToken, nil)
	if err != nil {
		t.Fatalf("GET /operator/%s failed: %v", createdOp.ID, err)
	}
	if getOpStatus != http.StatusOK {
		t.Errorf("Expected 200 OK for creator GET own operator via auto-created role, got %d. Body: %s", getOpStatus, string(getOpBody))
	}

	// 5. Verify Cascade Deletion of Role when Operator is deleted
	delStatus, delBody, err := client.DoRequest("DELETE", fmt.Sprintf("/operator/%s", createdOp.ID), rootToken, nil)
	if err != nil || delStatus != http.StatusOK {
		t.Fatalf("DELETE /operator/%s failed: status %d, body %s", createdOp.ID, delStatus, string(delBody))
	}

	// Query the auto-created role directly: it must be deleted!
	verifyRoleStatus, _, _ := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", autoCreatedRoleId), rootToken, nil)
	if verifyRoleStatus != http.StatusNotFound {
		t.Errorf("Expected 404 Not Found for auto-created role after operator deletion, got %d", verifyRoleStatus)
	}
}

func TestOperator_CreatorRole_RootExemption_Positive(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken, _, _, _, _, _ := getLiveTestTokensAndFixtures(t)

	uniqueSuffix := fmt.Sprintf("%d", time.Now().UnixNano()%1000000)
	regId := fmt.Sprintf("root-reg-%s", uniqueSuffix)
	opName := fmt.Sprintf("RootOp-%s", uniqueSuffix)

	createPayload := map[string]interface{}{
		"name":           opName,
		"description":    "Operator created by ROOT",
		"registrationId": regId,
	}

	// ROOT creates an Operator
	status, body, err := client.DoRequest("POST", "/operator/0", rootToken, createPayload)
	if err != nil {
		t.Fatalf("POST /operator/0 failed: %v", err)
	}
	if status != http.StatusOK && status != http.StatusCreated {
		t.Fatalf("Expected 200/201 on Operator creation by ROOT, got %d. Body: %s", status, string(body))
	}

	var createdOp operatorResponse
	if err := json.Unmarshal(body, &createdOp); err != nil {
		t.Fatalf("Failed to decode Operator response: %v", err)
	}

	defer func() {
		_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/operator/%s", createdOp.ID), rootToken, nil)
	}()

	// ROOT can access the Operator directly without requiring a local scoped role
	getStatus, getBody, err := client.DoRequest("GET", fmt.Sprintf("/operator/%s", createdOp.ID), rootToken, nil)
	if err != nil || getStatus != http.StatusOK {
		t.Errorf("Expected 200 OK for ROOT GET operator, got %d. Body: %s", getStatus, string(getBody))
	}
}

func TestOperator_Creation_MissingPermission_Negative(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken, _, _, _, _, _ := getLiveTestTokensAndFixtures(t)

	// Discover or set a user token with only operator:READ
	readOnlyToken := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-operator-token")
	_ = rootToken

	payload := map[string]interface{}{
		"name":           fmt.Sprintf("unauth-op-%d", time.Now().UnixNano()%100000),
		"registrationId": fmt.Sprintf("unauth-%d", time.Now().UnixNano()%100000),
	}

	status, body, err := client.DoRequest("POST", "/operator/0", readOnlyToken, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusForbidden {
		t.Errorf("Expected 403 Access Denied for operator creation without operator:CREATE, got %d. Body: %s", status, string(body))
	}
}

func TestOperator_Creation_DuplicateRegistrationId_Negative(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken, _, _, _, _, _ := getLiveTestTokensAndFixtures(t)

	// Discover existing operator registrationId
	var existingRegId string
	cmd := exec.Command("docker", "exec", "openwifi-postgresql-1", "psql", "-U", "owprov", "-d", "owprov", "-t", "-A", "-c",
		"SELECT registrationid FROM operators2 WHERE registrationid != '' LIMIT 1;")
	out, err := cmd.Output()
	if err == nil && len(strings.TrimSpace(string(out))) > 0 {
		existingRegId = strings.TrimSpace(string(out))
	} else {
		existingRegId = "111"
	}

	payload := map[string]interface{}{
		"name":           "duplicate-reg-operator",
		"registrationId": existingRegId,
	}

	status, body, err := client.DoRequest("POST", "/operator/0", rootToken, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest {
		t.Errorf("Expected 400 Bad Request for duplicate registrationId, got %d. Body: %s", status, string(body))
	}
}

func TestOperator_Creation_EntityIdInPayload_Negative(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken, _, _, _, _, _ := getLiveTestTokensAndFixtures(t)

	payload := map[string]interface{}{
		"name":           "entityId-in-payload",
		"registrationId": fmt.Sprintf("invalid-%d", time.Now().UnixNano()%100000),
		"entityId":       "0000-0000-0000",
	}

	status, body, err := client.DoRequest("POST", "/operator/0", rootToken, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest {
		t.Errorf("Expected 400 Bad Request when entityId is supplied in body, got %d. Body: %s", status, string(body))
	}
}

func TestOperator_CrossTenant_Isolation_Negative(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken, _, _, _, _, _ := getLiveTestTokensAndFixtures(t)

	// Fetch an existing operator ID
	cmd := exec.Command("docker", "exec", "openwifi-postgresql-1", "psql", "-U", "owprov", "-d", "owprov", "-t", "-A", "-c",
		"SELECT id FROM operators2 WHERE defaultoperator = false LIMIT 1;")
	out, err := cmd.Output()
	var opId string
	if err == nil && len(strings.TrimSpace(string(out))) > 10 {
		opId = strings.TrimSpace(string(out))
	} else {
		opId = "8a61f770-4f1b-471c-b5a4-9ece0e456b12"
	}

	// Use an unassigned or foreign token
	tokenOther := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-operator-token")
	_ = rootToken

	status, body, err := client.DoRequest("GET", fmt.Sprintf("/operator/%s", opId), tokenOther, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusForbidden {
		t.Errorf("Expected 403 Access Denied for unauthorized tenant querying Operator %s, got %d. Body: %s", opId, status, string(body))
	}
}

/*
 * TestOperator_Deletion_AtomicMultiRoleCleanup_Positive
 *
 * VALIDATES:
 *   Issue 1 (P1): Operator deletion atomic cleanup across companion entity and management roles.
 *   - Verifies that deleting an Operator atomically cascades to delete its companion Entity
 *     and all attached Management Roles in one single bulk operation without record caps.
 */
func TestOperator_Deletion_AtomicMultiRoleCleanup_Positive(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken, _, adminUserId, _, _, policyId := getLiveTestTokensAndFixtures(t)

	uniqueSuffix := fmt.Sprintf("%d", time.Now().UnixNano()%1000000)
	regId := fmt.Sprintf("atomic-op-%s", uniqueSuffix)
	opName := fmt.Sprintf("AtomicOp-%s", uniqueSuffix)

	createPayload := map[string]interface{}{
		"name":           opName,
		"description":    "Operator for atomic deletion test",
		"registrationId": regId,
	}

	// 1. Create Operator (provisions companion entity + creator role)
	status, body, err := client.DoRequest("POST", "/operator/0", rootToken, createPayload)
	if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
		t.Fatalf("Failed to create test operator: %v, body: %s", err, string(body))
	}

	var createdOp operatorResponse
	_ = json.Unmarshal(body, &createdOp)
	if createdOp.ID == "" || createdOp.EntityID == "" {
		t.Fatalf("Operator created without ID or EntityID: %+v", createdOp)
	}

	// 2. Attach multiple additional management roles to the companion entity
	roleIds := []string{}
	for i := 1; i <= 3; i++ {
		rolePayload := map[string]interface{}{
			"name":             fmt.Sprintf("atomic-role-%d-%s", i, uniqueSuffix),
			"description":      fmt.Sprintf("Test role %d on companion entity", i),
			"entity":           createdOp.EntityID,
			"venue":            "",
			"managementPolicy": policyId,
			"users":            []string{adminUserId},
		}
		var createdRole struct {
			ID string `json:"id"`
		}
		rStatus, rBody, rErr := client.DoRequest("POST", "/managementRole/0", rootToken, rolePayload)
		if rErr == nil && (rStatus == http.StatusOK || rStatus == http.StatusCreated) {
			_ = json.Unmarshal(rBody, &createdRole)
			if createdRole.ID != "" {
				roleIds = append(roleIds, createdRole.ID)
			}
		}
	}

	// 3. Delete Operator
	delStatus, delBody, delErr := client.DoRequest("DELETE", fmt.Sprintf("/operator/%s", createdOp.ID), rootToken, nil)
	if delErr != nil || delStatus != http.StatusOK {
		t.Fatalf("DELETE /operator/%s failed: status %d, body: %s", createdOp.ID, delStatus, string(delBody))
	}

	// 4. Verify Operator is deleted (404 Not Found)
	getOpStatus, _, _ := client.DoRequest("GET", fmt.Sprintf("/operator/%s", createdOp.ID), rootToken, nil)
	if getOpStatus != http.StatusNotFound {
		t.Errorf("Expected 404 Not Found for deleted Operator, got %d", getOpStatus)
	}

	// 5. Verify Companion Entity is deleted (404 Not Found)
	getEntStatus, _, _ := client.DoRequest("GET", fmt.Sprintf("/entity/%s", createdOp.EntityID), rootToken, nil)
	if getEntStatus != http.StatusNotFound {
		t.Errorf("Expected 404 Not Found for companion Entity, got %d", getEntStatus)
	}

	// 6. Verify all attached roles are deleted (404 Not Found)
	for _, rId := range roleIds {
		getRoleStatus, _, _ := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", rId), rootToken, nil)
		if getRoleStatus != http.StatusNotFound {
			t.Errorf("Expected 404 Not Found for role %s after operator deletion, got %d", rId, getRoleStatus)
		}
	}

	// 7. Verify no roles remain matching entity
	listStatus, listBody, _ := client.DoRequest("GET", fmt.Sprintf("/managementRole?entity=%s", createdOp.EntityID), rootToken, nil)
	if listStatus == http.StatusOK {
		var resp struct {
			Roles []interface{} `json:"roles"`
		}
		_ = json.Unmarshal(listBody, &resp)
		if len(resp.Roles) != 0 {
			t.Errorf("Expected 0 roles remaining for entity %s, got %d", createdOp.EntityID, len(resp.Roles))
		}
	}
}


