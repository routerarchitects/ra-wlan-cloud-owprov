package subscriber_rbac

import (
	"bytes"
	"crypto/tls"
	"encoding/json"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"
	"testing"
	"time"
)

type fixture struct {
	base, token, id string
	client          *http.Client
}

func setup(t *testing.T) fixture {
	t.Helper()
	f := fixture{base: strings.TrimRight(os.Getenv("OWPROV_SUBSCRIBER_URL"), "/"),
		token: os.Getenv("SUBSCRIBER_TOKEN"), id: os.Getenv("SUBSCRIBER_ID")}
	if f.base == "" || f.token == "" || f.id == "" {
		t.Skip("set OWPROV_SUBSCRIBER_URL (including /api/v1), SUBSCRIBER_TOKEN and SUBSCRIBER_ID")
	}
	transport := http.DefaultTransport.(*http.Transport).Clone()
	transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: os.Getenv("OWPROV_TEST_INSECURE_TLS") == "1"}
	f.client = &http.Client{Transport: transport, Timeout: 120 * time.Second,
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	t.Cleanup(transport.CloseIdleConnections)
	return f
}

func (f fixture) request(t *testing.T, method, path string, payload any, expected int) []byte {
	t.Helper()
	var body io.Reader
	if payload != nil {
		data, err := json.Marshal(payload)
		if err != nil {
			t.Fatal(err)
		}
		body = bytes.NewReader(data)
	}
	req, err := http.NewRequest(method, f.base+path, body)
	if err != nil {
		t.Fatal(err)
	}
	token := f.token
	if !strings.HasPrefix(token, "Bearer ") {
		token = "Bearer " + token
	}
	req.Header.Set("Authorization", token)
	req.Header.Set("Content-Type", "application/json")
	resp, err := f.client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != expected {
		t.Fatalf("%s %s: expected %d, got %d: %s", method, path, expected, resp.StatusCode, data)
	}
	return data
}

func TestSubscriberCollection(t *testing.T) {
	f := setup(t)
	for _, query := range []string{"", "?operatorId=nonexistent", "?subscriberId=" + url.QueryEscape(f.id), "?limit=1&offset=1"} {
		data := f.request(t, "GET", "/subscriberDevice"+query, nil, http.StatusOK)
		var list struct {
			Devices []struct {
				SubscriberID string `json:"subscriberId"`
			} `json:"subscriberDevices"`
		}
		if err := json.Unmarshal(data, &list); err != nil {
			t.Fatal(err)
		}
		var object map[string]json.RawMessage
		if err := json.Unmarshal(data, &object); err != nil {
			t.Fatal(err)
		}
		if _, ok := object["subscriberDevices"]; !ok {
			t.Fatal("missing subscriberDevices collection")
		}
		for _, device := range list.Devices {
			if device.SubscriberID != f.id {
				t.Fatalf("foreign device in subscriber collection: %s", data)
			}
		}
	}
	for _, id := range []string{"", "another-subscriber"} {
		f.request(t, "GET", "/subscriberDevice?subscriberId="+id, nil, http.StatusForbidden)
	}
}

func TestSubscriberDeviceOwnership(t *testing.T) {
	f := setup(t)
	owned, foreign := os.Getenv("SUBSCRIBER_DEVICE_ID"), os.Getenv("FOREIGN_SUBSCRIBER_DEVICE_ID")
	if owned == "" || foreign == "" {
		t.Skip("set owned SUBSCRIBER_DEVICE_ID and FOREIGN_SUBSCRIBER_DEVICE_ID")
	}
	f.request(t, "GET", "/subscriberDevice/"+url.PathEscape(owned), nil, http.StatusOK)
	missing := "/subscriberDevice/00000000-0000-4000-8000-000000000099"
	for _, method := range []string{"GET", "DELETE"} {
		foreignBody := f.request(t, method, "/subscriberDevice/"+url.PathEscape(foreign), nil, http.StatusNotFound)
		missingBody := f.request(t, method, missing, nil, http.StatusNotFound)
		if !bytes.Equal(foreignBody, missingBody) {
			t.Fatalf("%s foreign/missing response bodies differ", method)
		}
	}
}

func TestSubscriberDeniedOperations(t *testing.T) {
	f := setup(t)
	for _, tc := range []struct{ method, path string }{
		{"PUT", "/subscriberDevice/aabbccddeeff"},
		{"POST", "/subscriberDevice/new"},
		{"POST", "/subscriberDevice/aabbccddeeff"},
		{"POST", "/subscriber"},
		{"DELETE", "/subscriber/another-subscriber"},
		{"GET", "/configuration/00000000-0000-4000-8000-000000000099"},
		{"GET", "/configuration/00000000-0000-4000-8000-000000000099?next=/api/v1/subscriberDevice"},
	} {
		f.request(t, tc.method, tc.path, map[string]any{}, http.StatusForbidden)
	}
	for _, payload := range []map[string]any{
		{"subscriberId": "another-subscriber"},
		{"subscriberId": ""},
		{"managementPolicy": "some-policy"},
		{"deviceConfiguration": "00000000-0000-4000-8000-000000000099"},
	} {
		f.request(t, "POST", "/subscriberDevice/0", payload, http.StatusForbidden)
	}
	f.request(t, "POST", "/subscriberDevice/0", map[string]any{"subscriberId": 123}, http.StatusBadRequest)
}

func TestInternalRegistrationRouting(t *testing.T) {
	f := setup(t)
	base := strings.TrimRight(os.Getenv("OWPROV_INTERNAL_URL"), "/")
	key, name := os.Getenv("OWPROV_INTERNAL_KEY"), os.Getenv("OWPROV_INTERNAL_NAME")
	if base == "" || key == "" || name == "" {
		t.Skip("set OWPROV_INTERNAL_URL (including /api/v1), OWPROV_INTERNAL_KEY and OWPROV_INTERNAL_NAME")
	}
	for _, tc := range []struct {
		label, key, name string
		status           int
	}{
		{"invalid key", "invalid-test-key", name, http.StatusForbidden},
		{"missing key", "", name, http.StatusForbidden},
		{"missing name", key, "", http.StatusForbidden},
		// Valid service credentials must reach the existing signup handler. Missing
		// signup parameters deliberately produce 400 without registering an account.
		{"existing signup handler", key, name, http.StatusBadRequest},
	} {
		t.Run(tc.label, func(t *testing.T) {
			req, err := http.NewRequest("POST", base+"/subscriber", strings.NewReader("{}"))
			if err != nil {
				t.Fatal(err)
			}
			token := f.token
			if !strings.HasPrefix(token, "Bearer ") {
				token = "Bearer " + token
			}
			req.Header.Set("Authorization", token)
			req.Header.Set("Content-Type", "application/json")
			if tc.key != "" {
				req.Header.Set("X-API-KEY", tc.key)
			}
			if tc.name != "" {
				req.Header.Set("X-INTERNAL-NAME", tc.name)
			}
			resp, err := f.client.Do(req)
			if err != nil {
				t.Fatal(err)
			}
			defer resp.Body.Close()
			if resp.StatusCode != tc.status {
				t.Fatalf("expected %d, got %d", tc.status, resp.StatusCode)
			}
		})
	}
}

// Explicit opt-in: the serial must be an available inventory device dedicated to
// this test. Unclaim runs even if an assertion after successful creation fails.
func TestSubscriberClaimUnclaim(t *testing.T) {
	f := setup(t)
	serial := os.Getenv("SUBSCRIBER_CLAIM_SERIAL")
	if serial == "" {
		t.Skip("set SUBSCRIBER_CLAIM_SERIAL for a disposable inventory device")
	}
	group := os.Getenv("SUBSCRIBER_CLAIM_GROUP")
	if group == "" {
		group = "olg"
	}
	var configuration any
	if raw := os.Getenv("SUBSCRIBER_CLAIM_CONFIGURATION"); raw != "" {
		if err := json.Unmarshal([]byte(raw), &configuration); err != nil {
			t.Fatal(err)
		}
	}
	payload := map[string]any{"name": "subscriber-rbac-test", "serialNumber": serial,
		"operatorId": "nonexistent", "deviceGroup": group}
	if configuration != nil {
		payload["configuration"] = configuration
	}
	data := f.request(t, "POST", "/subscriberDevice/0", payload, http.StatusOK)
	t.Cleanup(func() { f.request(t, "DELETE", "/subscriberDevice/"+url.PathEscape(serial), nil, http.StatusOK) })
	var device struct {
		SubscriberID string `json:"subscriberId"`
	}
	if err := json.Unmarshal(data, &device); err != nil {
		t.Fatal(err)
	}
	if device.SubscriberID != f.id {
		t.Fatalf("claim owner is %q; expected %q", device.SubscriberID, f.id)
	}
	f.request(t, "GET", "/subscriberDevice/"+url.PathEscape(serial), nil, http.StatusOK)
	// An account with a claimed device must remain intact.
	f.request(t, "DELETE", "/subscriber/"+url.PathEscape(f.id), nil, http.StatusBadRequest)
}

func TestSubscriberAccountDeletion(t *testing.T) {
	f := setup(t)
	if os.Getenv("SUBSCRIBER_DELETE_ACCOUNT") != "1" {
		t.Skip("set SUBSCRIBER_DELETE_ACCOUNT=1 only for a disposable empty account")
	}
	f.request(t, "DELETE", "/subscriber/"+url.PathEscape(f.id), nil, http.StatusOK)
}
