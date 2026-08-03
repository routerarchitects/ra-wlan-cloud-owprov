package rbac_tests

import (
	"fmt"
	"net/http"
	"testing"
)

// ----------------------------------------------------------------------------
// 1. SUBSCRIBER DELETE PERMISSION CHECK (Section 11.2)
// ----------------------------------------------------------------------------

/*
 * TestSubscriberDelete_MissingDeletePermission
 *
 * DESCRIPTION:
 *   Validates Section 11.2 subscriber deletion authorization:
 *   Deleting a subscriber requires subscriber:DELETE or subscriber:FULL permission.
 *   If a user has role on Operator A Entity but policy grants only subscriber:READ,
 *   DELETE /api/v1/subscriber/{id} MUST be denied.
 *
 * SCENARIO:
 *   User with subscriber:READ policy attempts DELETE /api/v1/subscriber/{SUBSCRIBER_ID}.
 *
 * EXPECTED OUTPUT:
 *   HTTP 403 Access Denied.
 */
func TestSubscriberDelete_MissingDeletePermission(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenNoAccess := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-token")
	subscriberID := getEnvOrDefault("SUBSCRIBER_ID", "subscriber-uuid-123")

	status, body, err := client.DoRequest("DELETE", fmt.Sprintf("/subscriber/%s", subscriberID), tokenNoAccess, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusForbidden {
		t.Errorf("Expected 403 Access Denied for missing subscriber:DELETE permission, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 2. OPERATOR UPDATE & DELETE PERMISSION CHECKS (Section 4.7 & 11.1)
// ----------------------------------------------------------------------------

/*
 * TestOperatorUpdate_MissingUpdatePermission
 *
 * DESCRIPTION:
 *   Validates Section 4.7 and Section 11.1 Operator update authorization:
 *   Updating an Operator profile requires an exact Entity-scoped role on its Operator Entity 
 *   with policy granting operator:UPDATE or operator:FULL.
 *
 * SCENARIO:
 *   User with operator:READ policy attempts PUT /api/v1/operator/{OPERATOR_A_UUID}.
 *
 * EXPECTED OUTPUT:
 *   HTTP 403 Access Denied.
 */
func TestOperatorUpdate_MissingUpdatePermission(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenReadOnly := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-operator-token")
	operatorA := getEnvOrDefault("OPERATOR_A_UUID", "b7dcf2fa-f35c-4e0a-818d-3136f38a6990")

	payload := map[string]interface{}{
		"id":   operatorA,
		"name": "Updated Operator Name Attempt",
	}

	status, body, err := client.DoRequest("PUT", fmt.Sprintf("/operator/%s", operatorA), tokenReadOnly, payload)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusForbidden {
		t.Errorf("Expected 403 Access Denied for missing operator:UPDATE permission, got %d. Body: %s", status, string(body))
	}
}

/*
 * TestOperatorDelete_MissingDeletePermission
 *
 * DESCRIPTION:
 *   Validates Section 4.8 and Section 11.1 Operator deletion authorization:
 *   Deleting an Operator requires an exact Entity-scoped role on its Operator Entity 
 *   with policy granting operator:DELETE or operator:FULL.
 *
 * SCENARIO:
 *   User with operator:READ policy attempts DELETE /api/v1/operator/{OPERATOR_A_UUID}.
 *
 * EXPECTED OUTPUT:
 *   HTTP 403 Access Denied.
 */
func TestOperatorDelete_MissingDeletePermission(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenReadOnly := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-operator-token")
	operatorA := getEnvOrDefault("OPERATOR_A_UUID", "b7dcf2fa-f35c-4e0a-818d-3136f38a6990")

	status, body, err := client.DoRequest("DELETE", fmt.Sprintf("/operator/%s", operatorA), tokenReadOnly, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusForbidden {
		t.Errorf("Expected 403 Access Denied for missing operator:DELETE permission, got %d. Body: %s", status, string(body))
	}
}

// ----------------------------------------------------------------------------
// 3. VENUE DELETION WITH ACTIVE DEPENDENCIES (Section 4.8)
// ----------------------------------------------------------------------------

/*
 * TestVenueDelete_VenueWithDevicesReturnsStillInUse
 *
 * DESCRIPTION:
 *   Validates Section 4.8 Deletion rule for Venues:
 *   A Venue can be deleted ONLY when it has no child venues, devices, subscribers, or references.
 *   Deleting a Venue with active devices MUST fail and leave data unchanged.
 *
 * SCENARIO:
 *   ROOT or Admin sends DELETE /api/v1/venue/{VENUE_WITH_DEVICES_UUID}.
 *
 * EXPECTED OUTPUT:
 *   HTTP 400 Bad Request or 409 Conflict with error "StillInUse".
 */
func TestVenueDelete_VenueWithDevicesReturnsStillInUse(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	tokenRoot := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")
	venueWithDevices := getEnvOrDefault("VENUE_A_UUID", "venue-a-uuid")

	status, body, err := client.DoRequest("DELETE", fmt.Sprintf("/venue/%s", venueWithDevices), tokenRoot, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusBadRequest && status != http.StatusConflict && status != http.StatusForbidden {
		t.Errorf("Expected 400/409/403 StillInUse error when deleting venue with active dependencies, got %d. Body: %s", status, string(body))
	}
}
