package rbac_tests

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"testing"
	"time"
)

func TestPropertyScopeAuthorizationRegressions(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	rootToken := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	testToken := getEnvOrDefault("PROPERTY_SCOPE_TEST_TOKEN", "Bearer property-scope-subscriber-token")
	testUserID := getEnvOrDefault("PROPERTY_SCOPE_TEST_USER_ID", "mock-subscriber-id")
	ts := time.Now().UnixNano()

	createEntity := func(name string) (string, func()) {
		t.Helper()
		status, body, err := client.DoRequest("POST", "/entity/0", rootToken, map[string]interface{}{
			"name":   name,
			"parent": "0000-0000-0000",
		})
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			t.Fatalf("create Entity %q failed: status=%d err=%v body=%s", name, status, err, string(body))
		}
		var response struct {
			ID string `json:"id"`
		}
		if err := json.Unmarshal(body, &response); err != nil || response.ID == "" {
			t.Fatalf("decode created Entity %q failed: err=%v body=%s", name, err, string(body))
		}
		return response.ID, func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/entity/%s", response.ID), rootToken, nil)
		}
	}

	createVenue := func(name, entityID string) (string, func()) {
		t.Helper()
		status, body, err := client.DoRequest("POST", "/venue/0", rootToken, map[string]interface{}{
			"name":   name,
			"entity": entityID,
		})
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			t.Fatalf("create Venue %q failed: status=%d err=%v body=%s", name, status, err, string(body))
		}
		var response struct {
			ID string `json:"id"`
		}
		if err := json.Unmarshal(body, &response); err != nil || response.ID == "" {
			t.Fatalf("decode created Venue %q failed: err=%v body=%s", name, err, string(body))
		}
		return response.ID, func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", response.ID), rootToken, nil)
		}
	}

	createPolicy := func(name string, entries []map[string]interface{}) (string, func()) {
		t.Helper()
		status, body, err := client.DoRequest("POST", "/managementPolicy/0", rootToken, map[string]interface{}{
			"name":        name,
			"description": "Property-scope RBAC regression policy",
			"entries":     entries,
		})
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			t.Fatalf("create policy %q failed: status=%d err=%v body=%s", name, status, err, string(body))
		}
		var response struct {
			ID string `json:"id"`
		}
		if err := json.Unmarshal(body, &response); err != nil || response.ID == "" {
			t.Fatalf("decode created policy %q failed: err=%v body=%s", name, err, string(body))
		}
		return response.ID, func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementPolicy/%s", response.ID), rootToken, nil)
		}
	}

	assignRole := func(name, entityID, venueID, policyID string) func() {
		t.Helper()
		status, body, err := client.DoRequest("POST", "/managementRole/0", rootToken, map[string]interface{}{
			"name":             name,
			"description":      "Property-scope RBAC regression role",
			"entity":           entityID,
			"venue":            venueID,
			"managementPolicy": policyID,
			"users":            []string{testUserID},
		})
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			t.Fatalf("assign role %q failed: status=%d err=%v body=%s", name, status, err, string(body))
		}
		var response struct {
			ID string `json:"id"`
		}
		if err := json.Unmarshal(body, &response); err != nil || response.ID == "" {
			t.Fatalf("decode created role %q failed: err=%v body=%s", name, err, string(body))
		}
		return func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/managementRole/%s", response.ID), rootToken, nil)
		}
	}

	entityA, cleanupEntityA := createEntity(fmt.Sprintf("property-rbac-entity-a-%d", ts))
	defer cleanupEntityA()
	entityB, cleanupEntityB := createEntity(fmt.Sprintf("property-rbac-entity-b-%d", ts))
	defer cleanupEntityB()
	venueA, cleanupVenueA := createVenue(fmt.Sprintf("property-rbac-venue-a-%d", ts), entityA)
	defer cleanupVenueA()
	venueB, cleanupVenueB := createVenue(fmt.Sprintf("property-rbac-venue-b-%d", ts), entityB)
	defer cleanupVenueB()

	readEntry := func(resource string) map[string]interface{} {
		return map[string]interface{}{
			"resources": []string{resource},
			"access":    []string{"READ"},
		}
	}

	t.Run("conflicting Venue parameters are rejected in either order", func(t *testing.T) {
		venuePolicy, cleanupVenuePolicy := createPolicy(
			fmt.Sprintf("property-rbac-venue-read-%d", ts),
			[]map[string]interface{}{readEntry("venue")},
		)
		defer cleanupVenuePolicy()
		locationPolicy, cleanupLocationPolicy := createPolicy(
			fmt.Sprintf("property-rbac-location-read-%d", ts),
			[]map[string]interface{}{readEntry("location")},
		)
		defer cleanupLocationPolicy()
		cleanupRoleA := assignRole(fmt.Sprintf("property-rbac-role-a-%d", ts), entityA, venueA, venuePolicy)
		defer cleanupRoleA()
		cleanupRoleB := assignRole(fmt.Sprintf("property-rbac-role-b-%d", ts), entityB, venueB, locationPolicy)
		defer cleanupRoleB()

		paths := []string{
			fmt.Sprintf("/venue?locationsForVenue=%s&venue=%s", venueB, venueA),
			fmt.Sprintf("/venue?venue=%s&locationsForVenue=%s", venueA, venueB),
			fmt.Sprintf("/venue?locationsForVenue=%s&locationsForVenue=%s", venueB, venueA),
		}
		for _, path := range paths {
			status, body, err := client.DoRequest("GET", path, testToken, nil)
			if err != nil {
				t.Fatalf("GET %s failed: %v", path, err)
			}
			if status != http.StatusForbidden && status != http.StatusBadRequest {
				t.Errorf("expected conflicting Venue parameters to be rejected for %s, got %d body=%s", path, status, string(body))
			}
		}

		status, body, err := client.DoRequest("GET", fmt.Sprintf("/venue?locationsForVenue=%s", venueB), testToken, nil)
		if err != nil {
			t.Fatalf("location-only Venue request failed: %v", err)
		}
		if status != http.StatusForbidden {
			t.Errorf("expected venue:READ to be required with location:READ, got %d body=%s", status, string(body))
		}

		status, body, err = client.DoRequest("GET", fmt.Sprintf("/venue?locationsForVenue=%s", venueA), testToken, nil)
		if err != nil {
			t.Fatalf("Venue-only Location request failed: %v", err)
		}
		if status != http.StatusForbidden {
			t.Errorf("expected location:READ to be required with venue:READ, got %d body=%s", status, string(body))
		}
	})

	t.Run("entity READ without venue READ hides Venue nodes", func(t *testing.T) {
		policyID, cleanupPolicy := createPolicy(
			fmt.Sprintf("property-rbac-entity-only-%d", ts),
			[]map[string]interface{}{readEntry("entity")},
		)
		defer cleanupPolicy()
		cleanupRole := assignRole(fmt.Sprintf("property-rbac-entity-role-%d", ts), entityA, "", policyID)
		defer cleanupRole()

		status, body, err := client.DoRequest("GET", "/entity?getTree=true", testToken, nil)
		if err != nil || status != http.StatusOK {
			t.Fatalf("entity-only tree request failed: status=%d err=%v body=%s", status, err, string(body))
		}
		response := string(body)
		if !strings.Contains(response, entityA) {
			t.Errorf("entity-only tree omitted authorized Entity %s: %s", entityA, response)
		}
		if strings.Contains(response, venueA) || strings.Contains(response, venueB) {
			t.Errorf("entity-only tree exposed Venue metadata: %s", response)
		}
	})

	t.Run("Venue-only Entity count and list are empty", func(t *testing.T) {
		policyID, cleanupPolicy := createPolicy(
			fmt.Sprintf("property-rbac-venue-tree-%d", ts),
			[]map[string]interface{}{readEntry("entity"), readEntry("venue")},
		)
		defer cleanupPolicy()
		cleanupRole := assignRole(fmt.Sprintf("property-rbac-venue-role-%d", ts), entityA, venueA, policyID)
		defer cleanupRole()

		status, body, err := client.DoRequest("GET", "/entity?countOnly=true", testToken, nil)
		if err != nil || status != http.StatusOK {
			t.Fatalf("Venue-only count request failed: status=%d err=%v body=%s", status, err, string(body))
		}
		var countResponse struct {
			Count int `json:"count"`
		}
		if err := json.Unmarshal(body, &countResponse); err != nil {
			t.Fatalf("decode Venue-only count response failed: %v body=%s", err, string(body))
		}
		if countResponse.Count != 0 {
			t.Errorf("expected Venue-only Entity count 0, got %d body=%s", countResponse.Count, string(body))
		}

		status, body, err = client.DoRequest("GET", "/entity", testToken, nil)
		if err != nil || status != http.StatusOK {
			t.Fatalf("Venue-only Entity list request failed: status=%d err=%v body=%s", status, err, string(body))
		}
		var listResponse struct {
			Entities []json.RawMessage `json:"entities"`
		}
		if err := json.Unmarshal(body, &listResponse); err != nil {
			t.Fatalf("decode Venue-only Entity list failed: %v body=%s", err, string(body))
		}
		if len(listResponse.Entities) != 0 {
			t.Errorf("expected Venue-only Entity list to be empty, got %d records body=%s", len(listResponse.Entities), string(body))
		}
	})

	t.Run("mixed Contact and Location select excludes foreign records", func(t *testing.T) {
		policyID, cleanupPolicy := createPolicy(
			fmt.Sprintf("property-rbac-associated-read-%d", ts),
			[]map[string]interface{}{readEntry("contact"), readEntry("location")},
		)
		defer cleanupPolicy()
		cleanupRole := assignRole(fmt.Sprintf("property-rbac-associated-role-%d", ts), entityA, "", policyID)
		defer cleanupRole()

		createRecord := func(resource, name, entityID string, extra map[string]interface{}) string {
			t.Helper()
			payload := map[string]interface{}{
				"name":   name,
				"entity": entityID,
			}
			for key, value := range extra {
				payload[key] = value
			}
			status, body, err := client.DoRequest("POST", "/"+resource+"/0", rootToken, payload)
			if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
				t.Fatalf("create %s %q failed: status=%d err=%v body=%s", resource, name, status, err, string(body))
			}
			var response struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal(body, &response); err != nil || response.ID == "" {
				t.Fatalf("decode created %s %q failed: err=%v body=%s", resource, name, err, string(body))
			}
			return response.ID
		}

		contactA := createRecord("contact", fmt.Sprintf("property-contact-a-%d", ts), entityA,
			map[string]interface{}{"email": fmt.Sprintf("contact-a-%d@example.com", ts)})
		contactB := createRecord("contact", fmt.Sprintf("property-contact-b-%d", ts), entityB,
			map[string]interface{}{"email": fmt.Sprintf("contact-b-%d@example.com", ts)})
		locationA := createRecord("location", fmt.Sprintf("property-location-a-%d", ts), entityA,
			map[string]interface{}{"city": "Scope A", "country": "US", "timezone": "UTC"})
		locationB := createRecord("location", fmt.Sprintf("property-location-b-%d", ts), entityB,
			map[string]interface{}{"city": "Scope B", "country": "US", "timezone": "UTC"})

		attach := func(venueID, contactID, locationID string) {
			t.Helper()
			status, body, err := client.DoRequest("PUT", fmt.Sprintf("/venue/%s", venueID), rootToken,
				map[string]interface{}{"contacts": []string{contactID}, "location": locationID})
			if err != nil || status != http.StatusOK {
				t.Fatalf("attach records to Venue %s failed: status=%d err=%v body=%s", venueID, status, err, string(body))
			}
		}
		attach(venueA, contactA, locationA)
		attach(venueB, contactB, locationB)
		defer func() {
			_, _, _ = client.DoRequest("PUT", fmt.Sprintf("/venue/%s", venueA), rootToken,
				map[string]interface{}{"contacts": []string{}, "location": ""})
			_, _, _ = client.DoRequest("PUT", fmt.Sprintf("/venue/%s", venueB), rootToken,
				map[string]interface{}{"contacts": []string{}, "location": ""})
			for _, record := range []struct{ resource, id string }{
				{"contact", contactA}, {"contact", contactB},
				{"location", locationA}, {"location", locationB},
			} {
				_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/%s/%s", record.resource, record.id), rootToken, nil)
			}
		}()

		assertSelected := func(resource, block, authorizedID, foreignID string) {
			t.Helper()
			path := fmt.Sprintf("/%s?entity=%s&venue=%s&select=%s,%s",
				resource, entityA, venueA, authorizedID, foreignID)
			status, body, err := client.DoRequest("GET", path, testToken, nil)
			if err != nil || status != http.StatusOK {
				t.Fatalf("mixed %s select failed: status=%d err=%v body=%s", resource, status, err, string(body))
			}
			var response map[string][]struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal(body, &response); err != nil {
				t.Fatalf("decode mixed %s select failed: err=%v body=%s", resource, err, string(body))
			}
			records := response[block]
			if len(records) != 1 || records[0].ID != authorizedID {
				t.Fatalf("mixed %s select returned records outside the requested authorized scope: %s", resource, string(body))
			}
		}

		assertSelected("contact", "contacts", contactA, contactB)
		assertSelected("location", "locations", locationA, locationB)
	})

	t.Run("Subscriber Device select applies per-record scope authorization", func(t *testing.T) {
		status, body, err := client.DoRequest("POST", "/operator/0000-0000-0000", rootToken,
			map[string]interface{}{
				"name":           fmt.Sprintf("property-scope-operator-%d", ts),
				"registrationId": fmt.Sprintf("property-scope-%d", ts),
			})
		if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
			t.Fatalf("create test Operator failed: status=%d err=%v body=%s", status, err, string(body))
		}
		var operator struct {
			ID       string `json:"id"`
			EntityID string `json:"entityId"`
		}
		if err := json.Unmarshal(body, &operator); err != nil || operator.ID == "" || operator.EntityID == "" {
			t.Fatalf("decode test Operator failed: err=%v body=%s", err, string(body))
		}
		defer func() {
			_, _, _ = client.DoRequest("DELETE", fmt.Sprintf("/operator/%s", operator.ID), rootToken, nil)
		}()

		policyID, cleanupPolicy := createPolicy(
			fmt.Sprintf("property-rbac-subscriber-device-read-%d", ts),
			[]map[string]interface{}{readEntry("inventory")},
		)
		defer cleanupPolicy()
		cleanupOperatorScopeRole := assignRole(
			fmt.Sprintf("property-rbac-operator-scope-role-%d", ts), operator.EntityID, "", policyID)
		defer cleanupOperatorScopeRole()
		cleanupDeviceScopeRole := assignRole(
			fmt.Sprintf("property-rbac-device-scope-role-%d", ts), entityA, venueA, policyID)
		defer cleanupDeviceScopeRole()

		serialSuffix := uint64(ts) & 0xffffffffff
		serialA := fmt.Sprintf("aa%010x", serialSuffix)
		serialB := fmt.Sprintf("ab%010x", serialSuffix)
		createInventory := func(serial, name, venueID string) {
			t.Helper()
			status, body, err := client.DoRequest("POST", "/inventory/"+serial, rootToken,
				map[string]interface{}{
					"name":           name,
					"serialNumber":   serial,
					"deviceType":     "edgecore_eap101",
					"devClass":       "any",
					"venue":          venueID,
					"realMacAddress": serial,
				})
			if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
				t.Fatalf("create Inventory %s failed: status=%d err=%v body=%s", serial, status, err, string(body))
			}
		}
		createInventory(serialA, fmt.Sprintf("property-device-a-%d", ts), venueA)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", "/inventory/"+serialA, rootToken, nil)
		}()
		createInventory(serialB, fmt.Sprintf("property-device-b-%d", ts), venueB)
		defer func() {
			_, _, _ = client.DoRequest("DELETE", "/inventory/"+serialB, rootToken, nil)
		}()

		createSubscriberDevice := func(serial, name, group string) string {
			t.Helper()
			status, body, err := client.DoRequest("POST", "/subscriberDevice/0", rootToken,
				map[string]interface{}{
					"name":           name,
					"serialNumber":   serial,
					"operatorId":     operator.ID,
					"subscriberId":   "00000000-0000-0000-0000-000000000005",
					"deviceGroup":    group,
					"deviceType":     "edgecore_eap101",
					"realMacAddress": serial,
				})
			if err != nil || (status != http.StatusOK && status != http.StatusCreated) {
				t.Fatalf("create Subscriber Device %s failed: status=%d err=%v body=%s", serial, status, err, string(body))
			}
			var response struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal(body, &response); err != nil || response.ID == "" {
				t.Fatalf("decode Subscriber Device %s failed: err=%v body=%s", serial, err, string(body))
			}
			return response.ID
		}
		deviceA := createSubscriberDevice(serialA, fmt.Sprintf("property-subscriber-device-a-%d", ts), "olg")
		defer func() {
			_, _, _ = client.DoRequest("DELETE", "/subscriberDevice/"+serialA, rootToken, nil)
		}()
		deviceB := createSubscriberDevice(serialB, fmt.Sprintf("property-subscriber-device-b-%d", ts), "ap")
		defer func() {
			_, _, _ = client.DoRequest("DELETE", "/subscriberDevice/"+serialB, rootToken, nil)
		}()

		path := fmt.Sprintf("/subscriberDevice?operatorId=%s&select=%s,%s", operator.ID, deviceA, deviceB)
		status, body, err = client.DoRequest("GET", path, testToken, nil)
		if err != nil || status != http.StatusOK {
			t.Fatalf("cross-scope Subscriber Device select failed: status=%d err=%v body=%s", status, err, string(body))
		}
		var response struct {
			Devices []struct {
				ID string `json:"id"`
			} `json:"subscriberDevices"`
		}
		if err := json.Unmarshal(body, &response); err != nil {
			t.Fatalf("decode Subscriber Device select failed: err=%v body=%s", err, string(body))
		}
		if len(response.Devices) != 1 || response.Devices[0].ID != deviceA {
			t.Fatalf("Subscriber Device select returned records outside the authorized Device scope: %s", string(body))
		}
	})
}
