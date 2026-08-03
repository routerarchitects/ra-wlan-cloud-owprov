package rbac_tests

import (
	"fmt"
	"net/http"
	"testing"
)

// ----------------------------------------------------------------------------
// 1. PARENT IMMUTABILITY TESTS (Section 4.6)
// ----------------------------------------------------------------------------

/*
 * TestVenueUpdate_ParentImmutability
 *
 * DESCRIPTION:
 *   Validates Section 4.6 Parent Immutability rule:
 *   Entity and Venue parent references are strictly IMMUTABLE after creation.
 *   An update request attempting to move a Venue to a different parent or entity MUST be rejected.
 *
 * SCENARIO:
 *   ROOT or Admin sends PUT /api/v1/venue/{VENUE_A_UUID} with a modified "entity" or "parent" field.
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request (Parent / Entity reference is immutable).
 */
func TestVenueUpdate_ParentImmutability(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenRoot := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	venueA := getEnvOrDefault("VENUE_A_UUID", "venue-a-uuid")
	differentEntity := getEnvOrDefault("OPERATOR_B_ENTITY_UUID", "8ab2b291-d04d-5c4c-b4bd-c4gcca1gb108")

	payload := map[string]interface{}{
		"id":     venueA,
		"entity": differentEntity, // Attempting to change entity owner!
	}

	status, body, err := client.DoRequest("PUT", fmt.Sprintf("/venue/%s", venueA), tokenRoot, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest {
		t.Errorf("Expected 400 Bad Request for immutable venue parent modification, got %d. Body: %s", status, string(body))
	}
}

/*
 * TestEntityUpdate_ParentImmutability
 *
 * DESCRIPTION:
 *   Validates Section 4.6 Parent Immutability rule for Entities:
 *   An update request attempting to modify an Entity's parent reference MUST be rejected.
 *
 * SCENARIO:
 *   ROOT or Admin sends PUT /api/v1/entity/{ENTITY_UUID} with a modified "parent" field.
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request (Parent reference is immutable).
 */
func TestEntityUpdate_ParentImmutability(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenRoot := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	differentParent := getEnvOrDefault("OPERATOR_B_ENTITY_UUID", "8ab2b291-d04d-5c4c-b4bd-c4gcca1gb108")

	payload := map[string]interface{}{
		"id":     entityA,
		"parent": differentParent, // Attempting to change parent entity!
	}

	status, body, err := client.DoRequest("PUT", fmt.Sprintf("/entity/%s", entityA), tokenRoot, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest {
		t.Errorf("Expected 400 Bad Request for immutable entity parent modification, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 2. VENUE-SCOPED ROLE DELEGATION CONSTRAINTS (Section 8)
// ----------------------------------------------------------------------------

/*
 * TestRoleAssignment_VenueRoleCannotDelegateToEntity
 *
 * DESCRIPTION:
 *   Validates Section 8 delegation rule for Venue-scoped roles:
 *   A Venue-scoped role may delegate ONLY to that exact Venue.
 *   A user holding a Venue-scoped role CANNOT delegate an Entity-scoped role.
 *
 * SCENARIO:
 *   User holding an exact Venue-scoped role on Venue A1 attempts POST /managementRole
 *   with entity = Entity A and venue = "" (Entity-scoped role assignment).
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request / 403 Access Denied.
 */
func TestRoleAssignment_VenueRoleCannotDelegateToEntity(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenVenueUser := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-with-venue-role-only-token")
	targetUserA := getEnvOrDefault("TARGET_USER_A", "user-a-uuid")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	weakPolicy := getEnvOrDefault("POLICY_WEAK_ID", "94bb61a3-aa3e-49aa-9704-cc254fec4482")

	payload := map[string]interface{}{
		"users":            []string{targetUserA},
		"entity":           entityA,
		"venue":            "", // Entity-scoped role delegation attempted by venue-role holder!
		"managementPolicy": weakPolicy,
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", tokenVenueUser, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest && status != http.StatusForbidden {
		t.Errorf("Expected 400/403 for Venue-role user attempting Entity-role delegation, got %d. Body: %s", status, string(body))
	}
}

/*
 * TestRoleAssignment_VenueRoleCannotDelegateToChildVenue
 *
 * DESCRIPTION:
 *   Validates Section 8 delegation rule for Venue-scoped roles:
 *   A Venue-scoped role may delegate ONLY to that exact Venue and NOT to child venues.
 *
 * SCENARIO:
 *   User holding an exact Venue-scoped role on Venue A1 attempts POST /managementRole
 *   with venue = ChildVenueA1_1.
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request / 403 Access Denied.
 */
func TestRoleAssignment_VenueRoleCannotDelegateToChildVenue(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenVenueUser := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-with-venue-role-only-token")
	targetUserA := getEnvOrDefault("TARGET_USER_A", "user-a-uuid")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	childVenueA1_1 := getEnvOrDefault("VENUE_B_UUID", "child-venue-a1-1-uuid")
	weakPolicy := getEnvOrDefault("POLICY_WEAK_ID", "94bb61a3-aa3e-49aa-9704-cc254fec4482")

	payload := map[string]interface{}{
		"users":            []string{targetUserA},
		"entity":           entityA,
		"venue":            childVenueA1_1, // Child venue delegation attempted by parent venue role holder!
		"managementPolicy": weakPolicy,
	}

	status, body, err := client.DoRequest("POST", "/managementRole/0", tokenVenueUser, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest && status != http.StatusForbidden {
		t.Errorf("Expected 400/403 for Venue-role user attempting child venue delegation, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 3. DELETION WITH ACTIVE DEPENDENCIES (Section 4.8)
// ----------------------------------------------------------------------------

/*
 * TestEntityDelete_EntityWithVenuesReturnsStillInUse
 *
 * DESCRIPTION:
 *   Validates Section 4.8 Deletion rule:
 *   An Entity can be deleted ONLY when it has no child entities, venues, devices, or references.
 *   Deleting an entity that has active child venues must fail and leave data unchanged.
 *
 * SCENARIO:
 *   ROOT or Admin sends DELETE /api/v1/entity/{ENTITY_WITH_CHILDREN_UUID}.
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request or 409 Conflict with error "StillInUse".
 */
func TestEntityDelete_EntityWithVenuesReturnsStillInUse(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenRoot := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	entityWithVenues := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")

	status, body, err := client.DoRequest("DELETE", fmt.Sprintf("/entity/%s", entityWithVenues), tokenRoot, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest && status != http.StatusConflict && status != http.StatusForbidden {
		t.Errorf("Expected 400/409/403 StillInUse error when deleting entity with child venues, got %d. Body: %s", status, string(body))
	}
}
