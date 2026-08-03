package rbac_tests

import (
	"fmt"
	"net/http"
	"testing"
)

// ----------------------------------------------------------------------------
// 2. OPERATOR ISOLATION & VISIBILITY TESTS
// ----------------------------------------------------------------------------

/*
 * TestOperatorIsolation_GetAndList
 *
 * DESCRIPTION:
 *   Validates Operator access rules according to Section 11.1 of the Specification.
 *   Non-root users can access an Operator only if they hold an exact Entity-scoped role
 *   on its Operator Entity with a policy granting Operator:READ.
 *   Root Entity assignment no longer grants blanket access to all operators.
 *
 * SCENARIOS TESTED:
 *   1. Positive: User A with role on Operator A Entity + operator:READ policy calls GET /operator/{uuid_A}.
 *      Expected Status: 200 OK.
 *   2. Negative (Cross-Operator): User A calls GET /operator/{uuid_B} (Operator B).
 *      Expected Status: 403 Access Denied.
 *   3. Negative (Cross-Operator): User B calls GET /operator/{uuid_A} (Operator A).
 *      Expected Status: 403 Access Denied.
 *   4. Negative (Root Entity Privilege Bypass Removed): User assigned to Root Entity (without role on Operator A)
 *      calls GET /operator/{uuid_A}.
 *      Expected Status: 403 Access Denied.
 */
func TestOperatorIsolation_GetAndList(t *testing.T) {
	client := NewTestClient("https://openwifi.wlan.local:16005/api/v1")

	operatorA_UUID := "b7dcf2fa-f35c-4e0a-818d-3136f38a6990"
	operatorB_UUID := "de2434b5-9fcb-4367-a101-8e4da5cfa36f"

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
		rootEntityUserToken := "b6ba0d38b3a438af80a65be2dab666ad95791091d5cfb72264f2a6d1d38e82cb"
		status, _, err := client.DoRequest("GET", fmt.Sprintf("/operator/%s", operatorA_UUID), rootEntityUserToken, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Access Denied (Root entity bypass removed), got %d", status)
		}
	})
}
