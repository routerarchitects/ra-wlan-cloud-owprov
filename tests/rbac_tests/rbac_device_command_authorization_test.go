package rbac_tests

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"
)

/*
 * TestDeviceCommandAuthorization
 *
 * VALIDATES POST /api/v1/authorization/deviceCommand:
 *   1. Normal authorization (entity-owned and venue-owned devices, MAC normalization, ROOT on existing device).
 *   2. Missing, invalid, and expired Bearer tokens (and internal endpoint Bearer requirement).
 *   3. F2: Public endpoint with both privileged X-API-KEY and restricted Bearer token enforces Bearer user's policy.
 *   4. F5: Malformed serial numbers rejected with 400; ROOT on nonexistent device returns allowed=false (DEVICE_NOT_FOUND).
 *   5. Venue override precedence: authoritative venue role overrides broader entity role; no venue role falls back to venue's entity.
 *   6. F4: Entity-owned devices require exact entity role (no parent->child entity inheritance or ancestor shadowing).
 *   7. Action-specific permissions & aliases (BLINK/LEDS, WIFI_SCAN/WIFISCAN, FACTORY_RESET/FACTORY, RE_ENROLL/REENROLL).
 *   8. F7: Unknown actions rejected rather than authorized under FULL policy (or ROOT).
 */
func TestDeviceCommandAuthorization(t *testing.T) {
	baseURL := getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1")
	client := NewTestClient(baseURL)
	rootToken, adminToken, adminUserId, _, _, _ := getLiveTestTokensAndFixtures(t)
	readOnlyToken := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-token")

	if adminToken == "" || adminUserId == "" {
		t.Skip("Skipping test: no non-root admin user/token available")
	}

	type authzResp struct {
		Allowed      bool   `json:"allowed"`
		Reason       string `json:"reason"`
		Details      string `json:"details"`
		Resource     string `json:"resource"`
		Action       string `json:"action"`
		SerialNumber string `json:"serialNumber"`
		UserID       string `json:"userId"`
		PolicyID     string `json:"policyId"`
		PolicyName   string `json:"policyName"`
	}

	doRequestWithHeaders := func(targetURL string, headers map[string]string, body interface{}) (int, []byte, error) {
		var bodyReader io.Reader
		if body != nil {
			raw, err := json.Marshal(body)
			if err != nil {
				return 0, nil, err
			}
			bodyReader = bytes.NewBuffer(raw)
		}
		req, err := http.NewRequest("POST", targetURL, bodyReader)
		if err != nil {
			return 0, nil, err
		}
		req.Header.Set("Content-Type", "application/json")
		for k, v := range headers {
			req.Header.Set(k, v)
		}
		resp, err := client.Client.Do(req)
		if err != nil {
			return 0, nil, err
		}
		defer resp.Body.Close()
		respBody, err := io.ReadAll(resp.Body)
		return resp.StatusCode, respBody, err
	}

	callAuthz := func(token string, serial, action string) (int, authzResp, []byte) {
		t.Helper()
		headers := map[string]string{}
		if token != "" {
			headers["Authorization"] = token
		}
		status, body, err := doRequestWithHeaders(baseURL+"/authorization/deviceCommand", headers, map[string]interface{}{
			"serialNumber": serial,
			"action":       action,
		})
		if err != nil {
			t.Fatalf("Request to /authorization/deviceCommand failed: %v", err)
		}
		var parsed authzResp
		_ = json.Unmarshal(body, &parsed)
		return status, parsed, body
	}

	ts := time.Now().UnixNano()
	macCounter := uint64(ts & 0xFFFFFFFF)
	nextSerial := func() string {
		macCounter++
		return fmt.Sprintf("ddcc%08x", macCounter)
	}

	// Discover the live UserID for adminToken directly from the endpoint (works across mock_owsec and real owsec)
	if status, probeResp, probeBody := callAuthz(adminToken, nextSerial(), "REBOOT"); status == http.StatusOK && probeResp.UserID != "" {
		adminUserId = probeResp.UserID
	} else {
		t.Logf("Probe for admin userId returned status=%d body=%s; using fixture userId=%s", status, string(probeBody), adminUserId)
	}

	createEntity := func(name, parentID string) (string, func()) {
		s, b, e := client.DoRequest("POST", "/entity/0", rootToken, map[string]interface{}{
			"name":   name,
			"parent": parentID,
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

	createDevice := func(serial, entityID, venueID string) func() {
		payload := map[string]interface{}{
			"serialNumber": serial,
			"name":         fmt.Sprintf("dev-%s", serial),
			"deviceType":   "edgecore_eap101",
		}
		if venueID != "" {
			payload["venue"] = venueID
		} else if entityID != "" {
			payload["entity"] = entityID
		}
		s, b, e := client.DoRequest("POST", fmt.Sprintf("/inventory/%s", serial), rootToken, payload)
		if e != nil || (s != http.StatusOK && s != http.StatusCreated) {
			t.Fatalf("Failed to create device %s: status=%d err=%v body=%s", serial, s, e, string(b))
		}
		return func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/inventory/%s", serial), rootToken, nil)
		}
	}

	createPolicy := func(name string, entries []map[string]interface{}) (string, func()) {
		s, b, e := client.DoRequest("POST", "/managementPolicy/0", rootToken, map[string]interface{}{
			"name":        name,
			"description": "Regression test policy for deviceCommand authorization",
			"entries":     entries,
		})
		if e != nil || (s != http.StatusOK && s != http.StatusCreated) {
			t.Fatalf("Failed to create policy %s: status=%d err=%v body=%s", name, s, e, string(b))
		}
		var resp struct {
			ID string `json:"id"`
		}
		_ = json.Unmarshal(b, &resp)
		return resp.ID, func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", resp.ID), rootToken, nil)
		}
	}

	assignRole := func(entityID, venueID, policyID, userID string) func() {
		s, b, e := client.DoRequest("POST", "/managementRole/0", rootToken, map[string]interface{}{
			"name":             fmt.Sprintf("devcmd-role-%d", time.Now().UnixNano()),
			"description":      "Test role for deviceCommand authorization",
			"entity":           entityID,
			"venue":            venueID,
			"managementPolicy": policyID,
			"users":            []string{userID},
		})
		if e != nil || (s != http.StatusOK && s != http.StatusCreated) {
			t.Fatalf("Failed to assign role: status=%d err=%v body=%s", s, e, string(b))
		}
		var resp struct {
			ID string `json:"id"`
		}
		_ = json.Unmarshal(b, &resp)
		return func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", resp.ID), rootToken, nil)
		}
	}

	createOperatorEntity := func(name, regID string) (string, func()) {
		s, b, e := client.DoRequest("POST", "/operator/0", rootToken, map[string]interface{}{
			"name":           name,
			"registrationId": regID,
		})
		if e != nil || (s != http.StatusOK && s != http.StatusCreated) {
			t.Fatalf("Failed to create operator %s: status=%d err=%v body=%s", name, s, e, string(b))
		}
		var resp struct {
			ID       string `json:"id"`
			EntityID string `json:"entityId"`
		}
		_ = json.Unmarshal(b, &resp)
		if resp.ID == "" || resp.EntityID == "" {
			t.Fatalf("Operator created without id/entityId: %s", string(b))
		}
		return resp.EntityID, func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/operator/%s", resp.ID), rootToken, nil)
		}
	}

	// Provision isolated tenant topology: Entity A, Operator Parent -> Child Entity, Venues A1/A2, Entity B
	entityAId, cleanupEntA := createEntity(fmt.Sprintf("devcmd-ent-a-%d", ts), "0000-0000-0000")
	defer cleanupEntA()
	opParentEntityId, cleanupOpParent := createOperatorEntity(
		fmt.Sprintf("devcmd-op-parent-%d", ts),
		fmt.Sprintf("reg-devcmd-%d", ts),
	)
	defer cleanupOpParent()
	entityAChildId, cleanupEntAChild := createEntity(fmt.Sprintf("devcmd-ent-a-child-%d", ts), opParentEntityId)
	defer cleanupEntAChild()
	entityBId, cleanupEntB := createEntity(fmt.Sprintf("devcmd-ent-b-%d", ts), "0000-0000-0000")
	defer cleanupEntB()

	venueA1Id, cleanupVenA1 := createVenue(fmt.Sprintf("devcmd-ven-a1-%d", ts), entityAId)
	defer cleanupVenA1()
	venueA2Id, cleanupVenA2 := createVenue(fmt.Sprintf("devcmd-ven-a2-%d", ts), entityAId)
	defer cleanupVenA2()

	entADeviceSerial := nextSerial()
	cleanupDevEntA := createDevice(entADeviceSerial, entityAId, "")
	defer cleanupDevEntA()

	entAChildDeviceSerial := nextSerial()
	cleanupDevEntAChild := createDevice(entAChildDeviceSerial, entityAChildId, "")
	defer cleanupDevEntAChild()

	venA1DeviceSerial := nextSerial()
	cleanupDevVenA1 := createDevice(venA1DeviceSerial, "", venueA1Id)
	defer cleanupDevVenA1()

	venA2DeviceSerial := nextSerial()
	cleanupDevVenA2 := createDevice(venA2DeviceSerial, "", venueA2Id)
	defer cleanupDevVenA2()

	entBDeviceSerial := nextSerial()
	cleanupDevEntB := createDevice(entBDeviceSerial, entityBId, "")
	defer cleanupDevEntB()

	// Policies
	polFullId, cleanupPolFull := createPolicy(fmt.Sprintf("devcmd-full-%d", ts), []map[string]interface{}{
		{"resources": []string{"deviceCommand"}, "access": []string{"FULL"}},
	})
	defer cleanupPolFull()

	polPartialId, cleanupPolPartial := createPolicy(fmt.Sprintf("devcmd-partial-%d", ts), []map[string]interface{}{
		{"resources": []string{"deviceCommand"}, "access": []string{"REBOOT", "BLINK", "WIFI_SCAN", "FACTORY_RESET", "RE_ENROLL"}},
	})
	defer cleanupPolPartial()

	polTelemetryOnlyId, cleanupPolTelemetry := createPolicy(fmt.Sprintf("devcmd-telemetry-%d", ts), []map[string]interface{}{
		{"resources": []string{"deviceCommand"}, "access": []string{"TELEMETRY"}},
	})
	defer cleanupPolTelemetry()

	polNoCmdId, cleanupPolNoCmd := createPolicy(fmt.Sprintf("devcmd-nocmd-%d", ts), []map[string]interface{}{
		{"resources": []string{"inventory"}, "access": []string{"FULL"}},
	})
	defer cleanupPolNoCmd()

	// 1. Auth & Token Validation
	t.Run("Missing and invalid tokens are rejected", func(t *testing.T) {
		s, _, b := callAuthz("", entADeviceSerial, "REBOOT")
		if s != http.StatusUnauthorized && s != http.StatusForbidden {
			t.Fatalf("Expected 401/403 without token, got %d body=%s", s, string(b))
		}

		s, _, b = callAuthz("Bearer invalid-devicecmd-token", entADeviceSerial, "REBOOT")
		if s != http.StatusUnauthorized && s != http.StatusForbidden {
			t.Fatalf("Expected 401/403 for invalid token, got %d body=%s", s, string(b))
		}

		s, _, b = callAuthz("Bearer expired-devicecmd-token", entADeviceSerial, "REBOOT")
		if s != http.StatusUnauthorized && s != http.StatusForbidden {
			t.Fatalf("Expected 401/403 for expired token, got %d body=%s", s, string(b))
		}
	})

	// 2. F5: ROOT & Input Validation
	t.Run("Malformed serial rejected and ROOT denied for nonexistent device", func(t *testing.T) {
		for _, badSerial := range []string{"not-a-mac", "12345", "zzzzzzzzzzzz"} {
			s, _, b := callAuthz(rootToken, badSerial, "REBOOT")
			if s != http.StatusBadRequest {
				t.Fatalf("Expected 400 for malformed serial '%s', got %d body=%s", badSerial, s, string(b))
			}
		}

		missingSerial := nextSerial()
		s, resp, b := callAuthz(rootToken, missingSerial, "REBOOT")
		if s != http.StatusOK || resp.Allowed || resp.Reason != "DEVICE_NOT_FOUND" {
			t.Fatalf("Expected ROOT on nonexistent device to return allowed=false DEVICE_NOT_FOUND, got status=%d resp=%+v body=%s", s, resp, string(b))
		}

		s, resp, b = callAuthz(rootToken, entADeviceSerial, "REBOOT")
		if s != http.StatusOK || !resp.Allowed || resp.Reason != "ALLOWED" {
			t.Fatalf("Expected ROOT on existing device to return allowed=true ALLOWED, got status=%d resp=%+v body=%s", s, resp, string(b))
		}
	})

	// 3. Zero-role user
	t.Run("User with no roles returns USER_HAS_NO_ROLES", func(t *testing.T) {
		s, resp, b := callAuthz(readOnlyToken, entADeviceSerial, "REBOOT")
		if s != http.StatusOK || resp.Allowed || resp.Reason != "USER_HAS_NO_ROLES" {
			t.Fatalf("Expected allowed=false USER_HAS_NO_ROLES, got status=%d resp=%+v body=%s", s, resp, string(b))
		}
	})

	// 4. Normal entity ownership, MAC normalization, Unknown Action under FULL policy, and F2 X-API-KEY override
	t.Run("Entity FULL policy, MAC normalization, unknown action rejection, and F2 X-API-KEY precedence", func(t *testing.T) {
		cleanupRoleA := assignRole(entityAId, "", polFullId, adminUserId)
		defer cleanupRoleA()

		// Colon-separated uppercase serial must normalize and authorize
		formattedSerial := strings.ToUpper(fmt.Sprintf("%s:%s:%s:%s:%s:%s",
			entADeviceSerial[0:2], entADeviceSerial[2:4], entADeviceSerial[4:6],
			entADeviceSerial[6:8], entADeviceSerial[8:10], entADeviceSerial[10:12]))
		s, resp, b := callAuthz(adminToken, formattedSerial, "reboot")
		if s != http.StatusOK || !resp.Allowed || resp.Reason != "ALLOWED" ||
			resp.SerialNumber != entADeviceSerial || resp.Resource != "deviceCommand" ||
			resp.Action != "REBOOT" || resp.PolicyID != polFullId {
			t.Fatalf("Expected normalized ALLOWED response, got status=%d resp=%+v body=%s", s, resp, string(b))
		}

		// Unknown actions (and policy keyword "FULL") must be rejected even under a FULL policy or ROOT
		for _, unknownAct := range []string{"UNKNOWN_CMD", "FULL", "DELETE"} {
			s, resp, b = callAuthz(adminToken, entADeviceSerial, unknownAct)
			if s != http.StatusBadRequest || resp.Allowed {
				t.Fatalf("Expected 400 rejection for unknown action '%s' under FULL policy, got status=%d resp=%+v body=%s", unknownAct, s, resp, string(b))
			}
			s, resp, b = callAuthz(rootToken, entADeviceSerial, unknownAct)
			if s != http.StatusBadRequest || resp.Allowed {
				t.Fatalf("Expected 400 rejection for unknown action '%s' as ROOT, got status=%d resp=%+v body=%s", unknownAct, s, resp, string(b))
			}
		}

		// F2: Sending ROOT X-API-KEY alongside adminToken Bearer for out-of-scope Entity B must still deny (OUT_OF_SCOPE)
		rawRootKey := strings.TrimPrefix(rootToken, "Bearer ")
		status, body, err := doRequestWithHeaders(baseURL+"/authorization/deviceCommand", map[string]string{
			"X-API-KEY":     rawRootKey,
			"Authorization": adminToken,
		}, map[string]interface{}{
			"serialNumber": entBDeviceSerial,
			"action":       "REBOOT",
		})
		if err != nil {
			t.Fatalf("F2 mixed-header request failed: %v", err)
		}
		var mixedResp authzResp
		_ = json.Unmarshal(body, &mixedResp)
		if status != http.StatusOK || mixedResp.Allowed || mixedResp.Reason != "OUT_OF_SCOPE" {
			t.Fatalf("F2 regression: X-API-KEY leaked ROOT privilege over Bearer user; got status=%d resp=%+v body=%s", status, mixedResp, string(body))
		}
	})

	// 5. Action-specific permissions & aliases
	t.Run("Action-specific permissions and aliases enforced strictly", func(t *testing.T) {
		cleanupRolePartial := assignRole(entityAId, "", polPartialId, adminUserId)
		defer cleanupRolePartial()

		allowedActions := []string{"REBOOT", "BLINK", "leds", "WIFI_SCAN", "wifiscan", "FACTORY_RESET", "factory", "RE_ENROLL", "reenroll"}
		for _, act := range allowedActions {
			s, resp, b := callAuthz(adminToken, entADeviceSerial, act)
			if s != http.StatusOK || !resp.Allowed || resp.Reason != "ALLOWED" {
				t.Fatalf("Expected action '%s' to be allowed by partial policy, got status=%d resp=%+v body=%s", act, s, resp, string(b))
			}
		}

		deniedActions := []string{"UPGRADE", "TELEMETRY", "RTTY", "TRACE", "SCRIPT"}
		for _, act := range deniedActions {
			s, resp, b := callAuthz(adminToken, entADeviceSerial, act)
			if s != http.StatusOK || resp.Allowed || resp.Reason != "ACTION_NOT_GRANTED" {
				t.Fatalf("Expected action '%s' to be denied (ACTION_NOT_GRANTED), got status=%d resp=%+v body=%s", act, s, resp, string(b))
			}
		}
	})

	// 6. Venue override & entity fallback behavior
	t.Run("Venue role overrides entity role and venue falls back to owning entity when absent", func(t *testing.T) {
		cleanupEntityRole := assignRole(entityAId, "", polFullId, adminUserId)
		defer cleanupEntityRole()

		// Without venue role on Venue A1, entity role covers Venue A1 device
		s, resp, b := callAuthz(adminToken, venA1DeviceSerial, "REBOOT")
		if s != http.StatusOK || !resp.Allowed || resp.Reason != "ALLOWED" {
			t.Fatalf("Expected entity role fallback to allow Venue A1 device, got status=%d resp=%+v body=%s", s, resp, string(b))
		}

		// Assigning narrower TELEMETRY-only venue role on Venue A1 overrides Entity A FULL role
		cleanupVenueRole := assignRole(entityAId, venueA1Id, polTelemetryOnlyId, adminUserId)
		defer cleanupVenueRole()

		s, resp, b = callAuthz(adminToken, venA1DeviceSerial, "REBOOT")
		if s != http.StatusOK || resp.Allowed || resp.Reason != "ACTION_NOT_GRANTED" || resp.PolicyID != polTelemetryOnlyId {
			t.Fatalf("Expected venue role to override entity FULL role and deny REBOOT, got status=%d resp=%+v body=%s", s, resp, string(b))
		}

		s, resp, b = callAuthz(adminToken, venA1DeviceSerial, "TELEMETRY")
		if s != http.StatusOK || !resp.Allowed || resp.Reason != "ALLOWED" || resp.PolicyID != polTelemetryOnlyId {
			t.Fatalf("Expected venue role to allow TELEMETRY, got status=%d resp=%+v body=%s", s, resp, string(b))
		}

		// Sibling Venue A2 (no specific venue role) still inherits Entity A FULL role
		s, resp, b = callAuthz(adminToken, venA2DeviceSerial, "REBOOT")
		if s != http.StatusOK || !resp.Allowed || resp.Reason != "ALLOWED" || resp.PolicyID != polFullId {
			t.Fatalf("Expected Venue A2 without venue role to fall back to Entity A role, got status=%d resp=%+v body=%s", s, resp, string(b))
		}
	})

	// 7. F4: Entity ownership strictness (no parent->child inheritance)
	t.Run("F4 Entity-owned devices require exact entity role without ancestor inheritance", func(t *testing.T) {
		cleanupParentRole := assignRole(opParentEntityId, "", polFullId, adminUserId)
		defer cleanupParentRole()

		// Parent entity role alone must NOT authorize child entity device
		s, resp, b := callAuthz(adminToken, entAChildDeviceSerial, "REBOOT")
		if s != http.StatusOK || resp.Allowed || resp.Reason != "OUT_OF_SCOPE" {
			t.Fatalf("F4 regression: Parent entity role granted access to child entity device; got status=%d resp=%+v body=%s", s, resp, string(b))
		}

		// Adding restricted child entity role evaluates child role and denies REBOOT (never shadowed by parent role)
		cleanupChildRestrictedRole := assignRole(entityAChildId, "", polNoCmdId, adminUserId)
		s, resp, b = callAuthz(adminToken, entAChildDeviceSerial, "REBOOT")
		cleanupChildRestrictedRole()
		if s != http.StatusOK || resp.Allowed || resp.Reason != "ACTION_NOT_GRANTED" || resp.PolicyID != polNoCmdId {
			t.Fatalf("F4 regression: Expected restricted child entity role to deny REBOOT, got status=%d resp=%+v body=%s", s, resp, string(b))
		}

		// Direct FULL role on child entity authorizes child entity device
		cleanupChildFullRole := assignRole(entityAChildId, "", polFullId, adminUserId)
		defer cleanupChildFullRole()
		s, resp, b = callAuthz(adminToken, entAChildDeviceSerial, "REBOOT")
		if s != http.StatusOK || !resp.Allowed || resp.Reason != "ALLOWED" || resp.PolicyID != polFullId {
			t.Fatalf("Expected direct child entity FULL role to allow REBOOT, got status=%d resp=%+v body=%s", s, resp, string(b))
		}
	})

	// 8. Internal endpoint strictly requires X-INTERNAL-NAME and valid X-API-KEY (no Bearer fallback)
	t.Run("Internal endpoint rejects Bearer token without valid X-INTERNAL-NAME and X-API-KEY", func(t *testing.T) {
		internalBaseURL := getEnvOrDefault("OWPROV_INTERNAL_URL", strings.Replace(baseURL, ":16005", ":17005", 1))
		if internalBaseURL == baseURL {
			t.Skip("Skipping internal endpoint check: could not derive internal URL from OWPROV_URL")
		}

		payload := map[string]interface{}{
			"serialNumber": entADeviceSerial,
			"action":       "REBOOT",
		}

		s, b, err := doRequestWithHeaders(internalBaseURL+"/authorization/deviceCommand", map[string]string{
			"Authorization": rootToken,
		}, payload)
		if err != nil {
			t.Skipf("Internal endpoint %s unreachable in this environment: %v", internalBaseURL, err)
		}
		if s != http.StatusUnauthorized && s != http.StatusForbidden {
			t.Fatalf("Expected 401/403 on internal endpoint without X-INTERNAL-NAME and X-API-KEY, got %d body=%s", s, string(b))
		}

		s, b, err = doRequestWithHeaders(internalBaseURL+"/authorization/deviceCommand", map[string]string{
			"Authorization":   rootToken,
			"X-INTERNAL-NAME": "owgw-test",
			"X-API-KEY":       "invalid-service-key",
		}, payload)
		if err != nil {
			t.Fatalf("Request with invalid service key failed unexpectedly: %v", err)
		}
		if s != http.StatusUnauthorized && s != http.StatusForbidden {
			t.Fatalf("Expected 401/403 on internal endpoint with invalid X-API-KEY, got %d body=%s", s, string(b))
		}

		s, b, err = doRequestWithHeaders(internalBaseURL+"/authorization/deviceCommand", map[string]string{
			"Authorization": rootToken,
			"X-API-KEY":     "invalid-service-key",
		}, payload)
		if err != nil {
			t.Fatalf("Request missing X-INTERNAL-NAME failed unexpectedly: %v", err)
		}
		if s != http.StatusUnauthorized && s != http.StatusForbidden {
			t.Fatalf("Expected 401/403 on internal endpoint without X-INTERNAL-NAME, got %d body=%s", s, string(b))
		}
	})
}
