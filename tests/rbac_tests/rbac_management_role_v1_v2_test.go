package rbac_tests

import (
	"encoding/json"
	"fmt"
	"net/http"
	"testing"
	"time"
)

// Helper structure to parse V2 role response envelope
type v2RolesEnvelope struct {
	Roles []struct {
		ID               string   `json:"id"`
		Name             string   `json:"name"`
		Description      string   `json:"description"`
		Entity           string   `json:"entity"`
		Venue            string   `json:"venue"`
		ManagementPolicy string   `json:"managementPolicy"`
		Users            []string `json:"users"`
	} `json:"roles"`
}

// Helper structure to parse single role response (V1 or V2 GET/PUT)
type singleRoleResponse struct {
	ID               string   `json:"id"`
	Name             string   `json:"name"`
	Description      string   `json:"description"`
	Entity           string   `json:"entity"`
	Venue            string   `json:"venue"`
	ManagementPolicy string   `json:"managementPolicy"`
	Users            []string `json:"users"`
}

type testFixtures struct {
	entityA     string
	entityB     string
	venueA1     string
	venueA2     string
	venueB1     string
	policyValid string
	userValid   string
	token       string
}

func getTestFixtures(t *testing.T, clientV1 *TestClient) testFixtures {
	token := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	if envToken := getEnvOrDefault("TEST_TOKEN", ""); envToken != "" {
		token = envToken
	}

	fixtures := testFixtures{
		entityA:     getEnvOrDefault("OPERATOR_A_ENTITY_UUID", ""),
		entityB:     getEnvOrDefault("OPERATOR_B_ENTITY_UUID", ""),
		venueA1:     getEnvOrDefault("VENUE_A1_UUID", ""),
		venueA2:     getEnvOrDefault("VENUE_A2_UUID", ""),
		venueB1:     getEnvOrDefault("VENUE_B1_UUID", ""),
		policyValid: getEnvOrDefault("POLICY_VALID_ID", ""),
		userValid:   getEnvOrDefault("USER_VALID_ID", ""),
		token:       token,
	}

	// 1. Discover existing entities
	var entResp struct {
		Entities []struct {
			ID     string   `json:"id"`
			Venues []string `json:"venues"`
		} `json:"entities"`
	}
	if status, body, err := clientV1.DoRequest("GET", "/entity", token, nil); err == nil && status == http.StatusOK {
		_ = json.Unmarshal(body, &entResp)
	}

	// Select or create entityA
	if fixtures.entityA == "" {
		for _, ent := range entResp.Entities {
			if ent.ID != "" && ent.ID != "0000-0000-0000" {
				fixtures.entityA = ent.ID
				if len(ent.Venues) >= 1 && fixtures.venueA1 == "" {
					fixtures.venueA1 = ent.Venues[0]
				}
				if len(ent.Venues) >= 2 && fixtures.venueA2 == "" {
					fixtures.venueA2 = ent.Venues[1]
				}
				break
			}
		}
	}

	if fixtures.entityA == "" {
		payload := map[string]interface{}{
			"name":   fmt.Sprintf("fixture-entity-a-%d", time.Now().UnixNano()),
			"parent": "0000-0000-0000",
		}
		if status, body, err := clientV1.DoRequest("POST", "/entity/0", token, payload); err == nil && (status == http.StatusOK || status == http.StatusCreated) {
			var created struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal(body, &created); err == nil && created.ID != "" {
				fixtures.entityA = created.ID
			}
		}
	}

	// Ensure entityA has at least 2 distinct venues
	if fixtures.entityA != "" {
		// Fetch fresh entity details if venues are not populated
		if fixtures.venueA1 == "" || fixtures.venueA2 == "" {
			var entDetail struct {
				Venues []string `json:"venues"`
			}
			if status, body, err := clientV1.DoRequest("GET", fmt.Sprintf("/entity/%s", fixtures.entityA), token, nil); err == nil && status == http.StatusOK {
				if err := json.Unmarshal(body, &entDetail); err == nil {
					if len(entDetail.Venues) >= 1 && fixtures.venueA1 == "" {
						fixtures.venueA1 = entDetail.Venues[0]
					}
					if len(entDetail.Venues) >= 2 && fixtures.venueA2 == "" {
						fixtures.venueA2 = entDetail.Venues[1]
					}
				}
			}
		}

		if fixtures.venueA1 == "" {
			payload := map[string]interface{}{
				"name":   fmt.Sprintf("fixture-venue-a1-%d", time.Now().UnixNano()),
				"entity": fixtures.entityA,
			}
			if status, body, err := clientV1.DoRequest("POST", "/venue/0", token, payload); err == nil && (status == http.StatusOK || status == http.StatusCreated) {
				var created struct {
					ID string `json:"id"`
				}
				if err := json.Unmarshal(body, &created); err == nil && created.ID != "" {
					fixtures.venueA1 = created.ID
				}
			}
		}

		if fixtures.venueA2 == "" || fixtures.venueA2 == fixtures.venueA1 {
			payload := map[string]interface{}{
				"name":   fmt.Sprintf("fixture-venue-a2-%d", time.Now().UnixNano()),
				"entity": fixtures.entityA,
			}
			if status, body, err := clientV1.DoRequest("POST", "/venue/0", token, payload); err == nil && (status == http.StatusOK || status == http.StatusCreated) {
				var created struct {
					ID string `json:"id"`
				}
				if err := json.Unmarshal(body, &created); err == nil && created.ID != "" {
					fixtures.venueA2 = created.ID
				}
			}
		}
	}

	// Select or create entityB and venueB1 for cross-entity tests
	if fixtures.entityB == "" {
		for _, ent := range entResp.Entities {
			if ent.ID != "" && ent.ID != "0000-0000-0000" && ent.ID != fixtures.entityA {
				fixtures.entityB = ent.ID
				if len(ent.Venues) >= 1 && fixtures.venueB1 == "" {
					fixtures.venueB1 = ent.Venues[0]
				}
				break
			}
		}
	}

	if fixtures.entityB == "" {
		payload := map[string]interface{}{
			"name":   fmt.Sprintf("fixture-entity-b-%d", time.Now().UnixNano()),
			"parent": "0000-0000-0000",
		}
		if status, body, err := clientV1.DoRequest("POST", "/entity/0", token, payload); err == nil && (status == http.StatusOK || status == http.StatusCreated) {
			var created struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal(body, &created); err == nil && created.ID != "" {
				fixtures.entityB = created.ID
			}
		}
	}

	if fixtures.entityB != "" && fixtures.venueB1 == "" {
		var entDetail struct {
			Venues []string `json:"venues"`
		}
		if status, body, err := clientV1.DoRequest("GET", fmt.Sprintf("/entity/%s", fixtures.entityB), token, nil); err == nil && status == http.StatusOK {
			if err := json.Unmarshal(body, &entDetail); err == nil && len(entDetail.Venues) >= 1 {
				fixtures.venueB1 = entDetail.Venues[0]
			}
		}
		if fixtures.venueB1 == "" {
			payload := map[string]interface{}{
				"name":   fmt.Sprintf("fixture-venue-b1-%d", time.Now().UnixNano()),
				"entity": fixtures.entityB,
			}
			if status, body, err := clientV1.DoRequest("POST", "/venue/0", token, payload); err == nil && (status == http.StatusOK || status == http.StatusCreated) {
				var created struct {
					ID string `json:"id"`
				}
				if err := json.Unmarshal(body, &created); err == nil && created.ID != "" {
					fixtures.venueB1 = created.ID
				}
			}
		}
	}

	// Discover existing valid policy, or create one
	if fixtures.policyValid == "" {
		var polResp struct {
			ManagementPolicies []struct {
				ID string `json:"id"`
			} `json:"managementPolicies"`
		}
		if status, body, err := clientV1.DoRequest("GET", "/managementPolicy", token, nil); err == nil && status == http.StatusOK {
			if err := json.Unmarshal(body, &polResp); err == nil && len(polResp.ManagementPolicies) > 0 {
				fixtures.policyValid = polResp.ManagementPolicies[0].ID
			}
		}
	}

	if fixtures.policyValid == "" {
		payload := map[string]interface{}{
			"name":        fmt.Sprintf("fixture-policy-%d", time.Now().UnixNano()),
			"description": "Auto-created test policy",
			"entries": []map[string]interface{}{
				{
					"resources": []string{"*"},
					"access":    []string{"*"},
				},
			},
		}
		if status, body, err := clientV1.DoRequest("POST", "/managementPolicy/0", token, payload); err == nil && (status == http.StatusOK || status == http.StatusCreated) {
			var created struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal(body, &created); err == nil && created.ID != "" {
				fixtures.policyValid = created.ID
			}
		}
	}

	// Discover existing user from management roles or fall back to default root user
	if fixtures.userValid == "" {
		var roleList struct {
			Roles []struct {
				Users []string `json:"users"`
			} `json:"roles"`
		}
		if status, body, err := clientV1.DoRequest("GET", "/managementRole", token, nil); err == nil && status == http.StatusOK {
			if err := json.Unmarshal(body, &roleList); err == nil {
				for _, r := range roleList.Roles {
					if len(r.Users) > 0 && r.Users[0] != "" {
						fixtures.userValid = r.Users[0]
						break
					}
				}
			}
		}
	}

	if fixtures.userValid == "" {
		fixtures.userValid = "00000000-0000-0000-0000-000000000001"
	}

	return fixtures
}

// ============================================================================
// V1 MANAGEMENT ROLE TESTS (/api/v1/managementRole/{id})
// ============================================================================

func TestManagementRole_V1_SingleVenue_Creation_Positive(t *testing.T) {
	baseURL := getEnvOrDefault("OWPROV_V1_URL", "https://localhost:16005/api/v1")
	client := NewTestClient(baseURL)
	fixtures := getTestFixtures(t, client)

	roleName := "v1-test-single-venue-role"
	payload := map[string]interface{}{
		"name":             roleName,
		"description":      "Test V1 single venue creation",
		"entity":           fixtures.entityA,
		"venue":            fixtures.venueA1,
		"managementPolicy": fixtures.policyValid,
		"users":            []string{fixtures.userValid},
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
	if err != nil {
		t.Fatalf("V1 request failed: %v", err)
	}
	if status != http.StatusOK && status != http.StatusCreated {
		t.Fatalf("Expected 200/201 from V1 POST, got %d. Body: %s", status, string(body))
	}

	// Verify response is a single object, NOT wrapped in { "roles": [...] }
	var rawMap map[string]interface{}
	if err := json.Unmarshal(body, &rawMap); err != nil {
		t.Fatalf("Failed to parse V1 response JSON: %v", err)
	}
	if _, hasRoles := rawMap["roles"]; hasRoles {
		t.Fatalf("V1 response must NOT contain 'roles' array envelope. Got: %s", string(body))
	}

	createdID, ok := rawMap["id"].(string)
	if !ok || createdID == "" {
		t.Fatalf("V1 response missing 'id'. Got: %s", string(body))
	}
	if rawMap["venue"] != fixtures.venueA1 {
		t.Errorf("Expected venue %s, got %v", fixtures.venueA1, rawMap["venue"])
	}

	// Cleanup
	client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", createdID), fixtures.token, nil)
}

func TestManagementRole_V1_EntityWide_Creation_Positive(t *testing.T) {
	baseURL := getEnvOrDefault("OWPROV_V1_URL", "https://localhost:16005/api/v1")
	client := NewTestClient(baseURL)
	fixtures := getTestFixtures(t, client)

	roleName := "v1-test-entity-wide-role"
	payload := map[string]interface{}{
		"name":             roleName,
		"description":      "Test V1 entity wide creation with empty venue",
		"entity":           fixtures.entityA,
		"venue":            "",
		"managementPolicy": fixtures.policyValid,
		"users":            []string{fixtures.userValid},
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
	if err != nil {
		t.Fatalf("V1 request failed: %v", err)
	}
	if status != http.StatusOK && status != http.StatusCreated {
		t.Fatalf("Expected 200/201 from V1 entity-wide POST, got %d. Body: %s", status, string(body))
	}

	var resp singleRoleResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("Failed to parse V1 response: %v", err)
	}
	if resp.ID == "" {
		t.Fatalf("V1 response missing 'id'. Body: %s", string(body))
	}
	if resp.Venue != "" {
		t.Errorf("Expected empty venue for entity-wide role, got %s", resp.Venue)
	}

	// Cleanup
	client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", resp.ID), fixtures.token, nil)
}

func TestManagementRole_V1_Negative_Scenarios(t *testing.T) {
	baseURL := getEnvOrDefault("OWPROV_V1_URL", "https://localhost:16005/api/v1")
	client := NewTestClient(baseURL)
	fixtures := getTestFixtures(t, client)

	t.Run("Negative: Non-existent policy returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             "v1-neg-nonexistent-policy",
			"entity":           fixtures.entityA,
			"venue":            fixtures.venueA1,
			"managementPolicy": "00000000-0000-0000-0000-000000000000",
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for non-existent policy, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Empty policy returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             "v1-neg-empty-policy",
			"entity":           fixtures.entityA,
			"venue":            fixtures.venueA1,
			"managementPolicy": "",
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for empty policy, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Non-existent venue returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             "v1-neg-nonexistent-venue",
			"entity":           fixtures.entityA,
			"venue":            "00000000-0000-0000-0000-000000000000",
			"managementPolicy": fixtures.policyValid,
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for non-existent venue, got %d. Body: %s", status, string(body))
		}
	})
}

// ============================================================================
// V2 MANAGEMENT ROLE TESTS (/api/v2/managementRole/{id})
// ============================================================================

func TestManagementRole_V2_MultiVenue_BatchCreation_Positive(t *testing.T) {
	baseURL := getEnvOrDefault("OWPROV_V2_URL", "https://localhost:16005/api/v2")
	client := NewTestClient(baseURL)
	clientV1 := NewTestClient(getEnvOrDefault("OWPROV_V1_URL", "https://localhost:16005/api/v1"))
	fixtures := getTestFixtures(t, clientV1)

	roleName := "v2-test-multivenue-role"
	targetVenues := []string{fixtures.venueA1, fixtures.venueA2}
	payload := map[string]interface{}{
		"name":             roleName,
		"description":      "Test V2 batch multi-venue creation",
		"entity":           fixtures.entityA,
		"venueIds":         targetVenues,
		"managementPolicy": fixtures.policyValid,
		"users":            []string{fixtures.userValid},
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
	if err != nil {
		t.Fatalf("V2 request failed: %v", err)
	}
	if status != http.StatusOK && status != http.StatusCreated {
		t.Fatalf("Expected 200/201 from V2 batch POST, got %d. Body: %s", status, string(body))
	}

	var env v2RolesEnvelope
	if err := json.Unmarshal(body, &env); err != nil {
		t.Fatalf("Failed to parse V2 response envelope: %v. Body: %s", err, string(body))
	}

	if len(env.Roles) != 2 {
		t.Fatalf("Expected 2 roles in V2 response, got %d. Body: %s", len(env.Roles), string(body))
	}

	createdVenues := make(map[string]bool)
	for _, r := range env.Roles {
		if r.ID == "" {
			t.Errorf("Role in V2 envelope has empty ID")
		}
		if r.Entity != fixtures.entityA {
			t.Errorf("Expected entity %s, got %s", fixtures.entityA, r.Entity)
		}
		if r.ManagementPolicy != fixtures.policyValid {
			t.Errorf("Expected policy %s, got %s", fixtures.policyValid, r.ManagementPolicy)
		}
		createdVenues[r.Venue] = true

		// Cleanup
		client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", r.ID), fixtures.token, nil)
	}

	if !createdVenues[fixtures.venueA1] || !createdVenues[fixtures.venueA2] {
		t.Errorf("Not all target venues created. Venues created: %+v", createdVenues)
	}
}

func TestManagementRole_V2_SingleVenueInArray_Positive(t *testing.T) {
	baseURL := getEnvOrDefault("OWPROV_V2_URL", "https://localhost:16005/api/v2")
	client := NewTestClient(baseURL)
	clientV1 := NewTestClient(getEnvOrDefault("OWPROV_V1_URL", "https://localhost:16005/api/v1"))
	fixtures := getTestFixtures(t, clientV1)

	payload := map[string]interface{}{
		"name":             "v2-test-single-venue-array",
		"description":      "Single venue in venueIds array",
		"entity":           fixtures.entityA,
		"venueIds":         []string{fixtures.venueA1},
		"managementPolicy": fixtures.policyValid,
		"users":            []string{fixtures.userValid},
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
	if err != nil {
		t.Fatalf("V2 request failed: %v", err)
	}
	if status != http.StatusOK && status != http.StatusCreated {
		t.Fatalf("Expected 200/201 from V2 single venue POST, got %d. Body: %s", status, string(body))
	}

	var env v2RolesEnvelope
	if err := json.Unmarshal(body, &env); err != nil {
		t.Fatalf("Failed to parse V2 response envelope: %v. Body: %s", err, string(body))
	}

	if len(env.Roles) != 1 {
		t.Fatalf("Expected 1 role in V2 response, got %d. Body: %s", len(env.Roles), string(body))
	}
	if env.Roles[0].Venue != fixtures.venueA1 {
		t.Errorf("Expected venue %s, got %s", fixtures.venueA1, env.Roles[0].Venue)
	}

	// Cleanup
	client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", env.Roles[0].ID), fixtures.token, nil)
}

func TestManagementRole_V2_EntityWide_Creation_Positive(t *testing.T) {
	baseURL := getEnvOrDefault("OWPROV_V2_URL", "https://localhost:16005/api/v2")
	client := NewTestClient(baseURL)
	clientV1 := NewTestClient(getEnvOrDefault("OWPROV_V1_URL", "https://localhost:16005/api/v1"))
	fixtures := getTestFixtures(t, clientV1)

	payload := map[string]interface{}{
		"name":             "v2-test-entity-wide",
		"description":      "Empty venueIds array for entity wide role",
		"entity":           fixtures.entityA,
		"venueIds":         []string{},
		"managementPolicy": fixtures.policyValid,
		"users":            []string{fixtures.userValid},
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
	if err != nil {
		t.Fatalf("V2 request failed: %v", err)
	}
	if status != http.StatusOK && status != http.StatusCreated {
		t.Fatalf("Expected 200/201 from V2 entity-wide POST, got %d. Body: %s", status, string(body))
	}

	var env v2RolesEnvelope
	if err := json.Unmarshal(body, &env); err != nil {
		t.Fatalf("Failed to parse V2 response envelope: %v. Body: %s", err, string(body))
	}

	if len(env.Roles) != 1 {
		t.Fatalf("Expected 1 role in V2 entity-wide response, got %d. Body: %s", len(env.Roles), string(body))
	}
	if env.Roles[0].Venue != "" {
		t.Errorf("Expected empty venue string, got '%s'", env.Roles[0].Venue)
	}

	// Cleanup
	client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", env.Roles[0].ID), fixtures.token, nil)
}

func TestManagementRole_V2_CRUD_Lifecycle_Positive(t *testing.T) {
	baseURL := getEnvOrDefault("OWPROV_V2_URL", "https://localhost:16005/api/v2")
	client := NewTestClient(baseURL)
	clientV1 := NewTestClient(getEnvOrDefault("OWPROV_V1_URL", "https://localhost:16005/api/v1"))
	fixtures := getTestFixtures(t, clientV1)

	// 1. CREATE
	createPayload := map[string]interface{}{
		"name":             "v2-crud-lifecycle-role",
		"description":      "Initial description",
		"entity":           fixtures.entityA,
		"venueIds":         []string{fixtures.venueA1},
		"managementPolicy": fixtures.policyValid,
		"users":            []string{fixtures.userValid},
	}
	status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, createPayload)
	if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
		t.Fatalf("Create failed: status %d, body: %s", status, string(body))
	}

	var env v2RolesEnvelope
	json.Unmarshal(body, &env)
	roleID := env.Roles[0].ID

	// 2. READ (GET)
	status, body, err = client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", roleID), fixtures.token, nil)
	if err != nil || status != http.StatusOK {
		t.Fatalf("GET /api/v2/managementRole/%s failed: status %d, body: %s", roleID, status, string(body))
	}
	var readRole singleRoleResponse
	json.Unmarshal(body, &readRole)
	if readRole.ID != roleID || readRole.Name != "v2-crud-lifecycle-role" {
		t.Errorf("GET response mismatch: %+v", readRole)
	}

	// 3. UPDATE (PUT)
	updatePayload := map[string]interface{}{
		"name":        "v2-crud-lifecycle-updated",
		"description": "Updated description",
	}
	status, body, err = client.DoRequest("PUT", fmt.Sprintf("/managementRole/%s", roleID), fixtures.token, updatePayload)
	if err != nil || status != http.StatusOK {
		t.Fatalf("PUT /api/v2/managementRole/%s failed: status %d, body: %s", roleID, status, string(body))
	}

	// Verify update via GET
	status, body, _ = client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", roleID), fixtures.token, nil)
	json.Unmarshal(body, &readRole)
	if readRole.Name != "v2-crud-lifecycle-updated" {
		t.Errorf("Updated name not reflected: got %s", readRole.Name)
	}

	// 4. DELETE
	status, body, err = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", roleID), fixtures.token, nil)
	if err != nil || status != http.StatusOK {
		t.Fatalf("DELETE /api/v2/managementRole/%s failed: status %d, body: %s", roleID, status, string(body))
	}

	// Verify deletion
	status, _, _ = client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", roleID), fixtures.token, nil)
	if status == http.StatusOK {
		t.Errorf("Expected role to be deleted, but GET returned 200 OK")
	}
}

func TestManagementRole_V2_Negative_Scenarios(t *testing.T) {
	baseURL := getEnvOrDefault("OWPROV_V2_URL", "https://localhost:16005/api/v2")
	client := NewTestClient(baseURL)
	clientV1 := NewTestClient(getEnvOrDefault("OWPROV_V1_URL", "https://localhost:16005/api/v1"))
	fixtures := getTestFixtures(t, clientV1)

	t.Run("Negative: Non-existent venue UUID in venueIds returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             "v2-neg-nonexistent-venue",
			"entity":           fixtures.entityA,
			"venueIds":         []string{"00000000-0000-0000-0000-000000000000"},
			"managementPolicy": fixtures.policyValid,
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for non-existent venue UUID, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Cross-entity venue in venueIds returns 400 Bad Request", func(t *testing.T) {
		if fixtures.venueB1 == "" {
			t.Skip("No secondary entity venue available to test cross-entity check")
		}
		payload := map[string]interface{}{
			"name":             "v2-neg-cross-entity",
			"entity":           fixtures.entityA,
			"venueIds":         []string{fixtures.venueB1}, // Belongs to entityB!
			"managementPolicy": fixtures.policyValid,
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for cross-entity venue, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Non-existent policy returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             "v2-neg-nonexistent-policy",
			"entity":           fixtures.entityA,
			"venueIds":         []string{fixtures.venueA1},
			"managementPolicy": "00000000-0000-0000-0000-000000000000",
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for non-existent policy, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Empty policy returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             "v2-neg-empty-policy",
			"entity":           fixtures.entityA,
			"venueIds":         []string{fixtures.venueA1},
			"managementPolicy": "",
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for empty policy, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Non-array string venueIds returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             "v2-neg-string-venueids",
			"entity":           fixtures.entityA,
			"venueIds":         fixtures.venueA1, // String instead of array!
			"managementPolicy": fixtures.policyValid,
			"users":            []string{fixtures.userValid},
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for string venueIds, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Non-array object venueIds returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             "v2-neg-object-venueids",
			"entity":           fixtures.entityA,
			"venueIds":         map[string]string{"id": fixtures.venueA1}, // Object instead of array!
			"managementPolicy": fixtures.policyValid,
			"users":            []string{fixtures.userValid},
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for object venueIds, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Empty string in venueIds returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             "v2-neg-empty-string-venueids",
			"entity":           fixtures.entityA,
			"venueIds":         []string{""},
			"managementPolicy": fixtures.policyValid,
			"users":            []string{fixtures.userValid},
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for empty string venueIds element, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Whitespace string in venueIds returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             "v2-neg-whitespace-venueids",
			"entity":           fixtures.entityA,
			"venueIds":         []string{"   "},
			"managementPolicy": fixtures.policyValid,
			"users":            []string{fixtures.userValid},
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for whitespace venueIds element, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Non-string element in venueIds returns 400 Bad Request", func(t *testing.T) {
		payload := map[string]interface{}{
			"name":             "v2-neg-non-string-venueids",
			"entity":           fixtures.entityA,
			"venueIds":         []interface{}{123},
			"managementPolicy": fixtures.policyValid,
			"users":            []string{fixtures.userValid},
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for non-string venueIds element, got %d. Body: %s", status, string(body))
		}
	})
}

func TestManagementRolePolicyQueryFilter(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	fixtures := getTestFixtures(t, client)

	roleName := fmt.Sprintf("test-policy-filter-%d", time.Now().UnixNano())
	createPayload := map[string]interface{}{
		"name":             roleName,
		"entity":           fixtures.entityA,
		"managementPolicy": fixtures.policyValid,
		"users":            []string{fixtures.userValid},
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, createPayload)
	if err != nil {
		t.Fatalf("POST /managementRole/0 failed: %v", err)
	}
	if status != http.StatusOK {
		t.Fatalf("Failed to create test role: status %d, body: %s", status, string(body))
	}

	var createdRole struct {
		ID string `json:"id"`
	}
	if err := json.Unmarshal(body, &createdRole); err != nil {
		t.Fatalf("Failed to unmarshal created role: %v", err)
	}
	defer client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", createdRole.ID), fixtures.token, nil)

	t.Run("Positive: Filter roles by policyId returns matching role", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s", fixtures.policyValid), fixtures.token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?policyId failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []struct {
				ID               string `json:"id"`
				ManagementPolicy string `json:"managementPolicy"`
			} `json:"roles"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse response: %v", err)
		}

		found := false
		for _, r := range resp.Roles {
			if r.ManagementPolicy != fixtures.policyValid {
				t.Errorf("Expected managementPolicy %s, got %s for role %s", fixtures.policyValid, r.ManagementPolicy, r.ID)
			}
			if r.ID == createdRole.ID {
				found = true
			}
		}
		if !found {
			t.Errorf("Expected role %s in results for policyId %s", createdRole.ID, fixtures.policyValid)
		}
	})

	t.Run("Positive: Filter roles by policyId and user returns matching role", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s&user=%s", fixtures.policyValid, fixtures.userValid), fixtures.token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?policyId&user failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []struct {
				ID               string   `json:"id"`
				ManagementPolicy string   `json:"managementPolicy"`
				Users            []string `json:"users"`
			} `json:"roles"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse response: %v", err)
		}

		found := false
		for _, r := range resp.Roles {
			if r.ID == createdRole.ID {
				found = true
			}
		}
		if !found {
			t.Errorf("Expected role %s in results for policyId %s and user %s", createdRole.ID, fixtures.policyValid, fixtures.userValid)
		}
	})

	t.Run("Positive: Filter roles by non-matching policy returns empty array", func(t *testing.T) {
		dummyPolicyID := "00000000-0000-0000-0000-999999999999"
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s", dummyPolicyID), fixtures.token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?policyId failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []interface{} `json:"roles"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse response: %v", err)
		}
		if len(resp.Roles) != 0 {
			t.Errorf("Expected 0 roles for non-existent policy, got %d", len(resp.Roles))
		}
	})

	venueRoleName := fmt.Sprintf("test-venue-filter-%d", time.Now().UnixNano())
	venuePayload := map[string]interface{}{
		"name":             venueRoleName,
		"entity":           fixtures.entityA,
		"venue":            fixtures.venueA1,
		"managementPolicy": fixtures.policyValid,
		"users":            []string{fixtures.userValid},
	}

	var createdVenueRole struct {
		ID string `json:"id"`
	}
	if status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, venuePayload); err == nil && status == http.StatusOK {
		_ = json.Unmarshal(body, &createdVenueRole)
	}
	if createdVenueRole.ID != "" {
		defer client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", createdVenueRole.ID), fixtures.token, nil)
	}

	t.Run("Positive: Filter roles by policyId and venue returns matching role", func(t *testing.T) {
		if createdVenueRole.ID == "" {
			t.Fatal("Test venue role was not created")
		}
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s&venue=%s", fixtures.policyValid, fixtures.venueA1), fixtures.token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?policyId&venue failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []struct {
				ID               string `json:"id"`
				ManagementPolicy string `json:"managementPolicy"`
				Venue            string `json:"venue"`
			} `json:"roles"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse response: %v", err)
		}

		found := false
		for _, r := range resp.Roles {
			if r.ID == createdVenueRole.ID {
				found = true
				if r.Venue != fixtures.venueA1 {
					t.Errorf("Expected venue %s, got %s", fixtures.venueA1, r.Venue)
				}
			}
		}
		if !found {
			t.Errorf("Expected role %s in results for policyId %s and venue %s", createdVenueRole.ID, fixtures.policyValid, fixtures.venueA1)
		}
	})

	t.Run("Positive: Filter roles by venue alone returns matching role", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?venue=%s", fixtures.venueA1), fixtures.token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?venue failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []struct {
				ID    string `json:"id"`
				Venue string `json:"venue"`
			} `json:"roles"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse response: %v", err)
		}

		found := false
		for _, r := range resp.Roles {
			if r.ID == createdVenueRole.ID {
				found = true
				if r.Venue != fixtures.venueA1 {
					t.Errorf("Expected venue %s, got %s", fixtures.venueA1, r.Venue)
				}
			}
		}
		if !found {
			t.Errorf("Expected role %s in results for standalone venue %s", createdVenueRole.ID, fixtures.venueA1)
		}

		// Check countOnly=true
		status, body, err = client.DoRequest("GET", fmt.Sprintf("/managementRole?venue=%s&countOnly=true", fixtures.venueA1), fixtures.token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?venue&countOnly failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK for countOnly, got %d. Body: %s", status, string(body))
		}
		var countResp struct {
			Count int `json:"count"`
		}
		if err := json.Unmarshal(body, &countResp); err != nil {
			t.Fatalf("Failed to parse count response: %v", err)
		}
		if countResp.Count == 0 {
			t.Errorf("Expected count > 0 for venue %s, got 0", fixtures.venueA1)
		}
	})

	t.Run("Positive: Entity-scoped user can query venue-filtered role under their entity", func(t *testing.T) {
		tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "")
		if tokenAdminA == "" {
			t.Skip("TOKEN_ADMIN_OPERATOR_A not provided; skipping entity-scoped query test")
		}
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s&venue=%s", fixtures.policyValid, fixtures.venueA1), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?policyId&venue failed for entity admin: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK for entity admin querying venue in their entity, got %d. Body: %s", status, string(body))
		}

		var resp struct {
			Roles []struct {
				ID string `json:"id"`
			} `json:"roles"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse response: %v", err)
		}
		found := false
		for _, r := range resp.Roles {
			if r.ID == createdVenueRole.ID {
				found = true
				break
			}
		}
		if !found {
			t.Errorf("Expected role %s to be visible to entity admin for venue %s under their entity", createdVenueRole.ID, fixtures.venueA1)
		}

		// Also check countOnly=true for entity-scoped user
		status, body, err = client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s&venue=%s&countOnly=true", fixtures.policyValid, fixtures.venueA1), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?policyId&venue&countOnly failed for entity admin: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK for entity admin countOnly, got %d. Body: %s", status, string(body))
		}
		var countResp struct {
			Count int `json:"count"`
		}
		if err := json.Unmarshal(body, &countResp); err != nil {
			t.Fatalf("Failed to parse count response: %v", err)
		}
		if countResp.Count == 0 {
			t.Errorf("Expected count > 0 for entity admin countOnly, got 0")
		}
	})

	t.Run("Positive: Admin of Entity A querying roles under Entity B does not see them", func(t *testing.T) {
		tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "")
		if tokenAdminA == "" || fixtures.entityB == "" {
			t.Skip("TOKEN_ADMIN_OPERATOR_A or entityB not provided; skipping cross-tenant isolation test")
		}

		// Create a role under Entity B using root token
		bRolePayload := map[string]interface{}{
			"name":             fmt.Sprintf("entity-b-role-%d", time.Now().UnixNano()),
			"entity":           fixtures.entityB,
			"managementPolicy": fixtures.policyValid,
			"users":            []string{fixtures.userValid},
		}
		status, body, err := client.DoRequest("POST", "/managementRole/0", fixtures.token, bRolePayload)
		if err != nil || status != http.StatusOK {
			t.Skipf("Could not create test role under entity B: %v, status %d", err, status)
		}
		var createdRoleB struct {
			ID string `json:"id"`
		}
		_ = json.Unmarshal(body, &createdRoleB)
		if createdRoleB.ID != "" {
			defer client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", createdRoleB.ID), fixtures.token, nil)
		}

		// Admin A queries GET /managementRole?policyId=fixtures.policyValid
		status, body, err = client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s", fixtures.policyValid), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?policyId failed for Admin A: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}
		var resp struct {
			Roles []struct {
				ID string `json:"id"`
			} `json:"roles"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse response: %v", err)
		}
		for _, r := range resp.Roles {
			if r.ID == createdRoleB.ID {
				t.Errorf("Cross-tenant leakage: Admin A can see role %s belonging to Entity B!", createdRoleB.ID)
			}
		}

		// Also verify Admin A directly filtering by entity B returns empty list
		status, body, err = client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s&entity=%s", fixtures.policyValid, fixtures.entityB), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?policyId&entity failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}
		var respEntB struct {
			Roles []interface{} `json:"roles"`
		}
		_ = json.Unmarshal(body, &respEntB)
		if len(respEntB.Roles) != 0 {
			t.Errorf("Expected 0 roles when Admin A filters for Entity B, got %d", len(respEntB.Roles))
		}
	})

	t.Run("Positive: Explicit countOnly=true coverage across policyId filter combinations", func(t *testing.T) {
		// 1. policyId + countOnly
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s&countOnly=true", fixtures.policyValid), fixtures.token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?policyId&countOnly failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
		}
		var countResp struct {
			Count int `json:"count"`
		}
		if err := json.Unmarshal(body, &countResp); err != nil {
			t.Fatalf("Failed to parse count response: %v", err)
		}
		if countResp.Count < 1 {
			t.Errorf("Expected count >= 1 for policyId=%s, got %d", fixtures.policyValid, countResp.Count)
		}

		// 2. policyId + entity + countOnly
		if fixtures.entityA != "" {
			status, body, err = client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s&entity=%s&countOnly=true", fixtures.policyValid, fixtures.entityA), fixtures.token, nil)
			if err != nil {
				t.Fatalf("GET /managementRole?policyId&entity&countOnly failed: %v", err)
			}
			if status != http.StatusOK {
				t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
			}
			var countEntResp struct {
				Count int `json:"count"`
			}
			if err := json.Unmarshal(body, &countEntResp); err != nil {
				t.Fatalf("Failed to parse count response: %v", err)
			}
			if countEntResp.Count < 1 {
				t.Errorf("Expected count >= 1 for policyId + entity, got %d", countEntResp.Count)
			}
		}

		// 3. policyId + venue + countOnly
		if fixtures.venueA1 != "" {
			status, body, err = client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s&venue=%s&countOnly=true", fixtures.policyValid, fixtures.venueA1), fixtures.token, nil)
			if err != nil {
				t.Fatalf("GET /managementRole?policyId&venue&countOnly failed: %v", err)
			}
			if status != http.StatusOK {
				t.Fatalf("Expected 200 OK, got %d. Body: %s", status, string(body))
			}
			var countVenResp struct {
				Count int `json:"count"`
			}
			if err := json.Unmarshal(body, &countVenResp); err != nil {
				t.Fatalf("Failed to parse count response: %v", err)
			}
			if countVenResp.Count < 1 {
				t.Errorf("Expected count >= 1 for policyId + venue, got %d", countVenResp.Count)
			}
		}

		// 4. non-root + policyId + countOnly
		tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "")
		if tokenAdminA != "" {
			status, body, err = client.DoRequest("GET", fmt.Sprintf("/managementRole?policyId=%s&countOnly=true", fixtures.policyValid), tokenAdminA, nil)
			if err != nil {
				t.Fatalf("GET /managementRole?policyId&countOnly failed for non-root: %v", err)
			}
			if status != http.StatusOK {
				t.Fatalf("Expected 200 OK for non-root countOnly, got %d. Body: %s", status, string(body))
			}
			var countNonRootResp struct {
				Count int `json:"count"`
			}
			if err := json.Unmarshal(body, &countNonRootResp); err != nil {
				t.Fatalf("Failed to parse count response: %v", err)
			}
			if countNonRootResp.Count < 1 {
				t.Errorf("Expected count >= 1 for non-root policyId, got %d", countNonRootResp.Count)
			}
		}
	})

	t.Run("Positive: Non-root user querying unauthorized venue returns empty list", func(t *testing.T) {
		tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "")
		if tokenAdminA == "" || fixtures.venueB1 == "" {
			t.Skip("TOKEN_ADMIN_OPERATOR_A or venueB1 not provided; skipping unauthorized venue test")
		}
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/managementRole?venue=%s", fixtures.venueB1), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?venue failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK for unauthorized venue query, got %d. Body: %s", status, string(body))
		}
		var resp struct {
			Roles []interface{} `json:"roles"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse response: %v", err)
		}
		if len(resp.Roles) != 0 {
			t.Errorf("Expected 0 roles for unauthorized venue query, got %d", len(resp.Roles))
		}

		// Verify countOnly=true returns 0
		status, body, err = client.DoRequest("GET", fmt.Sprintf("/managementRole?venue=%s&countOnly=true", fixtures.venueB1), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?venue&countOnly failed: %v", err)
		}
		if status != http.StatusOK {
			t.Fatalf("Expected 200 OK for countOnly unauthorized venue, got %d. Body: %s", status, string(body))
		}
		var countResp struct {
			Count int `json:"count"`
		}
		if err := json.Unmarshal(body, &countResp); err != nil {
			t.Fatalf("Failed to parse count response: %v", err)
		}
		if countResp.Count != 0 {
			t.Errorf("Expected count 0 for unauthorized venue, got %d", countResp.Count)
		}
	})

	t.Run("Negative: Filter roles with invalid UUID returns 400 Bad Request", func(t *testing.T) {
		status, _, err := client.DoRequest("GET", "/managementRole?policyId=invalid-uuid-format", fixtures.token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?policyId failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for malformed policy UUID, got %d", status)
		}

		status, _, err = client.DoRequest("GET", "/managementRole?venue=invalid-venue-format", fixtures.token, nil)
		if err != nil {
			t.Fatalf("GET /managementRole?venue failed: %v", err)
		}
		if status != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for malformed venue UUID, got %d", status)
		}
	})
}
