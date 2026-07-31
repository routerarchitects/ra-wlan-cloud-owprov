package rbac_tests

import (
	"fmt"
	"net/http"
	"testing"
)

// ----------------------------------------------------------------------------
// 2. OPERATOR ISOLATION & VISIBILITY TESTS
// ----------------------------------------------------------------------------

func TestOperatorIsolation_GetAndList(t *testing.T) {
	client := &TestClient{BaseURL: "https://openwifi.wlan.local:16005/api/v1", Client: http.DefaultClient}

	operatorA_UUID := "b7dcf2fa-f35c-4e0a-818d-3136f38a6990"
	operatorB_UUID := "c8edf3ab-a46d-5f1b-929e-4247g49b7001"

	userA_Token := "Bearer user-admin-operator-a-token"
	userB_Token := "Bearer user-admin-operator-b-token"

	t.Run("Positive: User A GET own Operator A", func(t *testing.T) {
		status, _, err := client.DoRequest("GET", fmt.Sprintf("/operator/%s", operatorA_UUID), userA_Token, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for own operator GET, got %d", status)
		}
	})

	t.Run("Negative: User A GET Operator B (Cross-Operator Isolation)", func(t *testing.T) {
		status, _, err := client.DoRequest("GET", fmt.Sprintf("/operator/%s", operatorB_UUID), userA_Token, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Access Denied for cross-operator GET, got %d", status)
		}
	})

	t.Run("Negative: User B GET Operator A (Cross-Operator Isolation)", func(t *testing.T) {
		status, _, err := client.DoRequest("GET", fmt.Sprintf("/operator/%s", operatorA_UUID), userB_Token, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Access Denied for cross-operator GET, got %d", status)
		}
	})

	t.Run("Negative: User assigned to Root Entity without operator:READ policy", func(t *testing.T) {
		rootEntityUserToken := "Bearer root-entity-user-no-operator-read-token"
		status, _, err := client.DoRequest("GET", fmt.Sprintf("/operator/%s", operatorA_UUID), rootEntityUserToken, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Access Denied (Root entity bypass removed), got %d", status)
		}
	})
}
