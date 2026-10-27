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

	// 5. Cross-Tenant Multi-ID Selection: Admin querying authorized + unauthorized variables skips unauthorized
	t.Run("Negative: Multi-ID selection skips unauthorized out-of-scope variable block", func(t *testing.T) {
		// Create a separate variable block in Root Entity or another tenant where Admin has no role
		otherEntityId := "00000000-0000-0000-0000-000000000000" // Root UUID
		otherVarPayload := map[string]interface{}{
			"name":        fmt.Sprintf("other-var-block-%d", time.Now().UnixNano()),
			"description": "Variable block for cross-tenant negative test",
			"entity":      otherEntityId,
			"variables": []map[string]interface{}{
				{
					"prefix":      "cfg",
					"name":        "SECRET_KEY",
					"type":        "string",
					"value":       "supersecret",
					"description": "Cross-tenant secret",
				},
			},
		}

		var otherVar struct {
			ID string `json:"id"`
		}
		oStatus, oBody, oErr := client.DoRequest("POST", "/variable/0", rootToken, otherVarPayload)
		if oErr != nil || (oStatus != http.StatusOK && oStatus != http.StatusCreated) {
			t.Fatalf("Failed to create out-of-scope variable block: %v, body=%s", oErr, string(oBody))
		}
		_ = json.Unmarshal(oBody, &otherVar)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/variable/%s", otherVar.ID), rootToken, nil)
		}()

		// Query ?select=<authorized_var_id>,<unauthorized_var_id>
		multiSelectURL := fmt.Sprintf("/variable?select=%s,%s", createdVar.ID, otherVar.ID)
		mStatus, mBody, mErr := client.DoRequest("GET", multiSelectURL, adminToken, nil)
		if mErr != nil {
			t.Fatalf("GET multi-select failed: %v", mErr)
		}
		if mStatus != http.StatusOK {
			t.Fatalf("Expected 200 OK from multi-select, got %d. Body: %s", mStatus, string(mBody))
		}

		var multiResp struct {
			VariableBlocks []struct {
				ID string `json:"id"`
			} `json:"variableBlocks"`
		}
		if err := json.Unmarshal(mBody, &multiResp); err != nil {
			t.Fatalf("Failed to unmarshal response: %v", err)
		}

		// Ensure the authorized block is present, and the out-of-scope block is skipped
		hasAuthorized := false
		hasUnauthorized := false
		for _, b := range multiResp.VariableBlocks {
			if b.ID == createdVar.ID {
				hasAuthorized = true
			}
			if b.ID == otherVar.ID {
				hasUnauthorized = true
			}
		}

		if !hasAuthorized {
			t.Errorf("Expected authorized variable block %s to be returned in list", createdVar.ID)
		}
		if hasUnauthorized {
			t.Errorf("Security Violation: Out-of-scope variable block %s was returned to unauthorized Admin!", otherVar.ID)
		}
	})

	// 6. Cross-Tenant Unfiltered Listing: Admin never receives out-of-scope variable blocks
	t.Run("Negative: Unfiltered listing excludes out-of-scope variable blocks", func(t *testing.T) {
		otherEntityId := "00000000-0000-0000-0000-000000000000"
		otherVarPayload := map[string]interface{}{
			"name":        fmt.Sprintf("secret-tenant-b-%d", time.Now().UnixNano()),
			"description": "Tenant B variable block",
			"entity":      otherEntityId,
			"variables": []map[string]interface{}{
				{
					"prefix":      "cfg",
					"name":        "TENANT_B_SECRET",
					"type":        "string",
					"value":       "tenantBpassword",
					"description": "Secret variable",
				},
			},
		}

		var otherVar struct {
			ID string `json:"id"`
		}
		oStatus, oBody, oErr := client.DoRequest("POST", "/variable/0", rootToken, otherVarPayload)
		if oErr != nil || (oStatus != http.StatusOK && oStatus != http.StatusCreated) {
			t.Fatalf("Failed to create out-of-scope variable block: %v, body=%s", oErr, string(oBody))
		}
		_ = json.Unmarshal(oBody, &otherVar)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/variable/%s", otherVar.ID), rootToken, nil)
		}()

		// Admin executes unfiltered GET /api/v1/variable
		gStatus, gBody, gErr := client.DoRequest("GET", "/variable", adminToken, nil)
		if gErr != nil {
			t.Fatalf("GET /variable failed: %v", gErr)
		}
		if gStatus != http.StatusOK {
			t.Fatalf("Expected 200 OK for Admin GET /variable, got %d. Body: %s", gStatus, string(gBody))
		}

		var listResp struct {
			VariableBlocks []struct {
				ID string `json:"id"`
			} `json:"variableBlocks"`
		}
		if err := json.Unmarshal(gBody, &listResp); err != nil {
			t.Fatalf("Failed to unmarshal response: %v", err)
		}

		for _, b := range listResp.VariableBlocks {
			if b.ID == otherVar.ID {
				t.Fatalf("Security Violation: Out-of-scope variable block %s leaked in unfiltered listing to Admin!", otherVar.ID)
			}
		}
	})

	// 7. Cross-Tenant Variable Transfer: Moving variable into unauthorized destination scope is blocked (403)
	t.Run("Negative: Transferring variable block to unauthorized entity scope is blocked (403)", func(t *testing.T) {
		otherEntityId := "00000000-0000-0000-0000-000000000000" // Root UUID where admin has no update role

		// Admin attempts to PUT /variable/<createdVar.ID> with "entity": otherEntityId
		transferPayload := map[string]interface{}{
			"entity": otherEntityId,
		}

		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/variable/%s", createdVar.ID), adminToken, transferPayload)
		if putErr != nil {
			t.Fatalf("PUT /variable transfer failed with network error: %v", putErr)
		}

		if putStatus != http.StatusForbidden && putStatus != http.StatusUnauthorized {
			t.Errorf("Security Violation: Expected 403 Forbidden when transferring variable to unauthorized entity, got %d. Body: %s", putStatus, string(putBody))
		}
	})

	// 8. Single-Scope Invariant: Supplying both entity and venue is rejected (400 NotBoth)
	t.Run("Edge: Supplying both entity and venue on variable update is rejected with 400 NotBoth", func(t *testing.T) {
		invalidPayload := map[string]interface{}{
			"entity": entityId,
			"venue":  "11111111-2222-3333-4444-555555555555",
		}

		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/variable/%s", createdVar.ID), adminToken, invalidPayload)
		if putErr != nil {
			t.Fatalf("PUT /variable failed with network error: %v", putErr)
		}

		if putStatus != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request (NotBoth) when supplying both entity and venue, got %d. Body: %s", putStatus, string(putBody))
		}
	})
}

