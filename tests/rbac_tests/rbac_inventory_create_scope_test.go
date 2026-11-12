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
 * TestInventoryCreateScopeAuthorization
 *
 * VALIDATES F1 FIX:
 *   1. ResolveTargetContext uses ParsedBody_ entity/venue on write requests and
 *      rejects conflicting scope between URL query parameters and request body
 *      (including cross-field query entity vs body venue and vice versa) with
 *      a descriptive error message.
 *   2. RESTAPI_inventory_handler::DoPost independently enforces destination
 *      IsScopeAuthorized(...) checks and rejects unassigned non-root creation,
 *      mirroring DoPut().
 */
func TestInventoryCreateScopeAuthorization(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken, adminToken, adminUserId, _, _, _ := getLiveTestTokensAndFixtures(t)

	if adminToken == "" || adminUserId == "" {
		t.Skip("Skipping test: no non-root admin user/token available")
	}

	ts := time.Now().UnixNano()
	macCounter := uint64(ts & 0xFFFFFF)
	nextSerial := func() string {
		macCounter++
		return fmt.Sprintf("60cf84%06x", macCounter&0xFFFFFF)
	}

	// Discover the live UserID for adminToken directly from /authorization/deviceCommand
	// (works across mock_owsec where TOKEN_ADMIN_OPERATOR_A maps to 0000...0003 and real owsec)
	if probeStatus, probeBody, probeErr := client.DoRequest("POST", "/authorization/deviceCommand", adminToken, map[string]interface{}{
		"serialNumber": nextSerial(),
		"action":       "REBOOT",
	}); probeErr == nil && probeStatus == http.StatusOK {
		var probeResp struct {
			UserID string `json:"userId"`
		}
		if json.Unmarshal(probeBody, &probeResp) == nil && probeResp.UserID != "" {
			adminUserId = probeResp.UserID
		}
	}
	createEntity := func(name string) (string, func()) {
		s, b, e := client.DoRequest("POST", "/entity/0", rootToken, map[string]interface{}{
			"name":   name,
			"parent": "0000-0000-0000",
		})
		if e != nil || (s != http.StatusOK && s != http.StatusCreated) {
			t.Fatalf("Failed to create entity %s: status=%d err=%v body=%s", name, s, e, string(b))
		}
		var resp struct {
			ID string `json:"id"`
		}
		_ = json.Unmarshal(b, &resp)
		return resp.ID, func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/entity/%s", resp.ID), rootToken, nil)
		}
	}

	createVenue := func(name, entityID string) (string, func()) {
		s, b, e := client.DoRequest("POST", "/venue/0", rootToken, map[string]interface{}{
			"name":   name,
			"entity": entityID,
		})
		if e != nil || (s != http.StatusOK && s != http.StatusCreated) {
			t.Fatalf("Failed to create venue %s: status=%d err=%v body=%s", name, s, e, string(b))
		}
		var resp struct {
			ID string `json:"id"`
		}
		_ = json.Unmarshal(b, &resp)
		return resp.ID, func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", resp.ID), rootToken, nil)
		}
	}

	entityAId, cleanupEntityA := createEntity(fmt.Sprintf("inv-create-ent-a-%d", ts))
	defer cleanupEntityA()
	entityBId, cleanupEntityB := createEntity(fmt.Sprintf("inv-create-ent-b-%d", ts))
	defer cleanupEntityB()

	venueAId, cleanupVenueA := createVenue(fmt.Sprintf("inv-create-ven-a-%d", ts), entityAId)
	defer cleanupVenueA()
	venueA2Id, cleanupVenueA2 := createVenue(fmt.Sprintf("inv-create-ven-a2-%d", ts), entityAId)
	defer cleanupVenueA2()
	venueBId, cleanupVenueB := createVenue(fmt.Sprintf("inv-create-ven-b-%d", ts), entityBId)
	defer cleanupVenueB()

	policyCreateId, cleanupPol := getOrCreatePolicyWithAccess(t, client, rootToken, fmt.Sprintf("test-inv-create-scope-%d", ts), "inventory", []string{"CREATE"})
	defer cleanupPol()

	assignRole := func(entityID, venueID string) func() {
		s, b, e := client.DoRequest("POST", "/managementRole/0", rootToken, map[string]interface{}{
			"name":             fmt.Sprintf("inv-create-role-%d", time.Now().UnixNano()),
			"description":      "Scoped inventory CREATE role for F1 verification",
			"entity":           entityID,
			"venue":            venueID,
			"managementPolicy": policyCreateId,
			"users":            []string{adminUserId},
		})
		if e != nil || (s != http.StatusOK && s != http.StatusCreated) {
			t.Fatalf("Failed to create role: status=%d err=%v body=%s", s, e, string(b))
		}
		var resp struct {
			ID string `json:"id"`
		}
		_ = json.Unmarshal(b, &resp)
		return func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", resp.ID), rootToken, nil)
		}
	}

	// Grant inventory:CREATE only on Entity A
	cleanupEntityARole := assignRole(entityAId, "")
	defer cleanupEntityARole()

	assertRejected := func(t *testing.T, methodPath string, payload map[string]interface{}, serial string, expectMsgSubstr string) {
		t.Helper()
		status, body, err := client.DoRequest("POST", methodPath, adminToken, payload)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusForbidden && status != http.StatusBadRequest {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/inventory/%s", serial), rootToken, nil)
			t.Fatalf("Expected 403/400 for %s, got %d body=%s", methodPath, status, string(body))
		}
		if expectMsgSubstr != "" {
			var errObj struct {
				ErrorDescription string `json:"ErrorDescription"`
			}
			_ = json.Unmarshal(body, &errObj)
			if !strings.Contains(strings.ToLower(errObj.ErrorDescription), strings.ToLower(expectMsgSubstr)) {
				t.Errorf("Expected ErrorDescription containing %q, got %q (body=%s)", expectMsgSubstr, errObj.ErrorDescription, string(body))
			}
		}
		getStatus, _, _ := client.DoRequest("GET", fmt.Sprintf("/inventory/%s", serial), rootToken, nil)
		if getStatus == http.StatusOK {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/inventory/%s", serial), rootToken, nil)
			t.Fatalf("Device %s was created despite rejection!", serial)
		}
	}

	t.Run("Reject query entity vs different body entity", func(t *testing.T) {
		serial := nextSerial()
		assertRejected(t, fmt.Sprintf("/inventory/%s?entity=%s", serial, entityAId), map[string]interface{}{
			"serialNumber": serial,
			"name":         "F1TestEntityMismatch",
			"deviceType":   "edgecore_eap101",
			"entity":       entityBId,
		}, serial, "mismatch")
	})

	t.Run("Reject cross-field query entity vs body venue in other tenant", func(t *testing.T) {
		serial := nextSerial()
		assertRejected(t, fmt.Sprintf("/inventory/%s?entity=%s", serial, entityAId), map[string]interface{}{
			"serialNumber": serial,
			"name":         "F1TestCrossFieldEntityToVenue",
			"deviceType":   "edgecore_eap101",
			"venue":        venueBId,
		}, serial, "mismatch")
	})

	t.Run("Reject cross-field query entity vs body venue even in same entity", func(t *testing.T) {
		serial := nextSerial()
		assertRejected(t, fmt.Sprintf("/inventory/%s?entity=%s", serial, entityAId), map[string]interface{}{
			"serialNumber": serial,
			"name":         "F1TestCrossFieldSameEntityVenue",
			"deviceType":   "edgecore_eap101",
			"venue":        venueAId,
		}, serial, "mismatch")
	})

	t.Run("Reject unauthorized body entity without query params", func(t *testing.T) {
		serial := nextSerial()
		assertRejected(t, fmt.Sprintf("/inventory/%s", serial), map[string]interface{}{
			"serialNumber": serial,
			"name":         "F1TestDirectEntityB",
			"deviceType":   "edgecore_eap101",
			"entity":       entityBId,
		}, serial, "")
	})

	t.Run("Reject unauthorized body venue without query params", func(t *testing.T) {
		serial := nextSerial()
		assertRejected(t, fmt.Sprintf("/inventory/%s", serial), map[string]interface{}{
			"serialNumber": serial,
			"name":         "F1TestDirectVenueB",
			"deviceType":   "edgecore_eap101",
			"venue":        venueBId,
		}, serial, "")
	})

	t.Run("Reject unassigned non-root creation", func(t *testing.T) {
		serial := nextSerial()
		assertRejected(t, fmt.Sprintf("/inventory/%s", serial), map[string]interface{}{
			"serialNumber": serial,
			"name":         "F1TestUnassigned",
			"deviceType":   "edgecore_eap101",
		}, serial, "")
	})

	t.Run("Allow creation in authorized entity with matching or omitted query param", func(t *testing.T) {
		serial := nextSerial()
		status, body, err := client.DoRequest("POST", fmt.Sprintf("/inventory/%s?entity=%s", serial, entityAId), adminToken, map[string]interface{}{
			"serialNumber": serial,
			"name":         "F1TestValidMatching",
			"deviceType":   "edgecore_eap101",
			"entity":       entityAId,
		})
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			t.Fatalf("Expected 200/201 for valid matching entity create, got %d err=%v body=%s", status, err, string(body))
		}
		_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/inventory/%s", serial), rootToken, nil)
	})

	// Remove entity-wide role and switch to venue-scoped role (Venue A only) to verify cross-venue attacks
	cleanupEntityARole()
	cleanupVenueARole := assignRole(entityAId, venueAId)
	defer cleanupVenueARole()

	t.Run("Reject query venue vs different body venue (cross-venue)", func(t *testing.T) {
		serial := nextSerial()
		assertRejected(t, fmt.Sprintf("/inventory/%s?venue=%s", serial, venueAId), map[string]interface{}{
			"serialNumber": serial,
			"name":         "F1TestVenueMismatchOtherTenant",
			"deviceType":   "edgecore_eap101",
			"venue":        venueBId,
		}, serial, "mismatch")

		serialSameEnt := nextSerial()
		assertRejected(t, fmt.Sprintf("/inventory/%s?venue=%s", serialSameEnt, venueAId), map[string]interface{}{
			"serialNumber": serialSameEnt,
			"name":         "F1TestVenueMismatchSibling",
			"deviceType":   "edgecore_eap101",
			"venue":        venueA2Id,
		}, serialSameEnt, "mismatch")
	})

	t.Run("Reject cross-field query venue vs body entity", func(t *testing.T) {
		serial := nextSerial()
		assertRejected(t, fmt.Sprintf("/inventory/%s?venue=%s", serial, venueAId), map[string]interface{}{
			"serialNumber": serial,
			"name":         "F1TestVenueToEntityB",
			"deviceType":   "edgecore_eap101",
			"entity":       entityBId,
		}, serial, "mismatch")

		serialParentEnt := nextSerial()
		assertRejected(t, fmt.Sprintf("/inventory/%s?venue=%s", serialParentEnt, venueAId), map[string]interface{}{
			"serialNumber": serialParentEnt,
			"name":         "F1TestVenueToEntityA",
			"deviceType":   "edgecore_eap101",
			"entity":       entityAId,
		}, serialParentEnt, "mismatch")
	})

	t.Run("Allow creation in authorized venue only", func(t *testing.T) {
		serial := nextSerial()
		status, body, err := client.DoRequest("POST", fmt.Sprintf("/inventory/%s", serial), adminToken, map[string]interface{}{
			"serialNumber": serial,
			"name":         "F1TestValidVenue",
			"deviceType":   "edgecore_eap101",
			"venue":        venueAId,
		})
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			t.Fatalf("Expected 200/201 for valid venue create, got %d err=%v body=%s", status, err, string(body))
		}
		_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/inventory/%s", serial), rootToken, nil)
	})
}
