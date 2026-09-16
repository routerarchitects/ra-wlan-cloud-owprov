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
					"actions":   []string{"*"},
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
}
