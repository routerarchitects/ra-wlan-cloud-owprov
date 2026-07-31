package rbac_tests

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"testing"
)

// Config & Test Harness
type TestClient struct {
	BaseURL string
	Client  *http.Client
}

func (c *TestClient) DoRequest(method, path, token string, body interface{}) (int, []byte, error) {
	var bodyReader io.Reader
	if body != nil {
		jsonBytes, err := json.Marshal(body)
		if err != nil {
			return 0, nil, err
		}
		bodyReader = bytes.NewBuffer(jsonBytes)
	}

	req, err := http.NewRequest(method, c.BaseURL+path, bodyReader)
	if err != nil {
		return 0, nil, err
	}

	req.Header.Set("Authorization", token)
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.Client.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(resp.Body)
	return resp.StatusCode, respBody, err
}

// ----------------------------------------------------------------------------
// 1. SCOPE RESOLUTION TESTS (ResolveTargetContext)
// ----------------------------------------------------------------------------

func TestScopeResolution_EntityIdInPath(t *testing.T) {
	client := &TestClient{BaseURL: "https://openwifi.wlan.local:16005/api/v1", Client: http.DefaultClient}

	t.Run("Positive: Resolve Valid Entity UUID in Path", func(t *testing.T) {
		status, _, err := client.DoRequest("GET", "/entity/7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097", "Bearer user-admin-token", nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for valid entity path, got %d", status)
		}
	})

	t.Run("Negative: Unresolvable Entity UUID in Path", func(t *testing.T) {
		status, _, err := client.DoRequest("GET", "/entity/00000000-0000-0000-0000-000000000000", "Bearer user-admin-token", nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusNotFound && status != http.StatusForbidden {
			t.Errorf("Expected 404/403 for non-existent entity, got %d", status)
		}
	})
}

func TestScopeResolution_OperatorRegistrationIdQuery(t *testing.T) {
	client := &TestClient{BaseURL: "https://openwifi.wlan.local:16005/api/v1", Client: http.DefaultClient}

	t.Run("Positive: Resolve registrationId=111 to Operator Entity UUID", func(t *testing.T) {
		status, _, err := client.DoRequest("POST", "/subscriber?email=test1@gmail.com&registrationId=111&resend=false", "Bearer user-admin-operator-a-token", map[string]string{
			"email": "test1@gmail.com",
		})
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK && status != http.StatusCreated {
			t.Errorf("Expected 200/201 for valid registrationId target resolution, got %d", status)
		}
	})

	t.Run("Negative: Cross-Operator registrationId Resolution Blocked", func(t *testing.T) {
		status, _, err := client.DoRequest("POST", "/subscriber?email=test2@gmail.com&registrationId=222&resend=false", "Bearer user-admin-operator-a-token", map[string]string{
			"email": "test2@gmail.com",
		})
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Access Denied for cross-operator registrationId, got %d", status)
		}
	})
}
