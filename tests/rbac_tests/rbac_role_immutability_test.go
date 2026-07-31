package rbac_tests

import (
	"fmt"
	"net/http"
	"testing"
)

// ----------------------------------------------------------------------------
// 3. MANAGEMENT ROLE IMMUTABILITY TESTS (PUT /api/v1/managementRole/{id})
// ----------------------------------------------------------------------------

func TestManagementRoleImmutability(t *testing.T) {
	client := &TestClient{BaseURL: "https://openwifi.wlan.local:16005/api/v1", Client: http.DefaultClient}

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
