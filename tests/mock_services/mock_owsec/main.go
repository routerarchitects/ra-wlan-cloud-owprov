package main

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

// ─── Data Models ──────────────────────────────────────────────────────────────

type WebToken struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token,omitempty"`
	TokenType    string `json:"token_type"`
	Created      int64  `json:"created"`
	Expires      int64  `json:"expires"`
	ExpiresIn    int64  `json:"expires_in"`
	ID           string `json:"uuid,omitempty"`
}

type UserRight struct {
	Role string `json:"role"`
}

type UserInfo struct {
	ID            string      `json:"id"`
	Email         string      `json:"email"`
	UserRole      string      `json:"userRole"`
	Name          string      `json:"name,omitempty"`
	UserRights    []UserRight `json:"userRights,omitempty"`
	OperatorID    string      `json:"operatorId,omitempty"`
	Registration  string      `json:"registrationId,omitempty"`
}

type TokenInfo struct {
	Token       string `json:"token"`
	AccessToken string `json:"access_token"`
	UserRole    string `json:"userRole"`
	Created     int64  `json:"created"`
	Expires     int64  `json:"expires"`
	ExpiresIn   int64  `json:"expires_in"`
}

type UserInfoAndPolicy struct {
	TokenInfo TokenInfo `json:"tokenInfo"`
	UserInfo  UserInfo  `json:"userInfo"`
	WebToken  WebToken  `json:"webtoken"`
	ExpiresOn int64     `json:"expiresOn,omitempty"`
}

type UserRecord struct {
	ID          string    `json:"id"`
	Email       string    `json:"email"`
	Username    string    `json:"username"`
	Password    string    `json:"password"`
	UserRole    string    `json:"userRole"`
	Name        string    `json:"name"`
	OperatorID  string    `json:"operatorId,omitempty"`
	UserRights  []UserRight `json:"userRights,omitempty"`
}

type SubUserRecord struct {
	ID         string `json:"id"`
	UserID     string `json:"userId"`
	Email      string `json:"email"`
	Name       string `json:"name"`
	OperatorID string `json:"operatorId,omitempty"`
}

// ─── In-Memory Store ──────────────────────────────────────────────────────────

type MockStore struct {
	mu          sync.Mutex
	users       map[string]*UserRecord    // key: id or email
	subUsers    map[string]*SubUserRecord // key: id
	tokens      map[string]*UserInfoAndPolicy // key: token string
	rootEmail   string
	rootPass    string
}

func newUUID() string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:])
}

func newMockStore() *MockStore {
	s := &MockStore{
		users:     make(map[string]*UserRecord),
		subUsers:  make(map[string]*SubUserRecord),
		tokens:    make(map[string]*UserInfoAndPolicy),
		rootEmail: "tip@ucentral.com",
		rootPass:  "Iotina@123",
	}

	// Pre-seed default ROOT user
	rootID := "00000000-0000-0000-0000-000000000001"
	rootUser := &UserRecord{
		ID:         rootID,
		Email:      s.rootEmail,
		Username:   s.rootEmail,
		Password:   s.rootPass,
		UserRole:   "root",
		Name:       "Root Administrator",
		UserRights: []UserRight{{Role: "root"}},
	}
	s.users[rootID] = rootUser
	s.users[s.rootEmail] = rootUser

	// Pre-seed root token
	rootToken := "root-test-token"
	s.tokens[rootToken] = &UserInfoAndPolicy{
		TokenInfo: TokenInfo{
			Token:    rootToken,
			UserRole: "root",
			Created:  time.Now().Unix(),
			Expires:  time.Now().Add(24 * time.Hour).Unix(),
		},
		UserInfo: UserInfo{
			ID:         rootID,
			Email:      s.rootEmail,
			UserRole:   "root",
			Name:       "Root Administrator",
			UserRights: []UserRight{{Role: "root"}},
		},
		WebToken: WebToken{
			AccessToken: rootToken,
			TokenType:   "Bearer",
			Created:     time.Now().Unix(),
			Expires:     time.Now().Add(24 * time.Hour).Unix(),
			ID:          rootID,
		},
	}

	return s
}

// ─── HTTP Handlers ────────────────────────────────────────────────────────────

func (s *MockStore) handleOAuth2(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req map[string]interface{}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	userID, _ := req["userId"].(string)
	if userID == "" {
		userID, _ = req["username"].(string)
	}
	password, _ := req["password"].(string)
	newPassword, _ := req["newPassword"].(string)

	// Handle first-boot password change
	if newPassword != "" {
		if userID == s.rootEmail || userID == "tip@ucentral.com" {
			s.rootPass = newPassword
			if user, ok := s.users[s.rootEmail]; ok {
				user.Password = newPassword
			}
			w.WriteHeader(http.StatusOK)
			_ = json.NewEncoder(w).Encode(map[string]string{"status": "success", "message": "Password changed"})
			return
		}
	}

	// Validate login credentials
	user, exists := s.users[userID]
	if !exists {
		// Allow root login fallback
		if userID == s.rootEmail && (password == s.rootPass || password == "openwifi" || password == "Iotina@123" || password == "Iotina@1234") {
			user = s.users[s.rootEmail]
		} else {
			http.Error(w, `{"error":"Invalid credentials"}`, http.StatusUnauthorized)
			return
		}
	} else {
		if user.Password != "" && user.Password != password && password != "openwifi" && password != "Iotina@123" {
			http.Error(w, `{"error":"Invalid credentials"}`, http.StatusUnauthorized)
			return
		}
	}

	// Issue token
	tokenStr := "mock-token-" + hex.EncodeToString([]byte(user.ID)) + "-" + fmt.Sprintf("%d", time.Now().UnixNano())
	now := time.Now().Unix()
	exp := time.Now().Add(24 * time.Hour).Unix()

	uInfo := UserInfo{
		ID:         user.ID,
		Email:      user.Email,
		UserRole:   user.UserRole,
		Name:       user.Name,
		OperatorID: user.OperatorID,
		UserRights: user.UserRights,
	}
	if len(uInfo.UserRights) == 0 {
		uInfo.UserRights = []UserRight{{Role: user.UserRole}}
	}

	wt := WebToken{
		AccessToken:  tokenStr,
		RefreshToken: "refresh-" + tokenStr,
		TokenType:    "Bearer",
		Created:      now,
		Expires:      exp,
		ExpiresIn:    86400,
		ID:           user.ID,
	}

	s.tokens[tokenStr] = &UserInfoAndPolicy{
		TokenInfo: TokenInfo{
			Token:       tokenStr,
			AccessToken: tokenStr,
			UserRole:    user.UserRole,
			Created:     now,
			Expires:     exp,
			ExpiresIn:   86400,
		},
		UserInfo: uInfo,
		WebToken: wt,
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_ = json.NewEncoder(w).Encode(wt)
}

func (s *MockStore) handleValidateToken(w http.ResponseWriter, r *http.Request) {
	token := r.URL.Query().Get("token")
	if token == "" {
		token = r.URL.Query().Get("apikey")
	}

	// Clean Bearer prefix if present
	token = strings.TrimPrefix(token, "Bearer ")

	s.mu.Lock()
	defer s.mu.Unlock()

	// If token not in map, generate dynamic valid response for testing
	info, exists := s.tokens[token]
	if !exists {
		// Fallback for root / test tokens
		role := "root"
		if strings.Contains(token, "admin") {
			role = "admin"
		}
		info = &UserInfoAndPolicy{
			TokenInfo: TokenInfo{
				Token:       token,
				AccessToken: token,
				UserRole:    role,
				Created:     time.Now().Unix(),
				Expires:     time.Now().Add(24 * time.Hour).Unix(),
				ExpiresIn:   86400,
			},
			UserInfo: UserInfo{
				ID:         "mock-user-id",
				Email:      "mock@openwifi.local",
				UserRole:   role,
				UserRights: []UserRight{{Role: role}},
			},
			WebToken: WebToken{
				AccessToken: token,
				TokenType:   "Bearer",
				Created:     time.Now().Unix(),
				Expires:     time.Now().Add(24 * time.Hour).Unix(),
				ExpiresIn:   86400,
			},
			ExpiresOn: time.Now().Add(24 * time.Hour).Unix(),
		}
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_ = json.NewEncoder(w).Encode(info)
}

func (s *MockStore) handleUser(w http.ResponseWriter, r *http.Request) {
	path := strings.TrimPrefix(r.URL.Path, "/api/v1/user")
	path = strings.TrimPrefix(path, "/")

	s.mu.Lock()
	defer s.mu.Unlock()

	switch r.Method {
	case http.MethodGet:
		if path == "" || path == "0" {
			// List users
			var userList []UserInfo
			for _, u := range s.users {
				userList = append(userList, UserInfo{
					ID:         u.ID,
					Email:      u.Email,
					UserRole:   u.UserRole,
					Name:       u.Name,
					OperatorID: u.OperatorID,
					UserRights: u.UserRights,
				})
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]interface{}{"users": userList})
			return
		}

		// Get single user by ID
		if u, ok := s.users[path]; ok {
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(UserInfo{
				ID:         u.ID,
				Email:      u.Email,
				UserRole:   u.UserRole,
				Name:       u.Name,
				OperatorID: u.OperatorID,
				UserRights: u.UserRights,
			})
			return
		}
		http.Error(w, `{"error":"User not found"}`, http.StatusNotFound)

	case http.MethodPost:
		auth := r.Header.Get("Authorization")
		token := strings.TrimPrefix(auth, "Bearer ")
		if token != "" {
			if info, exists := s.tokens[token]; exists {
				if info.TokenInfo.UserRole != "root" && info.TokenInfo.UserRole != "admin" {
					w.Header().Set("Content-Type", "application/json")
					w.WriteHeader(http.StatusForbidden)
					_ = json.NewEncoder(w).Encode(map[string]interface{}{
						"ErrorCode":        7,
						"ErrorDescription": "7: Access denied.",
					})
					return
				}
			}
		}
		var u UserRecord
		if err := json.NewDecoder(r.Body).Decode(&u); err != nil {
			http.Error(w, "Invalid JSON", http.StatusBadRequest)
			return
		}
		if u.ID == "" || u.ID == "0" {
			u.ID = newUUID()
		}
		if u.Username != "" && u.Email == "" {
			u.Email = u.Username
		}
		s.users[u.ID] = &u
		if u.Email != "" {
			s.users[u.Email] = &u
		}

		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_ = json.NewEncoder(w).Encode(UserInfo{
			ID:         u.ID,
			Email:      u.Email,
			UserRole:   u.UserRole,
			Name:       u.Name,
			OperatorID: u.OperatorID,
			UserRights: u.UserRights,
		})

	case http.MethodPut:
		if path != "" {
			var u UserRecord
			_ = json.NewDecoder(r.Body).Decode(&u)
			if existing, ok := s.users[path]; ok {
				if u.Name != "" {
					existing.Name = u.Name
				}
				if u.UserRole != "" {
					existing.UserRole = u.UserRole
				}
			}
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusOK)
			_ = json.NewEncoder(w).Encode(map[string]string{"status": "updated"})
			return
		}
		http.Error(w, "Missing user ID", http.StatusBadRequest)

	case http.MethodDelete:
		if path != "" {
			if u, ok := s.users[path]; ok {
				delete(s.users, u.Email)
				delete(s.users, u.ID)
			}
			w.WriteHeader(http.StatusOK)
			_ = json.NewEncoder(w).Encode(map[string]string{"status": "deleted"})
			return
		}
		http.Error(w, "Missing user ID", http.StatusBadRequest)

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *MockStore) handleSubUser(w http.ResponseWriter, r *http.Request) {
	path := strings.TrimPrefix(r.URL.Path, "/api/v1/subuser")
	path = strings.TrimPrefix(path, "/")

	s.mu.Lock()
	defer s.mu.Unlock()

	switch r.Method {
	case http.MethodGet:
		if path == "" || path == "0" || strings.HasPrefix(path, "s") {
			var subList []SubUserRecord
			for _, su := range s.subUsers {
				subList = append(subList, *su)
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]interface{}{"users": subList, "subscribers": subList})
			return
		}

		if su, ok := s.subUsers[path]; ok {
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(su)
			return
		}
		http.Error(w, `{"error":"Subscriber not found"}`, http.StatusNotFound)

	case http.MethodPost:
		var su SubUserRecord
		if err := json.NewDecoder(r.Body).Decode(&su); err != nil {
			http.Error(w, "Invalid JSON", http.StatusBadRequest)
			return
		}
		if su.ID == "" || su.ID == "0" {
			su.ID = newUUID()
		}
		if su.UserID == "" {
			su.UserID = newUUID()
		}
		s.subUsers[su.ID] = &su

		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_ = json.NewEncoder(w).Encode(su)

	case http.MethodDelete:
		if path != "" {
			delete(s.subUsers, path)
			w.WriteHeader(http.StatusOK)
			_ = json.NewEncoder(w).Encode(map[string]string{"status": "deleted"})
			return
		}
		http.Error(w, "Missing subscriber ID", http.StatusBadRequest)

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

// ─── Main ─────────────────────────────────────────────────────────────────────

func generateSelfSignedCert() (tls.Certificate, error) {
	priv, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return tls.Certificate{}, err
	}
	template := x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject: pkix.Name{
			Organization: []string{"OpenWifi Mock"},
		},
		NotBefore:             time.Now().Add(-1 * time.Hour),
		NotAfter:              time.Now().Add(365 * 24 * time.Hour),
		KeyUsage:              x509.KeyUsageKeyEncipherment | x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		IPAddresses:           []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP("::1")},
		DNSNames:              []string{"localhost"},
	}
	derBytes, err := x509.CreateCertificate(rand.Reader, &template, &template, &priv.PublicKey, priv)
	if err != nil {
		return tls.Certificate{}, err
	}
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: derBytes})
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(priv)})
	return tls.X509KeyPair(certPEM, keyPEM)
}

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "16001"
	}

	store := newMockStore()

	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/oauth2", store.handleOAuth2)
	mux.HandleFunc("/api/v1/validateToken", store.handleValidateToken)
	mux.HandleFunc("/api/v1/validateSubToken", store.handleValidateToken)
	mux.HandleFunc("/api/v1/validateApiKey", store.handleValidateToken)
	mux.HandleFunc("/api/v1/user", store.handleUser)
	mux.HandleFunc("/api/v1/user/", store.handleUser)
	mux.HandleFunc("/api/v1/subuser", store.handleSubUser)
	mux.HandleFunc("/api/v1/subuser/", store.handleSubUser)
	mux.HandleFunc("/api/v1/subusers", store.handleSubUser)
	mux.HandleFunc("/api/v1/subscriber", store.handleSubUser)
	mux.HandleFunc("/api/v1/subscriber/", store.handleSubUser)

	cert, err := generateSelfSignedCert()
	if err != nil {
		log.Fatalf("[mock_owsec] Failed to generate TLS cert: %v", err)
	}

	server := &http.Server{
		Addr:    ":" + port,
		Handler: mux,
		TLSConfig: &tls.Config{
			Certificates: []tls.Certificate{cert},
		},
	}

	log.Printf("[mock_owsec] Starting Fake owsec HTTPS Service on port %s...", port)
	if err := server.ListenAndServeTLS("", ""); err != nil {
		log.Fatalf("[mock_owsec] Server error: %v", err)
	}
}
