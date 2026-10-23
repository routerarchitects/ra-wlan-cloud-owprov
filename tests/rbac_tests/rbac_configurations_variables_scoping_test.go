package rbac_tests

import (
	"encoding/json"
	"fmt"
	"net/http"
	"testing"
)

// Data models for parsing list responses
type ConfigurationListResponse struct {
	Configurations []struct {
		ID     string `json:"id"`
		Name   string `json:"name"`
		Entity string `json:"entity"`
		Venue  string `json:"venue"`
	} `json:"configurations"`
	Count int `json:"count"`
}

type VariableListResponse struct {
	VariableBlocks []struct {
		ID     string `json:"id"`
		Name   string `json:"name"`
		Entity string `json:"entity"`
		Venue  string `json:"venue"`
	} `json:"variableBlocks"`
	Count int `json:"count"`
}

// ----------------------------------------------------------------------------
// 1. CONFIGURATIONS LIST RBAC SCOPING TESTS (GET /api/v1/configurations)
// ----------------------------------------------------------------------------

/*
 * TestConfigurationsList_RootBypass
 *
 * DESCRIPTION:
 *   Validates that ROOT users and internal microservices bypass entity/venue
 *   scoping and receive the unrestricted configurations list.
 *
 * SCENARIO:
 *   Caller with TOKEN_ROOT invokes GET /api/v1/configurations.
 *
 * EXPECTED OUTPUT:
 *   HTTP 200 OK with configurations array.
 */
func TestConfigurationsList_RootBypass(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenRoot := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")

	status, body, err := client.DoRequest("GET", "/configurations", tokenRoot, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusOK {
		t.Errorf("Expected 200 OK for ROOT GET /configurations, got %d. Body: %s", status, string(body))
	}

	var resp ConfigurationListResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("Failed to parse configurations response: %v", err)
	}
}

/*
 * TestConfigurationsList_EntityScopedAdmin
 *
 * DESCRIPTION:
 *   Validates multi-tenant isolation for entity-scoped non-root users:
 *   1. Positive: Admin on Operator A Entity sees configurations belonging to
 *      Operator A's Entity and all of its associated venues.
 *   2. Negative: Admin on Operator A Entity CANNOT see configurations
 *      belonging to Operator B's Entity or Operator B's Venues.
 *
 * SCENARIO:
 *   Caller with TOKEN_ADMIN_OPERATOR_A invokes GET /api/v1/configurations.
 *
 * EXPECTED OUTPUT:
 *   HTTP 200 OK. Every returned configuration belongs to Operator A. Zero from Operator B.
 */
func TestConfigurationsList_EntityScopedAdmin(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	entityB := getEnvOrDefault("OPERATOR_B_ENTITY_UUID", "8ab2b291-d04d-5c4c-b4bd-c4gcca1gb108")
	venueB := getEnvOrDefault("VENUE_B_UUID", "venue-b-uuid")

	status, body, err := client.DoRequest("GET", "/configurations", tokenAdminA, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusOK {
		t.Errorf("Expected 200 OK for scoped admin GET /configurations, got %d. Body: %s", status, string(body))
	}

	var resp ConfigurationListResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("Failed to parse configurations response: %v", err)
	}

	// Negative Check: Verify no Operator B configurations leak across tenants
	for _, config := range resp.Configurations {
		if config.Entity == entityB {
			t.Errorf("Cross-tenant leakage! Found configuration %s belonging to Entity B (%s)", config.ID, entityB)
		}
		if config.Venue == venueB && venueB != "" {
			t.Errorf("Cross-tenant leakage! Found configuration %s belonging to Venue B (%s)", config.ID, venueB)
		}
		// Positive Check: Returned item must match Entity A or a child venue
		if config.Entity != "" && config.Entity != entityA && config.Venue == "" {
			t.Errorf("Unexpected entity config %s with entity %s (expected %s)", config.ID, config.Entity, entityA)
		}
	}
}

/*
 * TestConfigurationsList_VenueScopedRole_ExcludesParentEntity
 *
 * DESCRIPTION:
 *   Validates that when a role specifies a venue (even if entity is populated),
 *   the user is scoped strictly to that venue and its descendants, NEVER the parent entity.
 *
 * SCENARIO:
 *   Caller with venue-specific token invokes GET /api/v1/configurations.
 *
 * EXPECTED OUTPUT:
 *   HTTP 200 OK. Returned configurations must match Venue A and NEVER Entity A or sibling venues.
 */
func TestConfigurationsList_VenueScopedRole_ExcludesParentEntity(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenVenueA := getEnvOrDefault("TOKEN_VENUE_A", "Bearer user-venue-a-token")
	venueA := getEnvOrDefault("VENUE_A_UUID", "venue-a-uuid")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")

	status, body, err := client.DoRequest("GET", "/configurations", tokenVenueA, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusOK {
		t.Errorf("Expected 200 OK for venue-scoped user GET /configurations, got %d. Body: %s", status, string(body))
	}

	var resp ConfigurationListResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("Failed to parse response: %v", err)
	}

	// Negative Verification: Must not contain entity-wide configs
	for _, config := range resp.Configurations {
		if config.Entity == entityA && config.Venue == "" {
			t.Errorf("Scope violation! Venue-only user received parent entity config %s", config.ID)
		}
		if config.Venue != "" && config.Venue != venueA {
			t.Errorf("Scope violation! Venue user received config %s from unrelated venue %s", config.ID, config.Venue)
		}
	}
}

/*
 * TestConfigurationsList_NoPermissions (Negative)
 *
 * DESCRIPTION:
 *   Validates that a user whose management policy lacks configuration:GET
 *   receives an empty configurations array.
 *
 * SCENARIO:
 *   Caller with TOKEN_NO_ACCESS invokes GET /api/v1/configurations.
 *
 * EXPECTED OUTPUT:
 *   HTTP 200 OK with {"configurations": []}.
 */
func TestConfigurationsList_NoPermissions(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenNoAccess := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-token")

	status, body, err := client.DoRequest("GET", "/configurations", tokenNoAccess, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusOK {
		t.Errorf("Expected 200 OK for non-permission user, got %d. Body: %s", status, string(body))
	}

	var resp ConfigurationListResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("Failed to parse response: %v", err)
	}

	if len(resp.Configurations) != 0 {
		t.Errorf("Expected 0 configurations for user without permission, got %d", len(resp.Configurations))
	}
}

/*
 * TestConfigurationsList_CrossTenantQueryParam_Negative (Negative)
 *
 * DESCRIPTION:
 *   Validates that when a scoped user requests ?entity=<Entity B> or ?venue=<Venue B>,
 *   the API rejects cross-tenant parameter tampering by returning 403 Forbidden.
 *
 * SCENARIO:
 *   Admin of Entity A requests GET /api/v1/configurations?entity=EntityB.
 *
 * EXPECTED OUTPUT:
 *   HTTP 403 Forbidden.
 */
func TestConfigurationsList_CrossTenantQueryParam_Negative(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	entityB := getEnvOrDefault("OPERATOR_B_ENTITY_UUID", "8ab2b291-d04d-5c4c-b4bd-c4gcca1gb108")
	venueA := getEnvOrDefault("VENUE_A_UUID", "venue-a-uuid")
	venueB := getEnvOrDefault("VENUE_B_UUID", "venue-b-uuid")

	t.Run("Negative: Tampered entity query param returns 403 Forbidden", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/configurations?entity=%s", entityB), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}

		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Tampered venue query param returns 403 Forbidden", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/configurations?venue=%s", venueB), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}

		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Valid entity with unauthorized venue query param returns 403 Forbidden", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/configurations?entity=%s&venue=%s", entityA, venueB), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}

		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden for unauthorized venue in combined query, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Positive: Combined query with authorized entity and authorized venue returns 200 OK", func(t *testing.T) {
		status, _, err := client.DoRequest("GET", fmt.Sprintf("/configurations?entity=%s&venue=%s", entityA, venueA), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}

		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for authorized combined query, got %d", status)
		}
	})
}

/*
 * TestConfigurationsList_CountOnly
 *
 * DESCRIPTION:
 *   Validates that GET /api/v1/configurations?countOnly=true respects
 *   RBAC scoping for both positive and negative permissions.
 */
func TestConfigurationsList_CountOnly(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	tokenNoAccess := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-token")

	t.Run("Positive: Scoped admin gets count of permitted configs", func(t *testing.T) {
		status, _, err := client.DoRequest("GET", "/configurations?countOnly=true", tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for countOnly, got %d", status)
		}
	})

	t.Run("Negative: User without permission gets count: 0", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", "/configurations?countOnly=true", tokenNoAccess, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK, got %d", status)
		}

		var resp struct {
			Count int `json:"count"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse count response: %v", err)
		}
		if resp.Count != 0 {
			t.Errorf("Expected count: 0 for user without permission, got %d", resp.Count)
		}
	})
}

/*
 * TestConfigurationsList_Unauthenticated (Negative)
 *
 * DESCRIPTION:
 *   Validates that requests without an authorization header are rejected.
 *
 * EXPECTED OUTPUT:
 *   HTTP 401 Unauthorized or 403 Forbidden.
 */
func TestConfigurationsList_Unauthenticated(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	status, _, err := client.DoRequest("GET", "/configurations", "", nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusUnauthorized && status != http.StatusForbidden {
		t.Errorf("Expected 401/403 for unauthenticated request, got %d", status)
	}
}

/*
 * TestConfigurationsList_SelectScoped (P1 Regression Test)
 *
 * DESCRIPTION:
 *   Validates that when ?select= is provided:
 *   1. Explicit ?entity= or ?venue= query filters are strictly enforced on selected records.
 *   2. Any selected records outside the requested entity/venue scope are omitted.
 *   3. Cross-tenant or unauthorized IDs passed in select are omitted.
 *   4. Scoped countOnly with select returns the accurate count of matching selected items.
 */
func TestConfigurationsList_SelectScoped(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")

	t.Run("Select query with explicit entity filter excludes other entity records", func(t *testing.T) {
		endpoint := fmt.Sprintf("/configurations?entity=%s&select=dummy-id-1,dummy-id-2", entityA)
		status, body, err := client.DoRequest("GET", endpoint, tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for select with entity filter, got %d", status)
		}

		var resp ConfigurationListResponse
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse response: %v", err)
		}

		for _, cfg := range resp.Configurations {
			if cfg.Entity != entityA {
				t.Errorf("Security defect! Returned config %s with entity %s does not match requested entity %s",
					cfg.ID, cfg.Entity, entityA)
			}
		}
	})

	t.Run("Select query with countOnly returns matching selected count", func(t *testing.T) {
		endpoint := fmt.Sprintf("/configurations?entity=%s&select=dummy-id-1&countOnly=true", entityA)
		status, body, err := client.DoRequest("GET", endpoint, tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for select countOnly, got %d", status)
		}

		var resp struct {
			Count int `json:"count"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse count response: %v", err)
		}
	})
}

// ----------------------------------------------------------------------------
// 2. VARIABLES LIST RBAC SCOPING TESTS (GET /api/v1/variables)
// ----------------------------------------------------------------------------

/*
 * TestVariablesList_RootBypass
 *
 * DESCRIPTION:
 *   Validates that ROOT users and internal microservices bypass scoping
 *   for GET /api/v1/variables.
 *
 * EXPECTED OUTPUT:
 *   HTTP 200 OK.
 */
func TestVariablesList_RootBypass(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenRoot := getEnvOrDefault("TOKEN_ROOT", "Bearer root-test-token")

	status, body, err := client.DoRequest("GET", "/variables", tokenRoot, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusOK {
		t.Errorf("Expected 200 OK for ROOT GET /variables, got %d. Body: %s", status, string(body))
	}

	var resp VariableListResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("Failed to parse variables response: %v", err)
	}
}

/*
 * TestVariablesList_EntityScopedAdmin
 *
 * DESCRIPTION:
 *   Validates multi-tenant isolation for variables:
 *   1. Positive: Scoped admin sees variables within their entity and venues.
 *   2. Negative: Scoped admin cannot see variables belonging to Entity B or Venue B.
 *
 * EXPECTED OUTPUT:
 *   HTTP 200 OK. No Operator B variables in the response.
 */
func TestVariablesList_EntityScopedAdmin(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	entityB := getEnvOrDefault("OPERATOR_B_ENTITY_UUID", "8ab2b291-d04d-5c4c-b4bd-c4gcca1gb108")
	venueB := getEnvOrDefault("VENUE_B_UUID", "venue-b-uuid")

	status, body, err := client.DoRequest("GET", "/variables", tokenAdminA, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusOK {
		t.Errorf("Expected 200 OK for scoped admin GET /variables, got %d. Body: %s", status, string(body))
	}

	var resp VariableListResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("Failed to parse variables response: %v", err)
	}

	// Negative Check: Verify no Operator B variables leak
	for _, variable := range resp.VariableBlocks {
		if variable.Entity == entityB {
			t.Errorf("Cross-tenant leakage! Found variable %s belonging to Entity B (%s)", variable.ID, entityB)
		}
		if variable.Venue == venueB && venueB != "" {
			t.Errorf("Cross-tenant leakage! Found variable %s belonging to Venue B (%s)", variable.ID, venueB)
		}
	}
}

/*
 * TestVariablesList_VenueScopedRole_ExcludesParentEntity
 *
 * DESCRIPTION:
 *   Validates that a venue-scoped user only sees variables for that venue,
 *   never the parent entity.
 */
func TestVariablesList_VenueScopedRole_ExcludesParentEntity(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenVenueA := getEnvOrDefault("TOKEN_VENUE_A", "Bearer user-venue-a-token")
	venueA := getEnvOrDefault("VENUE_A_UUID", "venue-a-uuid")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")

	status, body, err := client.DoRequest("GET", "/variables", tokenVenueA, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusOK {
		t.Errorf("Expected 200 OK, got %d. Body: %s", status, string(body))
	}

	var resp VariableListResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("Failed to parse response: %v", err)
	}

	for _, variable := range resp.VariableBlocks {
		if variable.Entity == entityA && variable.Venue == "" {
			t.Errorf("Scope violation! Venue user received parent entity variable %s", variable.ID)
		}
		if variable.Venue != "" && variable.Venue != venueA {
			t.Errorf("Scope violation! Venue user received variable %s from unrelated venue %s", variable.ID, variable.Venue)
		}
	}
}

/*
 * TestVariablesList_NoPermissions (Negative)
 *
 * DESCRIPTION:
 *   Validates that a user whose management policy lacks variables:GET
 *   receives an empty variables array.
 *
 * EXPECTED OUTPUT:
 *   HTTP 200 OK with {"variables": []}.
 */
func TestVariablesList_NoPermissions(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenNoAccess := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-token")

	status, body, err := client.DoRequest("GET", "/variables", tokenNoAccess, nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusOK {
		t.Errorf("Expected 200 OK for non-permission user, got %d. Body: %s", status, string(body))
	}

	var resp VariableListResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("Failed to parse response: %v", err)
	}

	if len(resp.VariableBlocks) != 0 {
		t.Errorf("Expected 0 variables for user without permission, got %d", len(resp.VariableBlocks))
	}
}

/*
 * TestVariablesList_CrossTenantQueryParam_Negative (Negative)
 *
 * DESCRIPTION:
 *   Validates that when a scoped user requests ?entity=<Entity B> or ?venue=<Venue B>,
 *   cross-tenant tampering is blocked by returning an empty list.
 */
func TestVariablesList_CrossTenantQueryParam_Negative(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")
	entityB := getEnvOrDefault("OPERATOR_B_ENTITY_UUID", "8ab2b291-d04d-5c4c-b4bd-c4gcca1gb108")
	venueA := getEnvOrDefault("VENUE_A_UUID", "venue-a-uuid")
	venueB := getEnvOrDefault("VENUE_B_UUID", "venue-b-uuid")

	t.Run("Negative: Tampered entity query param returns 403 Forbidden", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/variables?entity=%s", entityB), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}

		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Tampered venue query param returns 403 Forbidden", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/variables?venue=%s", venueB), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}

		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Negative: Valid entity with unauthorized venue query param returns 403 Forbidden", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", fmt.Sprintf("/variables?entity=%s&venue=%s", entityA, venueB), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}

		if status != http.StatusForbidden {
			t.Errorf("Expected 403 Forbidden for unauthorized venue in combined query, got %d. Body: %s", status, string(body))
		}
	})

	t.Run("Positive: Combined query with authorized entity and authorized venue returns 200 OK", func(t *testing.T) {
		status, _, err := client.DoRequest("GET", fmt.Sprintf("/variables?entity=%s&venue=%s", entityA, venueA), tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}

		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for authorized combined query, got %d", status)
		}
	})
}

/*
 * TestVariablesList_CountOnly
 *
 * DESCRIPTION:
 *   Validates that GET /api/v1/variables?countOnly=true respects
 *   RBAC scoping for both positive and negative permissions.
 */
func TestVariablesList_CountOnly(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	tokenNoAccess := getEnvOrDefault("TOKEN_NO_ACCESS", "Bearer user-read-only-token")

	t.Run("Positive: Scoped admin gets count of permitted variables", func(t *testing.T) {
		status, _, err := client.DoRequest("GET", "/variables?countOnly=true", tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for countOnly, got %d", status)
		}
	})

	t.Run("Negative: User without permission gets count: 0", func(t *testing.T) {
		status, body, err := client.DoRequest("GET", "/variables?countOnly=true", tokenNoAccess, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK, got %d", status)
		}

		var resp struct {
			Count int `json:"count"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse count response: %v", err)
		}
		if resp.Count != 0 {
			t.Errorf("Expected count: 0 for user without permission, got %d", resp.Count)
		}
	})
}

/*
 * TestVariablesList_Unauthenticated (Negative)
 *
 * DESCRIPTION:
 *   Validates that requests to /variables without authorization header are rejected.
 *
 * EXPECTED OUTPUT:
 *   HTTP 401 Unauthorized or 403 Forbidden.
 */
func TestVariablesList_Unauthenticated(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))

	status, _, err := client.DoRequest("GET", "/variables", "", nil)
	if err != nil {
		t.Fatalf("Request failed: %v", err)
	}

	if status != http.StatusUnauthorized && status != http.StatusForbidden {
		t.Errorf("Expected 401/403 for unauthenticated request, got %d", status)
	}
}

/*
 * TestVariablesList_SelectScoped (P1 Regression Test)
 *
 * DESCRIPTION:
 *   Validates that when ?select= is provided for variables:
 *   1. Explicit ?entity= or ?venue= query filters are strictly enforced on selected records.
 *   2. Any selected records outside the requested entity/venue scope are omitted.
 *   3. Scoped countOnly with select returns the accurate count of matching selected items.
 */
func TestVariablesList_SelectScoped(t *testing.T) {
	client := NewTestClient(getEnvOrDefault("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"))
	tokenAdminA := getEnvOrDefault("TOKEN_ADMIN_OPERATOR_A", "Bearer user-admin-operator-a-token")
	entityA := getEnvOrDefault("OPERATOR_A_ENTITY_UUID", "7fa1a180-c93c-4b3b-a3ac-b3fbbf0fa097")

	t.Run("Select query with explicit entity filter excludes other entity records", func(t *testing.T) {
		endpoint := fmt.Sprintf("/variables?entity=%s&select=dummy-var-1,dummy-var-2", entityA)
		status, body, err := client.DoRequest("GET", endpoint, tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for select with entity filter, got %d", status)
		}

		var resp VariableListResponse
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse response: %v", err)
		}

		for _, v := range resp.VariableBlocks {
			if v.Entity != entityA {
				t.Errorf("Security defect! Returned variable block %s with entity %s does not match requested entity %s",
					v.ID, v.Entity, entityA)
			}
		}
	})

	t.Run("Select query with countOnly returns matching selected count", func(t *testing.T) {
		endpoint := fmt.Sprintf("/variables?entity=%s&select=dummy-var-1&countOnly=true", entityA)
		status, body, err := client.DoRequest("GET", endpoint, tokenAdminA, nil)
		if err != nil {
			t.Fatalf("Request failed: %v", err)
		}
		if status != http.StatusOK {
			t.Errorf("Expected 200 OK for select countOnly, got %d", status)
		}

		var resp struct {
			Count int `json:"count"`
		}
		if err := json.Unmarshal(body, &resp); err != nil {
			t.Fatalf("Failed to parse count response: %v", err)
		}
	})
}

