package rbac_tests

import (
	"bytes"
	"crypto/tls"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"testing"
)

// Config & Test Harness
type TestClient struct {
	BaseURL string
	Client  *http.Client
}

func NewTestClient(baseURL string) *TestClient {
	tr := &http.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
	}
	return &TestClient{
		BaseURL: baseURL,
		Client:  &http.Client{Transport: tr},
	}
}

func (c *TestClient) DoRequest(method, path, token string, body interface{}) (int, []byte, error) {
	if envToken := os.Getenv("TEST_TOKEN"); envToken != "" {
		token = envToken
	}
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

/*
 * TestScopeResolution_EntityIdInPath
 *
 * DESCRIPTION:
 *   Validates that when an Entity UUID is passed directly in the request URL path,
 *   ResolveTargetContext extracts the Entity UUID and verifies its existence in EntityDB.
 *
 * SCENARIOS TESTED:
 *   1. Positive: Valid Entity UUID in URL path -> TargetEntity is resolved to the Entity UUID.
 *      Expected Status: 200 OK.
 *   2. Negative: Non-existent Entity UUID in URL path -> Target resolution fails.
 *      Expected Status: 404 Not Found or 403 Access Denied.
 */
func TestScopeResolution_EntityIdInPath(t *testing.T) {
	client := NewTestClient("https://openwifi.wlan.local:16005/api/v1")

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

/*
 * TestScopeResolution_OperatorRegistrationIdQuery
 *
 * DESCRIPTION:
 *   Validates target scope resolution when creating subscribers using registrationId query parameter.
 *   ResolveTargetContext queries OperatorDB by registrationId to find Operator record O,
 *   and resolves TargetEntity to O.entityId (the Operator's Entity UUID) to enable role.entity matching.
 *
 * SCENARIOS TESTED:
 *   1. Positive: Non-root user on Operator A Entity sends registrationId=111.
 *      TargetEntity is resolved to Operator A's Entity UUID -> Role entity matches.
 *      Expected Status: 200 OK or 201 Created.
 *   2. Negative: Non-root user on Operator A Entity sends registrationId=222 (Operator B).
 *      TargetEntity is resolved to Operator B's Entity UUID -> Role entity mismatch.
 *      Expected Status: 403 Access Denied.
 */
func TestScopeResolution_OperatorRegistrationIdQuery(t *testing.T) {
	client := NewTestClient("https://openwifi.wlan.local:16005/api/v1")

	t.Run("Positive: Resolve registrationId=111 to Operator Entity UUID", func(t *testing.T) {
		status, _, err := client.DoRequest("POST", "/subscriber?email=test1@gmail.com&registrationId=111&resend=false", "Bearer b6ba0d38b3a438af80a65be2dab666ad95791091d5cfb72264f2a6d1d38e82cb", map[string]string{
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
		status, _, err := client.DoRequest("POST", "/subscriber?email=test2@gmail.com&registrationId=222&resend=false", "Bearer b6ba0d38b3a438af80a65be2dab666ad95791091d5cfb72264f2a6d1d38e82cb", map[string]string{
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
