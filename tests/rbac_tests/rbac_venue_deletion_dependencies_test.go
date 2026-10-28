package rbac_tests

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"testing"
	"time"
)

/*
 * TestVenueDeletionDependencies
 *
 * VALIDATES:
 *   Section 4.2 & Section 10.1 of requirements/rbac-requirement.md:
 *   Issue 5 (P2): Venue deletion structural dependency checks in RESTAPI_venue_handler.cpp:DoDelete().
 *
 * RULES:
 *   1. Structural Deletion Pre-conditions (Section 10.1):
 *      - A Venue CANNOT be deleted if it contains dependent child venues, devices, contacts,
 *        locations, configs, or active subscriber bindings.
 *      - Any such dependency MUST reject the deletion with 400 Bad Request (StillInUse / 1005).
 *      - The error description must explicitly identify which dependencies are blocking deletion.
 *   2. Cascade Cleanup on Deletion (Section 10.2):
 *      - Once all structural blocking dependencies are cleared, deleting a Venue automatically
 *        purges all Management Roles associated with that Venue ID.
 */

func TestVenueDeletionDependencies(t *testing.T) {
	rootToken, _, adminUserId, entityId, _, policyId := getLiveTestTokensAndFixtures(t)
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	// Helper to create a standalone test venue
	createTestVenue := func() (string, func()) {
		payload := map[string]interface{}{
			"name":        fmt.Sprintf("dep-test-venue-%d", time.Now().UnixNano()),
			"description": "Venue for structural dependency test",
			"entity":      entityId,
		}
		var created struct {
			ID string `json:"id"`
		}
		status, body, err := client.DoRequest("POST", "/venue/0", rootToken, payload)
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			t.Fatalf("Failed to create test venue: %v, body=%s", err, string(body))
		}
		_ = json.Unmarshal(body, &created)
		cleanup := func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", created.ID), rootToken, nil)
		}
		return created.ID, cleanup
	}

	// =========================================================================
	// Scenario 1: Negative - Venue with attached contact is blocked with descriptive StillInUse
	// =========================================================================
	t.Run("Negative: Venue deletion blocked by attached contact with clear error", func(t *testing.T) {
		venueId, cleanupVenue := createTestVenue()
		defer cleanupVenue()

		// Create a contact
		contactPayload := map[string]interface{}{
			"name":  fmt.Sprintf("test-contact-%d", time.Now().UnixNano()),
			"title": "Site Manager",
			"type":  "technical",
			"email": fmt.Sprintf("tech-%d@example.com", time.Now().UnixNano()),
		}
		var createdContact struct {
			ID string `json:"id"`
		}
		cStatus, cBody, cErr := client.DoRequest("POST", "/contact/0", rootToken, contactPayload)
		if cErr != nil || (cStatus != http.StatusOK && cStatus != http.StatusCreated) {
			t.Fatalf("Failed to create test contact: %v, body=%s", cErr, string(cBody))
		}
		_ = json.Unmarshal(cBody, &createdContact)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/contact/%s", createdContact.ID), rootToken, nil)
		}()

		// Attach contact to venue
		putPayload := map[string]interface{}{
			"contacts": []string{createdContact.ID},
		}
		pStatus, pBody, pErr := client.DoRequest("PUT", fmt.Sprintf("/venue/%s", venueId), rootToken, putPayload)
		if pErr != nil || pStatus != http.StatusOK {
			t.Fatalf("Failed to attach contact to venue: %v, body=%s", pErr, string(pBody))
		}

		// Attempt DELETE venue
		delStatus, delBody, delErr := client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", venueId), rootToken, nil)
		if delErr != nil {
			t.Fatalf("DELETE /venue failed: %v", delErr)
		}

		if delStatus != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for venue with contact, got %d. Body: %s", delStatus, string(delBody))
		}

		bodyStr := string(delBody)
		if !strings.Contains(bodyStr, "Still in use") && !strings.Contains(bodyStr, "1005") {
			t.Errorf("Expected StillInUse (1005) in response, got: %s", bodyStr)
		}
		if !strings.Contains(bodyStr, "contact(s)") {
			t.Errorf("Expected error to explicitly mention 'contact(s)', got: %s", bodyStr)
		}

		// Detach contact so cleanup succeeds
		_, _, _ = client.DoRequest("PUT", fmt.Sprintf("/venue/%s", venueId), rootToken, map[string]interface{}{"contacts": []string{}})
	})

	// =========================================================================
	// Scenario 2: Negative - Venue with attached location is blocked with descriptive StillInUse
	// =========================================================================
	t.Run("Negative: Venue deletion blocked by attached location with clear error", func(t *testing.T) {
		venueId, cleanupVenue := createTestVenue()
		defer cleanupVenue()

		// Create a location
		locPayload := map[string]interface{}{
			"name":            fmt.Sprintf("test-location-%d", time.Now().UnixNano()),
			"addressLines":   []string{"123 Wireless Way"},
			"city":           "San Jose",
			"country":        "USA",
			"postal":         "95134",
		}
		var createdLoc struct {
			ID string `json:"id"`
		}
		lStatus, lBody, lErr := client.DoRequest("POST", "/location/0", rootToken, locPayload)
		if lErr != nil || (lStatus != http.StatusOK && lStatus != http.StatusCreated) {
			t.Fatalf("Failed to create test location: %v, body=%s", lErr, string(lBody))
		}
		_ = json.Unmarshal(lBody, &createdLoc)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/location/%s", createdLoc.ID), rootToken, nil)
		}()

		// Attach location to venue
		putPayload := map[string]interface{}{
			"location": createdLoc.ID,
		}
		pStatus, pBody, pErr := client.DoRequest("PUT", fmt.Sprintf("/venue/%s", venueId), rootToken, putPayload)
		if pErr != nil || pStatus != http.StatusOK {
			t.Fatalf("Failed to attach location to venue: %v, body=%s", pErr, string(pBody))
		}

		// Attempt DELETE venue
		delStatus, delBody, delErr := client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", venueId), rootToken, nil)
		if delErr != nil {
			t.Fatalf("DELETE /venue failed: %v", delErr)
		}

		if delStatus != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for venue with location, got %d. Body: %s", delStatus, string(delBody))
		}

		bodyStr := string(delBody)
		if !strings.Contains(bodyStr, "Still in use") && !strings.Contains(bodyStr, "1005") {
			t.Errorf("Expected StillInUse (1005) in response, got: %s", bodyStr)
		}
		if !strings.Contains(bodyStr, "attached location") {
			t.Errorf("Expected error to explicitly mention 'attached location', got: %s", bodyStr)
		}

		// Detach location so cleanup succeeds
		_, _, _ = client.DoRequest("PUT", fmt.Sprintf("/venue/%s", venueId), rootToken, map[string]interface{}{"location": ""})
	})

	// =========================================================================
	// Scenario 3: Positive - Venue with attached configuration does NOT block deletion
	// (Configurations do not block venue deletion; attached configs are detached on delete)
	// =========================================================================
	t.Run("Positive: Venue deletion permitted when configuration is attached", func(t *testing.T) {
		venueId, _ := createTestVenue()

		// Create a device configuration
		configPayload := map[string]interface{}{
			"name":          fmt.Sprintf("test-config-%d", time.Now().UnixNano()),
			"description":   "Test configuration profile",
			"configuration": []map[string]interface{}{},
		}
		var createdConfig struct {
			ID string `json:"id"`
		}
		cfgStatus, cfgBody, cfgErr := client.DoRequest("POST", "/configuration/0", rootToken, configPayload)
		if cfgErr != nil || (cfgStatus != http.StatusOK && cfgStatus != http.StatusCreated) {
			t.Fatalf("Failed to create configuration: %v, body=%s", cfgErr, string(cfgBody))
		}
		_ = json.Unmarshal(cfgBody, &createdConfig)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/configuration/%s", createdConfig.ID), rootToken, nil)
		}()

		// Attach config to venue
		putPayload := map[string]interface{}{
			"deviceConfiguration": []string{createdConfig.ID},
		}
		pStatus, pBody, pErr := client.DoRequest("PUT", fmt.Sprintf("/venue/%s", venueId), rootToken, putPayload)
		if pErr != nil || pStatus != http.StatusOK {
			t.Fatalf("Failed to attach configuration to venue: %v, body=%s", pErr, string(pBody))
		}

		// Attempt DELETE venue (should succeed because configs do not block venue deletion)
		delStatus, delBody, delErr := client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", venueId), rootToken, nil)
		if delErr != nil {
			t.Fatalf("DELETE /venue failed: %v", delErr)
		}

		if delStatus != http.StatusOK {
			t.Errorf("Expected 200 OK for deleting venue with attached configuration, got %d. Body: %s", delStatus, string(delBody))
		}
	})

	// =========================================================================
	// Scenario 4: Negative - Venue with child venue is blocked with descriptive StillInUse
	// =========================================================================
	t.Run("Negative: Venue deletion blocked by child venue with clear error", func(t *testing.T) {
		parentVenueId, cleanupParent := createTestVenue()
		defer cleanupParent()

		childPayload := map[string]interface{}{
			"name":        fmt.Sprintf("child-venue-%d", time.Now().UnixNano()),
			"description": "Child venue",
			"parent":      parentVenueId,
		}
		var createdChild struct {
			ID string `json:"id"`
		}
		chStatus, chBody, chErr := client.DoRequest("POST", "/venue/0", rootToken, childPayload)
		if chErr != nil || (chStatus != http.StatusOK && chStatus != http.StatusCreated) {
			t.Fatalf("Failed to create child venue: %v, body=%s", chErr, string(chBody))
		}
		_ = json.Unmarshal(chBody, &createdChild)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", createdChild.ID), rootToken, nil)
		}()

		// Attempt DELETE parent venue
		delStatus, delBody, delErr := client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", parentVenueId), rootToken, nil)
		if delErr != nil {
			t.Fatalf("DELETE parent venue failed: %v", delErr)
		}

		if delStatus != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for venue with child venue, got %d. Body: %s", delStatus, string(delBody))
		}

		bodyStr := string(delBody)
		if !strings.Contains(bodyStr, "child venue(s)") {
			t.Errorf("Expected error to explicitly mention 'child venue(s)', got: %s", bodyStr)
		}
	})

	// =========================================================================
	// Scenario 5: Positive - Management Roles do NOT block deletion & are cascadingly purged
	// Section 10.1 & 10.2: Management Roles are automatically purged when scope is deleted
	// =========================================================================
	t.Run("Positive: Management roles do not block deletion and are cascadingly purged", func(t *testing.T) {
		venueId, _ := createTestVenue()

		// Assign a Management Role to this venue
		rolePayload := map[string]interface{}{
			"name":             fmt.Sprintf("venue-scoped-role-%d", time.Now().UnixNano()),
			"description":      "Role on venue to verify cascade deletion",
			"entity":           entityId,
			"venue":            venueId,
			"managementPolicy": policyId,
			"users":            []string{adminUserId},
		}
		var createdRole struct {
			ID string `json:"id"`
		}
		rStatus, rBody, rErr := client.DoRequest("POST", "/managementRole/0", rootToken, rolePayload)
		if rErr != nil || (rStatus != http.StatusOK && rStatus != http.StatusCreated) {
			t.Fatalf("Failed to assign role to venue: %v, body=%s", rErr, string(rBody))
		}
		_ = json.Unmarshal(rBody, &createdRole)

		// Delete the venue (should succeed because roles do not block deletion)
		delStatus, delBody, delErr := client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", venueId), rootToken, nil)
		if delErr != nil {
			t.Fatalf("DELETE venue failed: %v", delErr)
		}

		if delStatus != http.StatusOK {
			t.Fatalf("Expected 200 OK for deleting venue with only roles attached, got %d. Body: %s", delStatus, string(delBody))
		}

		// Verify the role was cascadingly deleted
		getRoleStatus, _, _ := client.DoRequest("GET", fmt.Sprintf("/managementRole/%s", createdRole.ID), rootToken, nil)
		if getRoleStatus != http.StatusNotFound {
			t.Errorf("Expected 404 Not Found for cascadingly deleted role, got %d", getRoleStatus)
		}
	})
}

