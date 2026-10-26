package rbac_tests

import (
	"encoding/json"
	"fmt"
	"net/http"
	"testing"
	"time"
)

/*
 * TestVariableAccessAuthorization
 *
 * VALIDATES:
 *   1. Variable blocks are configuration variables and map to "configuration" permission in Management Policy.
 *   2. Endpoints:
 *      - GET /api/v1/variable?select={uuid}
 *      - GET /api/v1/variable/{uuid}
 *      - GET /api/v1/variable
 *   3. Scope Resolution via query parameter (?select={uuid}):
 *      - TargetEntity and TargetVenue are resolved from the VariableBlock record in DB.
 *      - Admin user holding configuration authority on that scope is allowed (200 OK).
 */

func TestVariableAccessAuthorization(t *testing.T) {
	rootToken, adminToken, _, entityId, _, _ := getLiveTestTokensAndFixtures(t)
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	// 1. Create a VariableBlock as root within the Admin's entity scope
	varBlockPayload := map[string]interface{}{
		"name":        fmt.Sprintf("test-var-block-%d", time.Now().UnixNano()),
		"description": "Variable block for RBAC authorization test",
		"entity":      entityId,
		"variables": []map[string]interface{}{
			{
				"prefix":      "cfg",
				"name":        "SSID_NAME",
				"type":        "string",
				"value":       "Corporate-WiFi",
				"description": "Default SSID",
			},
		},
	}

	var createdVar struct {
		ID string `json:"id"`
	}
	status, body, err := client.DoRequest("POST", "/variable/0", rootToken, varBlockPayload)
	if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
		t.Fatalf("Failed to create test variable block: %v, body=%s", err, string(body))
	}
	_ = json.Unmarshal(body, &createdVar)
	defer func() {
		_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/variable/%s", createdVar.ID), rootToken, nil)
	}()

	// 2. Test Admin GET /api/v1/variable?select={uuid}
	t.Run("Positive: Admin can read variable block via ?select={uuid}", func(t *testing.T) {
		getURL := fmt.Sprintf("/variable?withExtendedInfo=true&select=%s", createdVar.ID)
		gStatus, gBody, gErr := client.DoRequest("GET", getURL, adminToken, nil)
		if gErr != nil {
			t.Fatalf("GET /variable?select=... failed: %v", gErr)
		}
		if gStatus != http.StatusOK {
			t.Errorf("Expected 200 OK for Admin accessing variable block with ?select=, got %d. Body: %s", gStatus, string(gBody))
		}
	})

	// 3. Test Admin GET /api/v1/variable/{uuid}
	t.Run("Positive: Admin can read variable block directly via /{uuid}", func(t *testing.T) {
		getURL := fmt.Sprintf("/variable/%s", createdVar.ID)
		gStatus, gBody, gErr := client.DoRequest("GET", getURL, adminToken, nil)
		if gErr != nil {
			t.Fatalf("GET /variable/{uuid} failed: %v", gErr)
		}
		if gStatus != http.StatusOK {
			t.Errorf("Expected 200 OK for Admin accessing variable block directly, got %d. Body: %s", gStatus, string(gBody))
		}
	})

	// 4. Test Admin listing variables /api/v1/variable
	t.Run("Positive: Admin can list variables", func(t *testing.T) {
		getURL := "/variable"
		gStatus, gBody, gErr := client.DoRequest("GET", getURL, adminToken, nil)
		if gErr != nil {
			t.Fatalf("GET /variable failed: %v", gErr)
		}
		if gStatus != http.StatusOK {
			t.Errorf("Expected 200 OK for Admin listing variables, got %d. Body: %s", gStatus, string(gBody))
		}
	})
}

