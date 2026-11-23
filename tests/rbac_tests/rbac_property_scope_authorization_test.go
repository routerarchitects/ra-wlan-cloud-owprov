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
}
