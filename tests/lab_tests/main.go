package main

import (
	"bufio"
	"bytes"
	"crypto/tls"
	"encoding/csv"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"regexp"
	"strings"
	"time"
)

// ─── Structs ────────────────────────────────────────────────────────────────

type TestCase struct {
	ID             string
	Category       string
	Scenario       string
	Method         string
	Endpoint       string
	RequestBody    string
	ExpectedStatus string
	Description    string
}

var KnownFailures = map[string]string{}

// ─── Main ───────────────────────────────────────────────────────────────────

func main() {
	fmt.Println("=== Starting Hierarchical RBAC v2.1 Lab Test Runner ===")
	loadEnvFile(".env")

	owsecBase := getEnv("OWSEC_BASE_URL", "https://localhost:16001/api/v1")
	owprovBase := getEnv("OWPROV_BASE_URL", "https://localhost:16005/api/v1")
	rootEmail := getEnv("OWSEC_ROOT_EMAIL", "tip@ucentral.com")

	defaultRootPw := getEnv("OWSEC_ROOT_DEFAULT_PASSWORD", "openwifi")
	targetRootPw := getEnv("OWSEC_ROOT_PASSWORD", "Iotina@123")

	tr := &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
	client := &http.Client{Transport: tr, Timeout: 15 * time.Second}

	// Automatically handle first-boot password change if root password is still 'openwifi'
	if err := ensureRootPasswordChanged(client, owsecBase, rootEmail, defaultRootPw, targetRootPw); err != nil {
		fmt.Printf("[Bootstrap Warning] Root password initialization check: %v\n", err)
	}

	rootPasswords := []string{targetRootPw, "Iotina@1234"}

	var rootToken string
	var loginErr error
	for _, pw := range rootPasswords {
		fmt.Printf("Attempting ROOT login using password: %s...\n", pw)
		rootToken, loginErr = login(client, owsecBase, rootEmail, pw)
		if loginErr == nil {
			fmt.Println("ROOT Login Successful!")
			break
		}
	}
	if loginErr != nil {
		fmt.Fprintf(os.Stderr, "Fatal: ROOT Login failed: %v\n", loginErr)
		os.Exit(1)
	}

	// ── Pre-cleanup: make the test run idempotent ──────────────────────────
	fmt.Println("\n=== Pre-cleanup: removing any leftover test data from previous runs ===")
	preCleanup(client, owsecBase, owprovBase, rootToken)
	fmt.Printf("=== Pre-cleanup complete ===\n\n")

	// ── Load CSV ───────────────────────────────────────────────────────────
	csvEnv := getEnv("TEST_CSV", "")
	var csvFiles []string
	if csvEnv != "" {
		csvFiles = []string{csvEnv}
	} else {
		csvFiles = []string{
			"owprov_rbac_v2_1_root_lab_test_cases.csv",
			"owprov_rbac_v2_1_admin_lab_test_cases.csv",
		}
	}

	var testCases []TestCase
	for _, csvPath := range csvFiles {
		fmt.Printf("Loading test suite CSV: %s\n", csvPath)
		file, err := os.Open(csvPath)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Warning: Failed to open CSV %s: %v\n", csvPath, err)
			continue
		}
		reader := csv.NewReader(file)
		records, err := reader.ReadAll()
		file.Close()
		if err != nil {
			fmt.Fprintf(os.Stderr, "Warning: Failed to parse CSV %s: %v\n", csvPath, err)
			continue
		}
		for i, r := range records {
			if i == 0 || len(r) < 8 {
				continue
			}
			testCases = append(testCases, TestCase{
				ID:             r[0],
				Category:       r[1],
				Scenario:       r[2],
				Method:         r[3],
				Endpoint:       r[4],
				RequestBody:    r[5],
				ExpectedStatus: r[6],
				Description:    r[7],
			})
		}
	}
	fmt.Printf("Loaded %d total test cases from CSV suites.\n\n", len(testCases))

	// ── Placeholders ───────────────────────────────────────────────────────
	placeholders := map[string]string{
		"entity_l1_a_id":     "",
		"entity_l1_b_id":     "",
		"entity_l1_1_id":     "",
		"entity_l2_1_id":     "",
		"operator_a_id":      "",
		"operator_b_id":      "",
		"venue_l1_1_id":      "",
		"venue_l2_1_id":      "",
		"policy_a_id":        "",
		"policy_b_id":        "",
		"policy_c_id":        "",
		"role_a_id":          "",
		"sub_a_id":           "",
		"sub_a_user_id":      "",
		"user_admin_a_id":    "",
		"user_admin_b_id":    "",
		"role_admin_a_id":    "",
		"role_admin_b_id":    "",
		"role_venue_a_id":    "",
		"admin_a_token":      "",
		"admin_b_token":      "",
		"user_client_a_id":   "",
		"role_client_a_id":   "",
		"config_a_id":        "",
		"user_delegated_a_id": "",
		"role_delegated_a_id": "",
		"venue_l1_auto_id":   "",
		"role_venue_auto_id": "",
	}

	// ── Results ────────────────────────────────────────────────────────────
	passedCount := 0
	failedCount := 0
	knownFailCount := 0

	resultsFileName := getEnv("RESULTS_CSV", "rbac_actual_results.csv")
	fmt.Printf("Results will be written to: %s\n", resultsFileName)

	resultsFile, err := os.Create(resultsFileName)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Warning: Failed to create results CSV: %v\n", err)
	}
	defer resultsFile.Close()

	writer := csv.NewWriter(resultsFile)
	defer writer.Flush()
	_ = writer.Write([]string{
		"TestCaseID", "Category", "Scenario", "Method", "Endpoint",
		"ExpectedStatus", "ActualStatus", "Result", "KnownFailureReason", "Details",
	})

	placeholderRegex := regexp.MustCompile(`\$\{([a-zA-Z0-9_]+)\}`)

	// ── Execution loop ─────────────────────────────────────────────────────
	for _, tc := range testCases {
		fmt.Printf("\n--- Executing: %s (%s) ---\n", tc.ID, tc.Scenario)

		// Substitute placeholders in Endpoint and Body
		resolvedEndpoint := resolvePlaceholders(tc.Endpoint, placeholders, placeholderRegex)
		resolvedBody := resolvePlaceholders(tc.RequestBody, placeholders, placeholderRegex)

		// Route to correct microservice
		baseURL := owprovBase
		if strings.HasPrefix(tc.Endpoint, "/api/v1/oauth2") ||
			strings.HasPrefix(tc.Endpoint, "/api/v1/user") ||
			strings.HasPrefix(tc.Endpoint, "/api/v1/subuser") ||
			strings.HasPrefix(tc.Endpoint, "/api/v1/subscriber") {
			baseURL = owsecBase
		}

		trimmedEndpoint := strings.TrimPrefix(resolvedEndpoint, "/api/v1")

		// OWSEC user creation endpoint is POST /user/0 (handling query parameters)
		if tc.Method == "POST" && (trimmedEndpoint == "/user" || strings.HasPrefix(trimmedEndpoint, "/user?")) {
			trimmedEndpoint = strings.Replace(trimmedEndpoint, "/user", "/user/0", 1)
		}
		// POST /subuser → /subuser/0
		if tc.Method == "POST" && (trimmedEndpoint == "/subuser" || strings.HasPrefix(trimmedEndpoint, "/subuser?")) {
			trimmedEndpoint = strings.Replace(trimmedEndpoint, "/subuser", "/subuser/0", 1)
		}
		fullURL := baseURL + trimmedEndpoint
		fmt.Printf("%s %s\n", tc.Method, fullURL)
		if resolvedBody != "" {
			fmt.Printf("Body: %s\n", resolvedBody)
		}

		// Choose actor token
		activeToken := rootToken
		actor := "ROOT"
		if strings.HasPrefix(tc.ID, "ADMIN-FULL-") || strings.HasPrefix(tc.ID, "ADMIN-DELEGATION-") {
			activeToken = placeholders["token_admin_full"]
			actor = "Admin Full"
		} else if strings.HasPrefix(tc.ID, "ADMIN-READ-") {
			activeToken = placeholders["token_admin_read"]
			actor = "Admin Read"
		} else if strings.HasPrefix(tc.ID, "ADMIN-SUBVERIFY-") {
			activeToken = placeholders["token_sub_l1"]
			actor = "Sub-User 1"
		} else if strings.HasPrefix(tc.ID, "NOC-") {
			activeToken = placeholders["token_noc"]
			actor = "NOC User"
		} else if strings.HasPrefix(tc.ID, "CSR-") {
			activeToken = placeholders["token_csr"]
			actor = "CSR User"
		} else if strings.HasPrefix(tc.ID, "INSTALLER-") {
			activeToken = placeholders["token_installer"]
			actor = "INSTALLER User"
		} else if strings.HasPrefix(tc.ID, "ACCOUNTING-") {
			activeToken = placeholders["token_accounting"]
			actor = "ACCOUNTING User"
		} else if strings.HasPrefix(tc.ID, "DEF-ADMIN-") {
			activeToken = placeholders["token_def_admin"]
			actor = "Default Admin"
		} else if strings.HasPrefix(tc.ID, "DEF-NOC-") {
			activeToken = placeholders["token_def_noc"]
			actor = "Default NOC"
		} else if strings.HasPrefix(tc.ID, "DEF-CSR-") {
			activeToken = placeholders["token_def_csr"]
			actor = "Default CSR"
		} else if strings.HasPrefix(tc.ID, "DEF-INST-") {
			activeToken = placeholders["token_def_installer"]
			actor = "Default Installer"
		}
		fmt.Printf("Actor: %s\n", actor)

		// Build request
		var reqBody io.Reader
		if resolvedBody != "" {
			reqBody = bytes.NewReader([]byte(resolvedBody))
		}
		req, err := http.NewRequest(tc.Method, fullURL, reqBody)
		if err != nil {
			fmt.Printf("Error creating request: %v\n", err)
			failedCount++
			writeResult(writer, tc, resolvedEndpoint, "ERR", "FAIL", "", err.Error())
			continue
		}
		req.Header.Set("Content-Type", "application/json")
		if activeToken != "" {
			req.Header.Set("Authorization", "Bearer "+activeToken)
		}

		resp, err := client.Do(req)
		if err != nil {
			fmt.Printf("Request failed: %v\n", err)
			failedCount++
			writeResult(writer, tc, resolvedEndpoint, "ERR", "FAIL", "", err.Error())
			continue
		}
		bodyBytes, _ := io.ReadAll(resp.Body)
		resp.Body.Close()

		// Pause briefly after oauth2 login requests to prevent OWSEC rate limits
		if strings.HasPrefix(tc.Endpoint, "/api/v1/oauth2") {
			time.Sleep(250 * time.Millisecond)
		}

		actualStatus := resp.StatusCode
		fmt.Printf("Response Status: %d\n", actualStatus)

		// Extract placeholders BEFORE truncating output (use full body bytes)
		if len(bodyBytes) > 0 {
			var decoded map[string]any
			if json.Unmarshal(bodyBytes, &decoded) == nil && actualStatus >= 200 && actualStatus < 300 {
				extractPlaceholders(tc.ID, decoded, placeholders)
			}
		}

		if len(bodyBytes) > 0 {
			bodyStr := string(bodyBytes)
			if len(bodyStr) > 300 {
				bodyStr = bodyStr[:300] + "... [truncated]"
			}
			fmt.Printf("Response Body: %s\n", bodyStr)
		}

		// Check status
		statusMatches := false
		for _, exp := range strings.Split(tc.ExpectedStatus, "|") {
			expVal := 0
			fmt.Sscanf(exp, "%d", &expVal)
			if actualStatus == expVal {
				statusMatches = true
				break
			}
		}

		knownReason := KnownFailures[tc.ID]
		var resultStr, detailsStr string

		if statusMatches {
			passedCount++
			resultStr = "PASS"
			fmt.Println("Result: PASS")
		} else {
			if knownReason != "" {
				knownFailCount++
				resultStr = "KNOWN-FAIL"
			} else {
				failedCount++
				resultStr = "FAIL"
			}
			detailsStr = fmt.Sprintf("Expected %s, got %d. Body: %s",
				tc.ExpectedStatus, actualStatus, string(bodyBytes))
			fmt.Printf("Result: %s (%s)\n", resultStr, detailsStr)
		}

		writeResult(writer, tc, resolvedEndpoint, fmt.Sprintf("%d", actualStatus), resultStr, knownReason, detailsStr)
	}
	writer.Flush()

	// ── Summary ────────────────────────────────────────────────────────────
	total := len(testCases)
	fmt.Printf("\n╔══════════════════════════════════════════╗\n")
	fmt.Printf("║         Test Run Summary                 ║\n")
	fmt.Printf("╠══════════════════════════════════════════╣\n")
	fmt.Printf("║  Total run  : %-27d║\n", total)
	fmt.Printf("║  PASS       : %-27d║\n", passedCount)
	fmt.Printf("║  FAIL       : %-27d║\n", failedCount)
	fmt.Printf("║  KNOWN-FAIL : %-27d║\n", knownFailCount)
	fmt.Printf("╚══════════════════════════════════════════╝\n")
	fmt.Printf("Detailed results written to: %s\n", resultsFileName)

	if failedCount > 0 {
		fmt.Printf("\n[ERROR] %d unexpected failure(s). Inspect %s.\n", failedCount, resultsFileName)
		os.Exit(1)
	}
	fmt.Printf("\n[OK] All %d expected test cases passed (plus %d known infrastructure failures noted).\n",
		passedCount, knownFailCount)
}

// ─── Pre-cleanup ─────────────────────────────────────────────────────────────

func preCleanup(client *http.Client, owsecBase, owprovBase, rootToken string) {
	// Delete test users if they exist
	testEmails := []string{
		"admina@rbac.local",
		"adminb@rbac.local",
		"clienta@rbac.local",
		"delegateda@rbac.local",
	}
	for _, email := range testEmails {
		// Find user by listing and matching email
		userID := findUserByEmail(client, owsecBase, rootToken, email)
		if userID != "" {
			deleteResource(client, owsecBase+"/user/"+userID, rootToken, "user "+email)
		}
	}

	// Delete test operators/entities/venues/policies/roles by name via list
	deleteTestOperators(client, owprovBase, rootToken, []string{"op-l1-a", "updated-op-a", "op-l1-b"})
	deleteTestEntities(client, owprovBase, rootToken, []string{"entity-l1-1", "entity-l2-1"})
	deleteTestInventory(client, owprovBase, rootToken, "dc6279652f20")
	deleteTestPolicies(client, owprovBase, rootToken)
}

func findUserByEmail(client *http.Client, owsecBase, token, email string) string {
	req, _ := http.NewRequest("GET", owsecBase+"/user", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := client.Do(req)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	var decoded map[string]any
	if json.Unmarshal(b, &decoded) != nil {
		return ""
	}
	users, _ := decoded["users"].([]any)
	for _, u := range users {
		uMap, _ := u.(map[string]any)
		if uMap == nil {
			continue
		}
		if uMap["email"] == email {
			id, _ := uMap["id"].(string)
			return id
		}
	}
	return ""
}

func deleteTestPolicies(client *http.Client, owprovBase, token string) {
	req, _ := http.NewRequest("GET", owprovBase+"/managementPolicy", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := client.Do(req)
	if err != nil {
		return
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	var decoded map[string]any
	if json.Unmarshal(b, &decoded) != nil {
		return
	}
	policies, _ := decoded["managementPolicies"].([]any)
	for _, p := range policies {
		pMap, _ := p.(map[string]any)
		if pMap == nil {
			continue
		}
		name, _ := pMap["name"].(string)
		id, _ := pMap["id"].(string)
		if strings.HasPrefix(name, "policy-") {
			deleteResource(client, owprovBase+"/managementPolicy/"+id, token, "policy "+name)
		}
	}
}

func deleteTestOperators(client *http.Client, owprovBase, token string, names []string) {
	req, _ := http.NewRequest("GET", owprovBase+"/operator", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := client.Do(req)
	if err != nil {
		return
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	var decoded map[string]any
	if json.Unmarshal(b, &decoded) != nil {
		return
	}
	operators, _ := decoded["operators"].([]any)
	for _, o := range operators {
		oMap, _ := o.(map[string]any)
		if oMap == nil {
			continue
		}
		name, _ := oMap["name"].(string)
		id, _ := oMap["id"].(string)
		if id == "" {
			continue
		}
		for _, n := range names {
			if name == n {
				deleteResource(client, owprovBase+"/operator/"+id, token, "operator "+name)
				break
			}
		}
	}
}

func deleteTestEntities(client *http.Client, owprovBase, token string, names []string) {
	req, _ := http.NewRequest("GET", owprovBase+"/entity", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := client.Do(req)
	if err != nil {
		return
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	var decoded map[string]any
	if json.Unmarshal(b, &decoded) != nil {
		return
	}
	entities, _ := decoded["entities"].([]any)
	for _, e := range entities {
		eMap, _ := e.(map[string]any)
		if eMap == nil {
			continue
		}
		name, _ := eMap["name"].(string)
		id, _ := eMap["id"].(string)
		if id == "" || id == "0000-0000-0000" {
			continue
		}
		for _, n := range names {
			if name == n {
				deleteResource(client, owprovBase+"/entity/"+id, token, "entity "+name)
				break
			}
		}
	}
}

func deleteTestInventory(client *http.Client, owprovBase, token, serial string) {
	req, _ := http.NewRequest("DELETE", owprovBase+"/inventory/"+serial, nil)
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := client.Do(req)
	if err != nil {
		return
	}
	resp.Body.Close()
	if resp.StatusCode == 200 {
		fmt.Printf("[Pre-cleanup] Deleted inventory tag %s\n", serial)
	}
}

func deleteResource(client *http.Client, url, token, label string) {
	req, _ := http.NewRequest("DELETE", url, nil)
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := client.Do(req)
	if err != nil {
		return
	}
	resp.Body.Close()
	if resp.StatusCode == 200 {
		fmt.Printf("[Pre-cleanup] Deleted %s\n", label)
	}
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

func resolvePlaceholders(s string, placeholders map[string]string, re *regexp.Regexp) string {
	return re.ReplaceAllStringFunc(s, func(m string) string {
		key := m[2 : len(m)-1]
		val, ok := placeholders[key]
		if !ok || val == "" {
			fmt.Printf("[Placeholder WARNING] Key %s is not set!\n", key)
			return m
		}
		return val
	})
}

func writeResult(w *csv.Writer, tc TestCase, endpoint, actualStatus, result, knownReason, details string) {
	_ = w.Write([]string{
		tc.ID, tc.Category, tc.Scenario, tc.Method, endpoint,
		tc.ExpectedStatus, actualStatus, result, knownReason, details,
	})
}

func login(client *http.Client, base, email, password string) (string, error) {
	payload := map[string]any{"userId": email, "username": email, "password": password}
	raw, _ := json.Marshal(payload)
	req, err := http.NewRequest(http.MethodPost, base+"/oauth2", bytes.NewReader(raw))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	bodyBytes, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("login failed: status %d, body %s", resp.StatusCode, string(bodyBytes))
	}
	var decoded map[string]any
	if err := json.Unmarshal(bodyBytes, &decoded); err != nil {
		return "", err
	}
	token, _ := decoded["access_token"].(string)
	if token == "" {
		token, _ = decoded["token"].(string)
	}
	if token == "" {
		return "", fmt.Errorf("access token not found: %s", string(bodyBytes))
	}
	return token, nil
}

func ensureRootPasswordChanged(client *http.Client, owsecBase, email, oldPassword, newPassword string) error {
	// Step 1: Check if login already works with target new password
	_, err := login(client, owsecBase, email, newPassword)
	if err == nil {
		fmt.Println("[Bootstrap] Root user already configured with new password.")
		return nil
	}

	// Step 2: If login failed, perform first-boot password change
	fmt.Println("[Bootstrap] First boot detected. Changing root password from default ('openwifi') to target password...")

	payload := map[string]string{
		"userId":      email,
		"password":    oldPassword,
		"newPassword": newPassword,
	}
	raw, _ := json.Marshal(payload)

	req, err := http.NewRequest(http.MethodPost, owsecBase+"/oauth2", bytes.NewReader(raw))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := client.Do(req)
	if err != nil {
		return fmt.Errorf("failed to send password change request: %w", err)
	}
	defer resp.Body.Close()

	bodyBytes, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("first-boot password change failed: status %d, body: %s", resp.StatusCode, string(bodyBytes))
	}

	fmt.Println("[Bootstrap] Root password successfully changed to new password!")
	return nil
}

func extractPlaceholders(tcID string, decoded map[string]any, placeholders map[string]string) {
	id, _ := decoded["id"].(string)
	if id == "" {
		id, _ = decoded["uuid"].(string)
	}
	entityId, _ := decoded["entityId"].(string)
	if entityId == "" {
		entityId, _ = decoded["entity"].(string)
	}

	// Support nested 'operator' or 'operation' response wrapper
	if opObj, ok := decoded["operator"].(map[string]any); ok {
		if id == "" {
			id, _ = opObj["id"].(string)
		}
		if entityId == "" {
			entityId, _ = opObj["entityId"].(string)
		}
	}
	if opObj, ok := decoded["operation"].(map[string]any); ok {
		if id == "" {
			id, _ = opObj["id"].(string)
		}
		if entityId == "" {
			entityId, _ = opObj["entityId"].(string)
		}
	}

	switch tcID {
	case "ROOT-BOOTSTRAP-0002", "NONROOT-BOOTSTRAP-0002":
		placeholders["operator_a_id"] = id
		placeholders["entity_l1_a_id"] = entityId
		fmt.Printf("[Extracted] operator_a_id=%s  entity_l1_a_id=%s\n", id, entityId)
	case "ROOT-BOOTSTRAP-0003", "NONROOT-BOOTSTRAP-0003":
		placeholders["operator_b_id"] = id
		placeholders["entity_l1_b_id"] = entityId
		fmt.Printf("[Extracted] operator_b_id=%s  entity_l1_b_id=%s\n", id, entityId)
	case "ROOT-BOOTSTRAP-0004":
		placeholders["entity_l1_1_id"] = id
		fmt.Printf("[Extracted] entity_l1_1_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0005":
		placeholders["entity_l2_1_id"] = id
		fmt.Printf("[Extracted] entity_l2_1_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0006":
		placeholders["venue_l1_1_id"] = id
		fmt.Printf("[Extracted] venue_l1_1_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0007":
		placeholders["venue_l2_1_id"] = id
		fmt.Printf("[Extracted] venue_l2_1_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0008":
		// inventory tag – id is serial-based, no UUID to extract
	case "ROOT-BOOTSTRAP-0009":
		placeholders["config_a_id"] = id
		fmt.Printf("[Extracted] config_a_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0010":
		placeholders["policy_a_id"] = id
		fmt.Printf("[Extracted] policy_a_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0011":
		placeholders["policy_b_id"] = id
		fmt.Printf("[Extracted] policy_b_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0012":
		placeholders["policy_c_id"] = id
		fmt.Printf("[Extracted] policy_c_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0013":
		placeholders["role_a_id"] = id
		fmt.Printf("[Extracted] role_a_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0014":
		placeholders["sub_a_id"] = id
		// subscriber's owsec user ID — from 'userId' field
		if userID, ok := decoded["userId"].(string); ok && userID != "" {
			placeholders["sub_a_user_id"] = userID
			fmt.Printf("[Extracted] sub_a_id=%s  sub_a_user_id=%s\n", id, userID)
		} else {
			// Some versions use nested 'userInfo.id' structure
			if uInfo, ok2 := decoded["userInfo"].(map[string]any); ok2 {
				if uid, ok3 := uInfo["id"].(string); ok3 {
					placeholders["sub_a_user_id"] = uid
				}
			}
			fmt.Printf("[Extracted] sub_a_id=%s  sub_a_user_id=%s\n", id, placeholders["sub_a_user_id"])
		}
	case "ROOT-BOOTSTRAP-0015":
		placeholders["user_admin_a_id"] = id
		fmt.Printf("[Extracted] user_admin_a_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0016":
		placeholders["user_admin_b_id"] = id
		fmt.Printf("[Extracted] user_admin_b_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0017":
		placeholders["role_admin_a_id"] = id
		fmt.Printf("[Extracted] role_admin_a_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0018":
		placeholders["role_admin_b_id"] = id
		fmt.Printf("[Extracted] role_admin_b_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0019":
		placeholders["role_venue_a_id"] = id
		fmt.Printf("[Extracted] role_venue_a_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0020":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["admin_a_token"] = tok
		fmt.Printf("[Extracted] admin_a_token=%s\n", tok)
	case "ROOT-BOOTSTRAP-0021":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["admin_b_token"] = tok
		fmt.Printf("[Extracted] admin_b_token=%s\n", tok)
	case "ROOT-BOOTSTRAP-0022":
		placeholders["user_client_a_id"] = id
		fmt.Printf("[Extracted] user_client_a_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0023":
		placeholders["role_client_a_id"] = id
		fmt.Printf("[Extracted] role_client_a_id=%s\n", id)
	case "ROOT-BOOTSTRAP-0024":
		placeholders["user_delegated_a_id"] = id
		fmt.Printf("[Extracted] user_delegated_a_id=%s\n", id)
	case "ROOT-DELEGATION-0001":
		placeholders["role_delegated_a_id"] = id
		fmt.Printf("[Extracted] role_delegated_a_id=%s\n", id)
	case "ROOT-AUTOROLE-0001":
		placeholders["venue_l1_auto_id"] = id
		fmt.Printf("[Extracted] venue_l1_auto_id=%s\n", id)
	case "ROOT-AUTOROLE-0002":
		// Find the auto-created role for admin_a on venue_l1_auto
		if roles, ok := decoded["roles"].([]any); ok {
			autoVenueID := placeholders["venue_l1_auto_id"]
			for _, rVal := range roles {
				rMap, _ := rVal.(map[string]any)
				if rMap == nil {
					continue
				}
				vID, _ := rMap["venue"].(string)
				if vID == autoVenueID {
					roleID, _ := rMap["id"].(string)
					placeholders["role_venue_auto_id"] = roleID
					fmt.Printf("[Extracted] role_venue_auto_id=%s\n", roleID)
					break
				}
			}
		}

	case "NONROOT-BOOTSTRAP-0004":
		placeholders["policy_admin_full_id"] = id
		fmt.Printf("[Extracted] policy_admin_full_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0005":
		placeholders["policy_admin_read_id"] = id
		fmt.Printf("[Extracted] policy_admin_read_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0006":
		placeholders["policy_noc_id"] = id
		fmt.Printf("[Extracted] policy_noc_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0007":
		placeholders["policy_csr_id"] = id
		fmt.Printf("[Extracted] policy_csr_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0008":
		placeholders["policy_installer_id"] = id
		fmt.Printf("[Extracted] policy_installer_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0009":
		placeholders["policy_accounting_id"] = id
		fmt.Printf("[Extracted] policy_accounting_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0010":
		placeholders["user_admin_full_id"] = id
		fmt.Printf("[Extracted] user_admin_full_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0011":
		placeholders["user_admin_read_id"] = id
		fmt.Printf("[Extracted] user_admin_read_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0012":
		placeholders["user_noc_id"] = id
		fmt.Printf("[Extracted] user_noc_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0013":
		placeholders["user_csr_id"] = id
		fmt.Printf("[Extracted] user_csr_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0014":
		placeholders["user_installer_id"] = id
		fmt.Printf("[Extracted] user_installer_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0015":
		placeholders["user_accounting_id"] = id
		fmt.Printf("[Extracted] user_accounting_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0016":
		placeholders["venue_l1_1_id"] = id
		fmt.Printf("[Extracted] venue_l1_1_id=%s\n", id)
	case "NONROOT-BOOTSTRAP-0017":
		placeholders["role_admin_full_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_admin_full_id=%s\n", placeholders["role_admin_full_id"])
	case "NONROOT-BOOTSTRAP-0018":
		placeholders["role_admin_read_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_admin_read_id=%s\n", placeholders["role_admin_read_id"])
	case "NONROOT-BOOTSTRAP-0019":
		placeholders["role_noc_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_noc_id=%s\n", placeholders["role_noc_id"])
	case "NONROOT-BOOTSTRAP-0020":
		placeholders["role_csr_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_csr_id=%s\n", placeholders["role_csr_id"])
	case "NONROOT-BOOTSTRAP-0021":
		placeholders["role_installer_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_installer_id=%s\n", placeholders["role_installer_id"])
	case "NONROOT-BOOTSTRAP-0022":
		placeholders["role_accounting_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_accounting_id=%s\n", placeholders["role_accounting_id"])
	case "NONROOT-BOOTSTRAP-0023":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["token_admin_full"] = tok
		fmt.Printf("[Extracted] token_admin_full=%s\n", tok)
	case "NONROOT-BOOTSTRAP-0024":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["token_admin_read"] = tok
		fmt.Printf("[Extracted] token_admin_read=%s\n", tok)
	case "NONROOT-BOOTSTRAP-0025":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["token_noc"] = tok
		fmt.Printf("[Extracted] token_noc=%s\n", tok)
	case "NONROOT-BOOTSTRAP-0026":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["token_csr"] = tok
		fmt.Printf("[Extracted] token_csr=%s\n", tok)
	case "NONROOT-BOOTSTRAP-0027":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["token_installer"] = tok
		fmt.Printf("[Extracted] token_installer=%s\n", tok)
	case "NONROOT-BOOTSTRAP-0028":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["token_accounting"] = tok
		fmt.Printf("[Extracted] token_accounting=%s\n", tok)
	case "NONROOT-BOOTSTRAP-0030":
		placeholders["config_a_id"] = id
		fmt.Printf("[Extracted] config_a_id=%s\n", id)
	case "DEFAULT-BOOTSTRAP-0001":
		placeholders["user_def_admin_id"] = id
		fmt.Printf("[Extracted] user_def_admin_id=%s\n", id)
	case "DEFAULT-BOOTSTRAP-0002":
		placeholders["user_def_noc_id"] = id
		fmt.Printf("[Extracted] user_def_noc_id=%s\n", id)
	case "DEFAULT-BOOTSTRAP-0003":
		placeholders["user_def_csr_id"] = id
		fmt.Printf("[Extracted] user_def_csr_id=%s\n", id)
	case "DEFAULT-BOOTSTRAP-0004":
		placeholders["user_def_installer_id"] = id
		fmt.Printf("[Extracted] user_def_installer_id=%s\n", id)
	case "DEFAULT-BOOTSTRAP-0005":
		placeholders["role_def_admin_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_def_admin_id=%s\n", placeholders["role_def_admin_id"])
	case "DEFAULT-BOOTSTRAP-0006":
		placeholders["role_def_noc_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_def_noc_id=%s\n", placeholders["role_def_noc_id"])
	case "DEFAULT-BOOTSTRAP-0007":
		placeholders["role_def_csr_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_def_csr_id=%s\n", placeholders["role_def_csr_id"])
	case "DEFAULT-BOOTSTRAP-0008":
		placeholders["role_def_installer_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_def_installer_id=%s\n", placeholders["role_def_installer_id"])
	case "DEFAULT-BOOTSTRAP-0009":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["token_def_admin"] = tok
		fmt.Printf("[Extracted] token_def_admin=%s\n", tok)
	case "DEFAULT-BOOTSTRAP-0010":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["token_def_noc"] = tok
		fmt.Printf("[Extracted] token_def_noc=%s\n", tok)
	case "DEFAULT-BOOTSTRAP-0011":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["token_def_csr"] = tok
		fmt.Printf("[Extracted] token_def_csr=%s\n", tok)
	case "DEFAULT-BOOTSTRAP-0012":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["token_def_installer"] = tok
		fmt.Printf("[Extracted] token_def_installer=%s\n", tok)
	case "DEF-ADMIN-0002":
		placeholders["venue_def_admin_id"] = id
		fmt.Printf("[Extracted] venue_def_admin_id=%s\n", id)
	case "ADMIN-FULL-0002":
		placeholders["entity_l2_admin_id"] = id
		fmt.Printf("[Extracted] entity_l2_admin_id=%s\n", id)
	case "ADMIN-FULL-0003":
		placeholders["venue_l2_admin_id"] = id
		fmt.Printf("[Extracted] venue_l2_admin_id=%s\n", id)
	case "ADMIN-DELEGATION-0001":
		placeholders["user_sub_l1_id"] = id
		fmt.Printf("[Extracted] user_sub_l1_id=%s\n", id)
	case "ADMIN-DELEGATION-0002":
		placeholders["role_sub_l1_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_sub_l1_id=%s\n", placeholders["role_sub_l1_id"])
	case "ADMIN-DELEGATION-0003":
		tok, _ := decoded["access_token"].(string)
		if tok == "" {
			tok, _ = decoded["token"].(string)
		}
		placeholders["token_sub_l1"] = tok
		fmt.Printf("[Extracted] token_sub_l1=%s\n", tok)
	case "ADMIN-FULL-0004":
		placeholders["user_sub_full_id"] = id
		fmt.Printf("[Extracted] user_sub_full_id=%s\n", id)
	case "ADMIN-FULL-0005":
		placeholders["role_sub_full_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_sub_full_id=%s\n", placeholders["role_sub_full_id"])
	case "ADMIN-INSCOPE-0008":
		placeholders["policy_admin_a_id"] = id
		fmt.Printf("[Extracted] policy_admin_a_id=%s\n", id)
	case "ADMIN-DELEGATION-POS-0001":
		placeholders["role_sub_a_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_sub_a_id=%s\n", placeholders["role_sub_a_id"])
	case "ADMIN-HIERARCHY-NEG-0005":
		placeholders["entity_hacked_b_id"] = id
		fmt.Printf("[Extracted] entity_hacked_b_id=%s\n", id)
	case "ADMIN-HIERARCHY-NEG-0006":
		placeholders["venue_hacked_b_id"] = id
		fmt.Printf("[Extracted] venue_hacked_b_id=%s\n", id)
	case "ADMIN-HIERARCHY-NEG-0007":
		placeholders["op_unauth_id"] = id
		if entity, ok := decoded["entityId"].(string); ok && entity != "" {
			placeholders["entity_op_unauth_id"] = entity
		}
		fmt.Printf("[Extracted] op_unauth_id=%s\n", id)
	case "ADMIN-SHADOW-0001":
		placeholders["role_venue_shadow_id"] = extractRoleID(id, decoded)
		fmt.Printf("[Extracted] role_venue_shadow_id=%s\n", placeholders["role_venue_shadow_id"])
	}
}

func extractRoleID(fallbackID string, decoded map[string]any) string {
	if roleID, ok := decoded["id"].(string); ok && roleID != "" {
		return roleID
	}
	if roles, ok := decoded["roles"].([]any); ok && len(roles) > 0 {
		if rMap, ok := roles[0].(map[string]any); ok {
			if rID, ok := rMap["id"].(string); ok {
				return rID
			}
		}
	}
	return fallbackID
}

func getEnv(key, fallback string) string {
	if val := os.Getenv(key); val != "" {
		return val
	}
	return fallback
}

func loadEnvFile(filename string) {
	file, err := os.Open(filename)
	if err != nil {
		return
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.SplitN(line, "=", 2)
		if len(parts) == 2 {
			k := strings.TrimSpace(parts[0])
			v := strings.TrimSpace(parts[1])
			if os.Getenv(k) == "" {
				os.Setenv(k, v)
			}
		}
	}
}
