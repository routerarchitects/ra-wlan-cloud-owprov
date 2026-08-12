package main

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"time"
)

func generateSelfSignedCert() (tls.Certificate, error) {
	priv, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return tls.Certificate{}, err
	}

	template := x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject: pkix.Name{
			Organization: []string{"OpenWifi Fake FMS"},
			CommonName:   "localhost",
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
		port = "16004"
	}

	mux := http.NewServeMux()

	// Handler for firmwares / device types
	mux.HandleFunc("/api/v1/firmwares", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)

		resp := map[string]interface{}{
			"deviceTypes": []string{
				"openwrt_generic",
				"edgecore_eap101",
				"edgecore_eap102",
				"edgecore_ecs4100",
				"indio_um305ac",
				"cig_wf188",
				"wlan_ap",
				"generic",
				"tplink_archer",
			},
			"firmwares": []map[string]interface{}{
				{
					"deviceType": "generic",
					"revision":   "1.0.0",
					"uri":        "https://firmware.openwifi.local/generic-1.0.0.bin",
				},
			},
		}
		_ = json.NewEncoder(w).Encode(resp)
	})

	cert, err := generateSelfSignedCert()
	if err != nil {
		log.Fatalf("[mock_owfms] Failed to generate self-signed cert: %v", err)
	}

	server := &http.Server{
		Addr:    ":" + port,
		Handler: mux,
		TLSConfig: &tls.Config{
			Certificates: []tls.Certificate{cert},
		},
	}

	log.Printf("[mock_owfms] Starting Fake owfms HTTPS Service on port %s...", port)
	if err := server.ListenAndServeTLS("", ""); err != nil {
		log.Fatalf("[mock_owfms] Server error: %v", err)
	}
}
