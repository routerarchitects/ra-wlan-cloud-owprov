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
 * TestInventoryDualScopeTransfer
 *
 * VALIDATES:
 *   Section 8.1.4, Section 8.2 & Section 9.3 of requirements/rbac-requirement.md:
 *   Issue 3 (P1): Dual-Scope Inventory Transfer Authorization in RESTAPI_inventory_handler.cpp:DoPut()
 *
 * RULES:
 *   1. Cross-Scope Movement Model: Device movement is modeled as an update across two scopes.
 *   2. Dual-Scope inventory:UPDATE Requirement:
 *      - Source Scope: Actor MUST hold effective inventory:UPDATE on originating scope.
 *      - Destination Scope: Actor MUST ALSO hold effective inventory:UPDATE on target scope.
 *   3. Insufficiency of Destination inventory:CREATE:
 *      - Holding only inventory:CREATE on destination scope is insufficient; actor must hold UPDATE.
 *   4. Mutual Exclusivity (NotBoth) on Update:
 *      - Cannot specify both entity and venue simultaneously (400 Bad Request).
 *      - Moving to venue auto-detaches from prior direct entity, and vice versa.
 *   5. Unassigned Scope:
 *      - Detaching device to unassigned pool (empty venue and entity) is restricted to ROOT.
 *   6. ROOT Parity:
 *      - ROOT user bypasses creator/scope checks and can move devices unconditionally.
 */

type inventoryTagResponse struct {
	ID           string `json:"id"`
	SerialNumber string `json:"serialNumber"`
	Name         string `json:"name"`
	Entity       string `json:"entity"`
	Venue        string `json:"venue"`
	DeviceType   string `json:"deviceType"`
}

// Helper to ensure an inventory device exists for testing
func getOrCreateTestDevice(t *testing.T, client *TestClient, rootToken string, entityId string, venueId string) (serialNumber string, cleanup func()) {
	serialNumber = fmt.Sprintf("02%010x", time.Now().UnixNano()&0xFFFFFFFFFF)
	payload := map[string]interface{}{
		"serialNumber": serialNumber,
		"name":         "test-ap-" + serialNumber,
		"deviceType":   "AP",
		"entity":       entityId,
		"venue":        venueId,
	}

	status, body, err := client.DoRequest("POST", fmt.Sprintf("/inventory/%s", serialNumber), rootToken, payload)
	if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
		t.Fatalf("Failed to create test inventory device %s: status=%d, err=%v, body=%s", serialNumber, status, err, string(body))
	}

	cleanup = func() {
		_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/inventory/%s", serialNumber), rootToken, nil)
	}
	return serialNumber, cleanup
}

// Helper to discover or create a management policy with specified permissions
func getOrCreatePolicyWithAccess(t *testing.T, client *TestClient, rootToken string, policyName string, resource string, access []string) (string, func()) {
	// Look for existing policy in DB first
	accessPattern := "%" + access[0] + "%"
	cmd := exec.Command("docker", "exec", "openwifi-postgresql-1", "psql", "-U", "owprov", "-d", "owprov", "-t", "-A", "-c",
		fmt.Sprintf("SELECT id FROM policies WHERE entries LIKE '%%%s%%' AND entries LIKE '%s' LIMIT 1;", resource, accessPattern))
	out, err := cmd.Output()
	if err == nil && len(strings.TrimSpace(string(out))) > 10 {
		return strings.TrimSpace(string(out)), func() {}
	}

	policyPayload := map[string]interface{}{
		"name":        policyName,
		"description": "Auto-created policy for dual-scope test",
		"entries": []map[string]interface{}{
			{
				"resources": []string{resource},
				"access":    access,
			},
		},
	}

	var createdPolicy struct {
		ID string `json:"id"`
	}
	status, body, err := client.DoRequest("POST", "/managementPolicy/0", rootToken, policyPayload)
	if err == nil && (status == http.StatusOK || status == http.StatusCreated) {
		if json.Unmarshal(body, &createdPolicy) == nil && createdPolicy.ID != "" {
			return createdPolicy.ID, func() {
				_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", createdPolicy.ID), rootToken, nil)
			}
		}
	}

	// Fallback policy ID
	return "c5d4ea77-26e2-4c4f-b403-5fa906c7635d", func() {}
}

func TestInventoryDualScopeTransfer(t *testing.T) {
	rootToken, adminToken, adminUserId, entityId, venueAId, _ := getLiveTestTokensAndFixtures(t)
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	// Ensure we have a second distinct venue (Venue B) under the entity for transfer testing
	venueBPayload := map[string]interface{}{
		"name":        fmt.Sprintf("test-venue-b-%d", time.Now().UnixNano()),
		"description": "Destination Venue B for transfer test",
		"entity":      entityId,
	}
	var createdVenueB struct {
		ID string `json:"id"`
	}
	status, body, err := client.DoRequest("POST", "/venue/0", rootToken, venueBPayload)
	if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
		t.Fatalf("Failed to create Venue B for test: status=%d, body=%s", status, string(body))
	}
	_ = json.Unmarshal(body, &createdVenueB)
	venueBId := createdVenueB.ID
	defer func() {
		_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", venueBId), rootToken, nil)
	}()

	// Discover/create policies for inventory permissions
	policyUpdateId, cleanupPolUpdate := getOrCreatePolicyWithAccess(t, client, rootToken, "test-policy-inv-update", "inventory", []string{"UPDATE"})
	defer cleanupPolUpdate()

	policyReadId, cleanupPolRead := getOrCreatePolicyWithAccess(t, client, rootToken, "test-policy-inv-read", "inventory", []string{"READ"})
	defer cleanupPolRead()

	policyCreateId, cleanupPolCreate := getOrCreatePolicyWithAccess(t, client, rootToken, "test-policy-inv-create", "inventory", []string{"CREATE"})
	defer cleanupPolCreate()

	// Helper to create a management role for the admin
	createRoleForAdmin := func(entity string, venue string, policyId string) (string, func()) {
		rolePayload := map[string]interface{}{
			"name":             fmt.Sprintf("inv-dual-role-%d", time.Now().UnixNano()),
			"description":      "Role for inventory transfer test",
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
	// Scenario 1: Negative - Admin has inventory:UPDATE on Venue A, but NO role on Venue B
	// Attempting PUT /inventory/{id} to move device from Venue A to Venue B must be DENIED (403)
	// =========================================================================
	t.Run("Negative: Transfer rejected when actor lacks role on Destination Venue", func(t *testing.T) {
		deviceSerial, cleanupDev := getOrCreateTestDevice(t, client, rootToken, "", venueAId)
		defer cleanupDev()

		// Grant admin inventory:UPDATE only on Venue A
		roleAId, cleanupRoleA := createRoleForAdmin(entityId, venueAId, policyUpdateId)
		defer cleanupRoleA()

		// Admin attempts to transfer device to Venue B
		putPayload := map[string]interface{}{
			"venue": venueBId,
		}
		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", deviceSerial), adminToken, putPayload)
		if putErr != nil {
			t.Fatalf("PUT request failed: %v", putErr)
		}

		if putStatus != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden when transferring to unauthorized venue, got %d. Body: %s", putStatus, string(putBody))
		}

		// Verify device remains in Venue A
		getStatus, getBody, getErr := client.DoRequest("GET", fmt.Sprintf("/inventory/%s", deviceSerial), rootToken, nil)
		if getErr == nil && getStatus == http.StatusOK {
			var dev inventoryTagResponse
			_ = json.Unmarshal(getBody, &dev)
			if dev.Venue != venueAId {
				t.Errorf("Device was moved despite unauthorized destination! Current venue: %s, expected: %s", dev.Venue, venueAId)
			}
		}
		_ = roleAId
	})

	// =========================================================================
	// Scenario 2: Negative - Admin has inventory:UPDATE on Venue A, but only inventory:READ on Venue B
	// Section 8.1.4: Actor must hold inventory:UPDATE on destination; READ is insufficient.
	// =========================================================================
	t.Run("Negative: Transfer rejected when Destination Venue has only inventory:READ", func(t *testing.T) {
		deviceSerial, cleanupDev := getOrCreateTestDevice(t, client, rootToken, "", venueAId)
		defer cleanupDev()

		// Grant inventory:UPDATE on Venue A, but only inventory:READ on Venue B
		_, cleanupRoleA := createRoleForAdmin(entityId, venueAId, policyUpdateId)
		defer cleanupRoleA()
		_, cleanupRoleB := createRoleForAdmin(entityId, venueBId, policyReadId)
		defer cleanupRoleB()

		putPayload := map[string]interface{}{
			"venue": venueBId,
		}
		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", deviceSerial), adminToken, putPayload)
		if putErr != nil {
			t.Fatalf("PUT request failed: %v", putErr)
		}

		if putStatus != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden when destination only has READ, got %d. Body: %s", putStatus, string(putBody))
		}
	})

	// =========================================================================
	// Scenario 3: Negative - Admin has inventory:UPDATE on Venue A, but only inventory:CREATE on Venue B
	// Section 8.1.4 Item 3: Insufficiency of Destination inventory:CREATE
	// =========================================================================
	t.Run("Negative: Transfer rejected when Destination Venue has only inventory:CREATE", func(t *testing.T) {
		deviceSerial, cleanupDev := getOrCreateTestDevice(t, client, rootToken, "", venueAId)
		defer cleanupDev()

		_, cleanupRoleA := createRoleForAdmin(entityId, venueAId, policyUpdateId)
		defer cleanupRoleA()
		_, cleanupRoleB := createRoleForAdmin(entityId, venueBId, policyCreateId)
		defer cleanupRoleB()

		putPayload := map[string]interface{}{
			"venue": venueBId,
		}
		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", deviceSerial), adminToken, putPayload)
		if putErr != nil {
			t.Fatalf("PUT request failed: %v", putErr)
		}

		if putStatus != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden when destination only has CREATE, got %d. Body: %s", putStatus, string(putBody))
		}
	})

	// =========================================================================
	// Scenario 4: Positive - Admin holds effective inventory:UPDATE on BOTH Venue A and Venue B
	// Cross-scope transfer succeeds (200 OK) and membership is updated.
	// =========================================================================
	t.Run("Positive: Transfer succeeds when actor holds inventory:UPDATE on both scopes", func(t *testing.T) {
		deviceSerial, cleanupDev := getOrCreateTestDevice(t, client, rootToken, "", venueAId)
		defer cleanupDev()

		_, cleanupRoleA := createRoleForAdmin(entityId, venueAId, policyUpdateId)
		defer cleanupRoleA()
		_, cleanupRoleB := createRoleForAdmin(entityId, venueBId, policyUpdateId)
		defer cleanupRoleB()

		putPayload := map[string]interface{}{
			"venue": venueBId,
		}
		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", deviceSerial), adminToken, putPayload)
		if putErr != nil {
			t.Fatalf("PUT request failed: %v", putErr)
		}

		if putStatus != http.StatusOK {
			t.Fatalf("Expected 200 OK for authorized dual-scope transfer, got %d. Body: %s", putStatus, string(putBody))
		}

		// Verify device venue is now Venue B
		var updatedDev inventoryTagResponse
		if err := json.Unmarshal(putBody, &updatedDev); err != nil || updatedDev.Venue != venueBId {
			t.Errorf("Expected device.venue == %s, got: %s", venueBId, updatedDev.Venue)
		}
	})

	// =========================================================================
	// Scenario 5: Edge Case - Mutual Exclusivity (NotBoth) on PUT
	// Supplying both entity and venue simultaneously in PUT payload is rejected (400 Bad Request)
	// =========================================================================
	t.Run("Edge: Supplying both entity and venue simultaneously is rejected with NotBoth", func(t *testing.T) {
		deviceSerial, cleanupDev := getOrCreateTestDevice(t, client, rootToken, entityId, "")
		defer cleanupDev()

		putPayload := map[string]interface{}{
			"entity": entityId,
			"venue":  venueAId,
		}
		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", deviceSerial), rootToken, putPayload)
		if putErr != nil {
			t.Fatalf("PUT request failed: %v", putErr)
		}

		if putStatus != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for NotBoth violation on PUT, got %d. Body: %s", putStatus, string(putBody))
		}
	})

	// =========================================================================
	// Scenario 6: Edge Case - Auto-Detachment Preserving Single-Scope Invariant
	// Moving a device from direct Entity binding to a Venue automatically detaches entity.
	// =========================================================================
	t.Run("Edge: Moving to Venue automatically clears direct Entity binding", func(t *testing.T) {
		deviceSerial, cleanupDev := getOrCreateTestDevice(t, client, rootToken, entityId, "")
		defer cleanupDev()

		// Move to Venue A without mentioning entity
		putPayload := map[string]interface{}{
			"venue": venueAId,
		}
		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", deviceSerial), rootToken, putPayload)
		if putErr != nil {
			t.Fatalf("PUT request failed: %v", putErr)
		}

		if putStatus != http.StatusOK {
			t.Fatalf("Expected 200 OK for transfer to venue, got %d. Body: %s", putStatus, string(putBody))
		}

		// Verify in DB that entity is empty and venue is set
		getStatus, getBody, _ := client.DoRequest("GET", fmt.Sprintf("/inventory/%s", deviceSerial), rootToken, nil)
		if getStatus == http.StatusOK {
			var dev inventoryTagResponse
			_ = json.Unmarshal(getBody, &dev)
			if dev.Venue != venueAId || dev.Entity != "" {
				t.Errorf("Single-scope invariant violated! Venue: '%s' (expected '%s'), Entity: '%s' (expected '')",
					dev.Venue, venueAId, dev.Entity)
			}
		}
	})

	// =========================================================================
	// Scenario 7: Edge Case - Non-existent Destination Scope
	// Transferring to a non-existent venue UUID must return 400 Bad Request
	// =========================================================================
	t.Run("Edge: Transfer to non-existent venue returns 400 Bad Request", func(t *testing.T) {
		deviceSerial, cleanupDev := getOrCreateTestDevice(t, client, rootToken, "", venueAId)
		defer cleanupDev()

		putPayload := map[string]interface{}{
			"venue": "ffffffff-ffff-ffff-ffff-ffffffffffff",
		}
		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", deviceSerial), rootToken, putPayload)
		if putErr != nil {
			t.Fatalf("PUT request failed: %v", putErr)
		}

		if putStatus != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for non-existent venue, got %d. Body: %s", putStatus, string(putBody))
		}
	})

	// =========================================================================
	// Scenario 8: Edge Case - Detaching to Unassigned Pool by Non-Root User
	// Non-root actor attempting to clear both venue and entity is rejected (403 Forbidden)
	// =========================================================================
	t.Run("Edge: Detaching device to unassigned pool by non-root is rejected", func(t *testing.T) {
		deviceSerial, cleanupDev := getOrCreateTestDevice(t, client, rootToken, "", venueAId)
		defer cleanupDev()

		_, cleanupRoleA := createRoleForAdmin(entityId, venueAId, policyUpdateId)
		defer cleanupRoleA()

		putPayload := map[string]interface{}{
			"venue":  "",
			"entity": "",
		}
		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", deviceSerial), adminToken, putPayload)
		if putErr != nil {
			t.Fatalf("PUT request failed: %v", putErr)
		}

		if putStatus != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden when non-root attempts to unassign device, got %d. Body: %s", putStatus, string(putBody))
		}
	})

	// =========================================================================
	// Scenario 9: Positive Control - ROOT User Transfer Parity
	// ROOT can transfer devices across any scopes unconditionally.
	// =========================================================================
	t.Run("Positive: ROOT user can transfer devices unconditionally", func(t *testing.T) {
		deviceSerial, cleanupDev := getOrCreateTestDevice(t, client, rootToken, "", venueAId)
		defer cleanupDev()

		putPayload := map[string]interface{}{
			"venue": venueBId,
		}
		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", deviceSerial), rootToken, putPayload)
		if putErr != nil {
			t.Fatalf("ROOT PUT request failed: %v", putErr)
		}

		if putStatus != http.StatusOK {
			t.Errorf("Expected 200 OK for ROOT device transfer, got %d. Body: %s", putStatus, string(putBody))
		}
	})

	// =========================================================================
	// Scenario 10: Negative - Transfer of Subscriber-Assigned Device Prohibited for Admin
	// Validates business invariant: subscriber-assigned inventory cannot be transferred
	// even when the Admin holds dual-scope authorization on both source and target.
	// =========================================================================
	t.Run("Negative: Transfer of subscriber-assigned device prohibited for Admin (400 Bad Request)", func(t *testing.T) {
		subId := "11111111-2222-3333-4444-555555555555"
		subSerial := fmt.Sprintf("02%010x", time.Now().UnixNano()&0xFFFFFFFFFF)
		createPayload := map[string]interface{}{
			"serialNumber": subSerial,
			"name":         "test-sub-ap-" + subSerial,
			"deviceType":   "AP",
			"venue":        venueAId,
			"subscriber":   subId,
		}
		cStatus, cBody, cErr := client.DoRequest("POST", fmt.Sprintf("/inventory/%s", subSerial), rootToken, createPayload)
		if cErr != nil || (cStatus != http.StatusOK && cStatus != http.StatusCreated) {
			t.Fatalf("Failed to create subscriber inventory device: status=%d, err=%v, body=%s", cStatus, cErr, string(cBody))
		}
		defer func() {
			_, _, _ = client.DoRequest("PUT", fmt.Sprintf("/inventory/%s?removeSubscriber=%s", subSerial, subId), rootToken, nil)
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/inventory/%s", subSerial), rootToken, nil)
		}()

		// Admin holds UPDATE on both Venue A and Venue B
		_, cleanupRoleA := createRoleForAdmin(entityId, venueAId, policyUpdateId)
		defer cleanupRoleA()
		_, cleanupRoleB := createRoleForAdmin(entityId, venueBId, policyUpdateId)
		defer cleanupRoleB()

		putPayload := map[string]interface{}{
			"venue": venueBId,
		}
		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", subSerial), adminToken, putPayload)
		if putErr != nil {
			t.Fatalf("PUT request failed: %v", putErr)
		}

		if putStatus != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for Admin transferring subscriber device, got %d. Body: %s", putStatus, string(putBody))
		}
		if !strings.Contains(string(putBody), "Subscriber-assigned inventory cannot be transferred") &&
			!strings.Contains(string(putBody), "1005") {
			t.Logf("Notice: Response description: %s", string(putBody))
		}
	})

	// =========================================================================
	// Scenario 11: Negative - Transfer of Subscriber-Assigned Device Prohibited for ROOT
	// Validates business invariant: subscriber-assigned inventory cannot be transferred
	// even by ROOT user (must remove subscriber first).
	// =========================================================================
	t.Run("Negative: Transfer of subscriber-assigned device prohibited for ROOT (400 Bad Request)", func(t *testing.T) {
		subId := "22222222-3333-4444-5555-666666666666"
		subSerial := fmt.Sprintf("02%010x", time.Now().UnixNano()&0xFFFFFFFFFF)
		createPayload := map[string]interface{}{
			"serialNumber": subSerial,
			"name":         "test-sub-ap-root-" + subSerial,
			"deviceType":   "AP",
			"venue":        venueAId,
			"subscriber":   subId,
		}
		cStatus, cBody, cErr := client.DoRequest("POST", fmt.Sprintf("/inventory/%s", subSerial), rootToken, createPayload)
		if cErr != nil || (cStatus != http.StatusOK && cStatus != http.StatusCreated) {
			t.Fatalf("Failed to create subscriber inventory device: status=%d, err=%v, body=%s", cStatus, cErr, string(cBody))
		}
		defer func() {
			_, _, _ = client.DoRequest("PUT", fmt.Sprintf("/inventory/%s?removeSubscriber=%s", subSerial, subId), rootToken, nil)
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/inventory/%s", subSerial), rootToken, nil)
		}()

		putPayload := map[string]interface{}{
			"venue": venueBId,
		}
		putStatus, putBody, putErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", subSerial), rootToken, putPayload)
		if putErr != nil {
			t.Fatalf("PUT request failed: %v", putErr)
		}

		if putStatus != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request for ROOT transferring subscriber device, got %d. Body: %s", putStatus, string(putBody))
		}
	})

	// =========================================================================
	// Scenario 12: Positive - Transfer Permitted After Subscriber Removed
	// Once subscriber is removed via ?removeSubscriber=, device can be transferred.
	// =========================================================================
	t.Run("Positive: Transfer permitted after subscriber assignment removed", func(t *testing.T) {
		subId := "33333333-4444-5555-6666-777777777777"
		subSerial := fmt.Sprintf("02%010x", time.Now().UnixNano()&0xFFFFFFFFFF)
		createPayload := map[string]interface{}{
			"serialNumber": subSerial,
			"name":         "test-sub-ap-flow-" + subSerial,
			"deviceType":   "AP",
			"venue":        venueAId,
			"subscriber":   subId,
		}
		cStatus, cBody, cErr := client.DoRequest("POST", fmt.Sprintf("/inventory/%s", subSerial), rootToken, createPayload)
		if cErr != nil || (cStatus != http.StatusOK && cStatus != http.StatusCreated) {
			t.Fatalf("Failed to create subscriber inventory device: status=%d, err=%v, body=%s", cStatus, cErr, string(cBody))
		}
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/inventory/%s", subSerial), rootToken, nil)
		}()

		// 1. Attempt transfer while subscriber is attached -> rejected
		putPayload := map[string]interface{}{
			"venue": venueBId,
		}
		bStatus, _, _ := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", subSerial), rootToken, putPayload)
		if bStatus != http.StatusBadRequest {
			t.Errorf("Expected 400 Bad Request while subscriber attached, got %d", bStatus)
		}

		// 2. Remove subscriber via de-provisioning parameter
		remStatus, _, remErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s?removeSubscriber=%s", subSerial, subId), rootToken, nil)
		if remErr != nil || remStatus != http.StatusOK {
			t.Fatalf("Failed to remove subscriber: status=%d, err=%v", remStatus, remErr)
		}

		// 3. Re-attempt transfer now that subscriber is removed -> succeeds
		tStatus, tBody, tErr := client.DoRequest("PUT", fmt.Sprintf("/inventory/%s", subSerial), rootToken, putPayload)
		if tErr != nil {
			t.Fatalf("PUT transfer request failed: %v", tErr)
		}
		if tStatus != http.StatusOK {
			t.Errorf("Expected 200 OK for transferring device after subscriber removed, got %d. Body: %s", tStatus, string(tBody))
		}
	})
}

