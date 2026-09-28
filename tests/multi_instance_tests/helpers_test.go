package multi_instance_tests

import (
	"database/sql"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	_ "github.com/lib/pq"
)

func getEnvOrDefault(key, defaultValue string) string {
	if val := os.Getenv(key); val != "" {
		return val
	}
	return defaultValue
}

func getDBConnStr(dbname string) string {
	host := getEnvOrDefault("PGHOST", "localhost")
	port := getEnvOrDefault("PGPORT", "5432")
	user := getEnvOrDefault("PGUSER", "postgres")
	pass := getEnvOrDefault("PGPASSWORD", "postgres")
	return fmt.Sprintf("host=%s port=%s user=%s password=%s dbname=%s sslmode=disable", host, port, user, pass, dbname)
}

func getOwprovBin() string {
	return getEnvOrDefault("OWPROV_BIN", "../../build/src/owprov")
}

func setupTestCaseDB(t *testing.T, tcSuffix string) string {
	baseDB := getEnvOrDefault("PGDATABASE", "owprov_test")
	tcDB := fmt.Sprintf("%s_%s", baseDB, tcSuffix)

	host := getEnvOrDefault("PGHOST", "localhost")
	port := getEnvOrDefault("PGPORT", "5432")
	user := getEnvOrDefault("PGUSER", "postgres")
	pass := getEnvOrDefault("PGPASSWORD", "postgres")

	connectAdminDB := func() (*sql.DB, error) {
		// First try connecting to baseDB
		connStr := fmt.Sprintf("host=%s port=%s user=%s password=%s dbname=%s sslmode=disable", host, port, user, pass, baseDB)
		db, err := sql.Open("postgres", connStr)
		if err == nil && db.Ping() == nil {
			return db, nil
		}
		if db != nil {
			_ = db.Close()
		}
		// Fallback: try connecting to postgres DB
		connStr = fmt.Sprintf("host=%s port=%s user=%s password=%s dbname=postgres sslmode=disable", host, port, user, pass)
		db, err = sql.Open("postgres", connStr)
		if err == nil && db.Ping() == nil {
			return db, nil
		}
		return db, err
	}

	adminDB, err := connectAdminDB()
	if err != nil || adminDB == nil {
		t.Fatalf("Failed to establish admin DB connection to set up %s: %v", tcDB, err)
	}
	defer adminDB.Close()

	if _, err := adminDB.Exec(fmt.Sprintf("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '%s' AND pid <> pg_backend_pid()", tcDB)); err != nil {
		t.Fatalf("Failed to terminate existing connections to %s: %v", tcDB, err)
	}
	if _, err := adminDB.Exec(fmt.Sprintf("DROP DATABASE IF EXISTS %s", tcDB)); err != nil {
		t.Fatalf("Failed to drop database %s: %v", tcDB, err)
	}
	if _, err := adminDB.Exec(fmt.Sprintf("CREATE DATABASE %s", tcDB)); err != nil {
		t.Fatalf("Failed to create isolated database %s: %v", tcDB, err)
	}

	// Verify that the new database can be connected to
	verifyDB, err := sql.Open("postgres", getDBConnStr(tcDB))
	if err != nil {
		t.Fatalf("Failed to open connection to verify %s: %v", tcDB, err)
	}
	defer verifyDB.Close()
	if err := verifyDB.Ping(); err != nil {
		t.Fatalf("Failed to ping freshly created database %s: %v", tcDB, err)
	}

	return tcDB
}

func writeTestConfig(targetPath string, dbname string, lockTimeoutSec int, restPort int, internalPort int, albPort int) error {
	host := getEnvOrDefault("PGHOST", "localhost")
	port := getEnvOrDefault("PGPORT", "5432")
	user := getEnvOrDefault("PGUSER", "postgres")
	pass := getEnvOrDefault("PGPASSWORD", "postgres")
	baseDir := filepath.Dir(targetPath)
	dataDir := filepath.Join(baseDir, "data")
	certsDir := filepath.Join(baseDir, "certs")

	_ = os.MkdirAll(dataDir, 0755)
	_ = os.MkdirAll(certsDir, 0755)
	_ = os.WriteFile(filepath.Join(dataDir, "registry.json"), []byte("{}"), 0644)

	certFile := filepath.Join(certsDir, "restapi-cert.pem")
	keyFile := filepath.Join(certsDir, "restapi-key.pem")
	caFile := filepath.Join(certsDir, "restapi-ca.pem")

	if _, err := os.Stat(keyFile); os.IsNotExist(err) {
		genCmd := exec.Command("openssl", "req", "-x509", "-newkey", "rsa:2048", "-passout", "pass:mypassword",
			"-keyout", keyFile, "-out", certFile, "-subj", "/CN=localhost")
		_ = genCmd.Run()
		_ = exec.Command("cp", certFile, caFile).Run()
	}

	content := fmt.Sprintf(`openwifi.restapi.host.0.backlog = 100
openwifi.restapi.host.0.security = relaxed
openwifi.restapi.host.0.rootca = %s
openwifi.restapi.host.0.address = *
openwifi.restapi.host.0.port = %d
openwifi.restapi.host.0.cert = %s
openwifi.restapi.host.0.key = %s
openwifi.restapi.host.0.key.password = mypassword

openwifi.internal.restapi.host.0.backlog = 100
openwifi.internal.restapi.host.0.security = relaxed
openwifi.internal.restapi.host.0.rootca = %s
openwifi.internal.restapi.host.0.address = *
openwifi.internal.restapi.host.0.port = %d
openwifi.internal.restapi.host.0.cert = %s
openwifi.internal.restapi.host.0.key = %s
openwifi.internal.restapi.host.0.key.password = mypassword

firmware.updater.upgrade = no
firmware.updater.rconly = no

openwifi.service.key = %s
openwifi.service.key.password = mypassword
openwifi.system.data = %s
openwifi.system.debug = true
openwifi.system.uri.private = https://localhost:%d
openwifi.system.uri.public = https://localhost:%d
openwifi.system.commandchannel = %s
openwifi.system.uri.ui = http://localhost
openwifi.security.restapi.disable = true

rrm.providers = owrrm

alb.enable = true
alb.port = %d

openwifi.kafka.group.id = prov
openwifi.kafka.client.id = prov_%d
openwifi.kafka.enable = false
openwifi.kafka.brokerlist = localhost:9092
openwifi.kafka.auto.commit = false
openwifi.kafka.queue.buffering.max.ms = 50

storage.type = postgresql
storage.type.postgresql.maxsessions = 64
storage.type.postgresql.idletime = 60
storage.type.postgresql.host = %s
storage.type.postgresql.username = %s
storage.type.postgresql.password = %s
storage.type.postgresql.database = %s
storage.type.postgresql.port = %s
storage.type.postgresql.connectiontimeout = 10
storage.startup.lock.timeout = %d

logging.type = console
logging.path = %s
logging.level = debug
`, caFile, restPort, certFile, keyFile,
		caFile, internalPort, certFile, keyFile,
		keyFile, dataDir, internalPort, restPort,
		filepath.Join(baseDir, "app.owprov"),
		albPort, restPort,
		host, user, pass, dbname, port, lockTimeoutSec,
		filepath.Join(baseDir, "logs"))

	return os.WriteFile(targetPath, []byte(content), 0644)
}
