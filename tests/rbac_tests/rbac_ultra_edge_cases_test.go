package rbac_tests

import (
	"fmt"
	"net/http"
	"testing"
)

// ----------------------------------------------------------------------------
// 1. CYCLE PREVENTION & PARENT VALIDATION (Section 4.5)
// ----------------------------------------------------------------------------

/*
 * TestVenueCreation_SelfReferenceAndCyclePrevention
 *
 * DESCRIPTION:
 *   Validates Section 4.5 Venue placement rule:
 *   Creation/update validation MUST prevent self-reference and cycles (e.g. Venue A parented to Venue A).
 *
 * SCENARIO:
 *   ROOT or Admin sends PUT /api/v1/venue/{VENUE_A_UUID} with parent = VENUE_A_UUID.
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request (Self-reference / cycle detected).
 */
func TestVenueCreation_SelfReferenceAndCyclePrevention(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenRoot := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	venueA := getEnvOrDefault("VENUE_A_UUID", "venue-a-uuid")

	payload := map[string]interface{}{
		"id":     venueA,
		"parent": venueA, // Self-reference cycle!
	}

	status, body, err := client.DoRequest("PUT", fmt.Sprintf("/venue/%s", venueA), tokenRoot, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest {
		t.Errorf("Expected 400 Bad Request for self-referencing venue parent, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 2. ROOT ENTITY DELETION PROTECTION (Section 4.8)
// ----------------------------------------------------------------------------

/*
 * TestOperatorEntityDelete_RootEntityCannotBeDeleted
 *
 * DESCRIPTION:
 *   Validates Section 4.8 Deletion rule:
 *   The Root Entity CANNOT be deleted under any circumstances.
 *
 * SCENARIO:
 *   ROOT sends DELETE /api/v1/entity/{ROOT_ENTITY_UUID}.
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request or 403 Access Denied.
 */
func TestOperatorEntityDelete_RootEntityCannotBeDeleted(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenRoot := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	rootEntityUUID := getEnvOrDefault("ROOT_ENTITY_UUID", "00000000-0000-0000-0000-000000000000")

	status, body, err := client.DoRequest("DELETE", fmt.Sprintf("/entity/%s", rootEntityUUID), tokenRoot, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest && status != http.StatusForbidden {
		t.Errorf("Expected 400/403 for attempting Root Entity deletion, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 3. OPERATOR ENTITY API CREATION RESTRICTION (Section 4.3 & 4.7)
// ----------------------------------------------------------------------------

/*
 * TestEntityCreation_OperatorEntityDisallowedViaGenericEntityAPI
 *
 * DESCRIPTION:
 *   Validates Section 4.3 and Section 4.7 Entity creation rules:
 *   An Operator Entity CANNOT be created through the generic Entity API (POST /api/v1/entity).
 *   Creating an Operator via POST /api/v1/operator creates its Operator Entity atomically.
 *
 * SCENARIO:
 *   ROOT sends POST /api/v1/entity with entityType = "operator".
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request (Operator Entity must be created via Operator API).
 */
func TestEntityCreation_OperatorEntityDisallowedViaGenericEntityAPI(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenRoot := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")

	payload := map[string]interface{}{
		"name":       "Disallowed Operator Entity",
		"entityType": "operator", // Generic entity API cannot create operator entity!
	}

	status, body, err := client.DoRequest("POST", "/entity/0", tokenRoot, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest {
		t.Errorf("Expected 400 Bad Request for creating Operator Entity via generic Entity API, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 4. SUBSCRIBER UPDATE & DELETE PERMISSION CHECKS (Section 11.2)
// ----------------------------------------------------------------------------

/*
 * TestSubscriberUpdate_MissingUpdatePermission
 *
 * DESCRIPTION:
 *   Validates Section 11.2 subscriber update authorization:
 *   Subscriber:UPDATE / Subscriber:DELETE must be enforced through the effective role 
 *   for the Subscriber's resolved Entity or Venue scope.
 *
 * SCENARIO:
 *   User with subscriber:READ policy attempts PUT /api/v1/subscriber/{SUBSCRIBER_ID}.
 *
 * EXPECTED OUTPUT:
 *   HTTP 403 Access Denied.
 */
func TestSubscriberUpdate_MissingUpdatePermission(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenNoAccess := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-token")
	subscriberID := getEnvOrDefault("SUBSCRIBER_ID", "subscriber-uuid-123")

	payload := map[string]interface{}{
		"id":    subscriberID,
		"email": "updated-email@gmail.com",
	}

	status, body, err := client.DoRequest("PUT", fmt.Sprintf("/subscriber/%s", subscriberID), tokenNoAccess, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusForbidden {
		t.Errorf("Expected 403 Access Denied for missing subscriber:UPDATE permission, got %d. Body: %s", status, string(body))
	}
}
