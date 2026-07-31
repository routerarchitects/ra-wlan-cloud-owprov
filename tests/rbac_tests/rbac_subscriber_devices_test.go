package rbac_tests

import (
	"fmt"
	"net/http"
	"testing"
)

// ----------------------------------------------------------------------------
// 4. SUBSCRIBER DEVICE & INVENTORY PERMISSION TESTS
// ----------------------------------------------------------------------------

func TestSubscriberDevicePermissions(t *testing.T) {
	client := &TestClient{BaseURL: "https://openwifi.wlan.local:16005/api/v1", Client: http.DefaultClient}

	operatorA_UUID := "b7dcf2fa-f35c-4e0a-818d-3136f38a6990"
	operatorB_UUID := "c8edf3ab-a46d-5f1b-929e-4247g49b7001"

	userWithInventoryToken := "Bearer user-admin-operator-a-token"
	userWithoutInventoryToken := "Bearer user-subscriber-only-token"

	t.Run("Positive: Query subscriber devices with inventory:READ permission", func(t *testing.T) {
		status, _, err := client.DoRequest("GET", fmt.Sprintf("/subscriberDevice?withExtendedInfo=true&operatorId=%s", operatorA_UUID), userWithInventoryToken, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for subscriber device query with inventory access, got %d", status)
		}
	})

	t.Run("Negative: Query subscriber devices with subscriber:FULL policy but missing inventory permission", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/subscriberDevice?withExtendedInfo=true&operatorId=%s", operatorA_UUID), userWithoutInventoryToken, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Access Denied when inventory permission is absent, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Cross-Operator subscriber device query", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/subscriberDevice?withExtendedInfo=true&operatorId=%s", operatorB_UUID), userWithInventoryToken, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Access Denied for cross-operator device list, got %d. Body: %s", status, string(body))
		}
	})
}
