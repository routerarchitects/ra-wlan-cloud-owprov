package multi_instance_tests

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	_ "github.com/lib/pq"
)

const (
	LockKeyDec = "5717126566418710529" // 0x4F5750524F560001LL in decimal
)

// SQL query matching PostgreSQL 64-bit bigint advisory lock key splitting across classid and objid
const PGLocksCountQuery = "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND objsubid = 1 AND ((classid::bigint << 32) | (objid::bigint & 4294967295)) = " + LockKeyDec + " AND granted = true"

/*
 * TestPostgresAdvisoryLock_Case1_ConcurrentCoordination
 *
 * DESCRIPTION:
 *   Validates that 5 real OWPROV processes starting simultaneously against PostgreSQL
 *   acquire the startup advisory lock sequentially and complete DB initialization.
 */
func TestPostgresAdvisoryLock_Case1_ConcurrentCoordination(t *testing.T) {
	owprovBin := getOwprovBin()
	if _, err := os.Stat(owprovBin); os.IsNotExist(err) {
		t.Skipf("OWPROV binary not found at %s. Skipping real binary test (set OWPROV_BIN to run).", owprovBin)
	}

	tcDB := setupTestCaseDB(t, "tc1")
	const instanceCount = 5
	tmpDir, err := os.MkdirTemp("", "owprov_go_tc1_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	var wg sync.WaitGroup
	var successCount int32

	t.Logf("Launching %d real OWPROV instances using binary %s against DB %s...", instanceCount, owprovBin, tcDB)

	for i := 1; i <= instanceCount; i++ {
		wg.Add(1)
		go func(instanceID int) {
			defer wg.Done()

			instDir := filepath.Join(tmpDir, fmt.Sprintf("inst_%d", instanceID))
			_ = os.MkdirAll(instDir, 0755)
			configPath := filepath.Join(instDir, "owprov.properties")
			logPath := filepath.Join(instDir, "inst.log")

			if err := writeTestConfig(configPath, tcDB, 60, 16000+instanceID, 17000+instanceID, 18000+instanceID); err != nil {
				t.Errorf("Failed to write config for instance %d: %v", instanceID, err)
				return
			}

			ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
			defer cancel()

			cmd := exec.CommandContext(ctx, owprovBin, "--file="+configPath)
			logFile, err := os.Create(logPath)
			if err != nil {
				t.Errorf("Failed to create log file for instance %d: %v", instanceID, err)
				return
			}
			cmd.Stdout = logFile
			cmd.Stderr = logFile

			if err := cmd.Start(); err != nil {
				_ = logFile.Close()
				t.Errorf("Failed to start instance %d: %v", instanceID, err)
				return
			}
			_ = logFile.Close()
			defer func() {
				if cmd.Process != nil {
					_ = cmd.Process.Kill()
					_ = cmd.Wait()
				}
			}()

			deadline := time.Now().Add(80 * time.Second)
			for time.Now().Before(deadline) {
				data, err := os.ReadFile(logPath)
				if err == nil && len(data) > 0 {
					if strings.Contains(string(data), "PostgreSQL startup advisory lock released") {
						atomic.AddInt32(&successCount, 1)
						t.Logf("Instance %d successfully acquired and released startup lock.", instanceID)
						return
					}
				}
				time.Sleep(300 * time.Millisecond)
			}

			data, _ := os.ReadFile(logPath)
			t.Errorf("Instance %d timed out waiting for startup advisory lock release log. Log:\n%s", instanceID, string(data))
		}(i)
	}

	wg.Wait()

	if successCount != int32(instanceCount) {
		t.Fatalf("Expected %d successful OWPROV instance startups, got %d", instanceCount, successCount)
	}
	t.Log("PASS: All 5 real OWPROV instances coordinated startup lock acquisition cleanly.")
}

/*
 * TestPostgresAdvisoryLock_Case2_LockExclusivity
 *
 * DESCRIPTION:
 *   Launches 5 real OWPROV binary processes concurrently and monitors pg_locks
 *   using the corrected classid/objid query for 5717126566418710529.
 *   Asserts that at no point do multiple processes hold the startup lock simultaneously.
 */
func TestPostgresAdvisoryLock_Case2_LockExclusivity(t *testing.T) {
	owprovBin := getOwprovBin()
	if _, err := os.Stat(owprovBin); os.IsNotExist(err) {
		t.Skipf("OWPROV binary not found at %s. Skipping real binary test (set OWPROV_BIN to run).", owprovBin)
	}

	tcDB := setupTestCaseDB(t, "tc2")
	const instanceCount = 5
	tmpDir, err := os.MkdirTemp("", "owprov_go_tc2_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	var cmds []*exec.Cmd
	var logs []string

	t.Logf("Launching %d real OWPROV processes to verify lock exclusivity in pg_locks on DB %s...", instanceCount, tcDB)

	for i := 1; i <= instanceCount; i++ {
		instDir := filepath.Join(tmpDir, fmt.Sprintf("inst_%d", i))
		_ = os.MkdirAll(instDir, 0755)
		cfgPath := filepath.Join(instDir, "owprov.properties")
		logPath := filepath.Join(instDir, "inst.log")
		logs = append(logs, logPath)

		if err := writeTestConfig(cfgPath, tcDB, 60, 16100+i, 17100+i, 18100+i); err != nil {
			t.Fatalf("Failed to write config for instance %d: %v", i, err)
		}

		cmd := exec.Command(owprovBin, "--file="+cfgPath)
		f, err := os.Create(logPath)
		if err != nil {
			t.Fatalf("Failed to create log for instance %d: %v", i, err)
		}
		cmd.Stdout = f
		cmd.Stderr = f

		if err := cmd.Start(); err != nil {
			_ = f.Close()
			t.Fatalf("Failed to start instance %d: %v", i, err)
		}
		_ = f.Close()
		cmds = append(cmds, cmd)
	}

	defer func() {
		for _, c := range cmds {
			if c.Process != nil {
				_ = c.Process.Kill()
				_ = c.Wait()
			}
		}
	}()

	monitorDB, err := sql.Open("postgres", getDBConnStr(tcDB))
	if err != nil {
		t.Fatalf("Failed to open monitor DB connection: %v", err)
	}
	defer monitorDB.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 40*time.Second)
	defer cancel()
	ticker := time.NewTicker(30 * time.Millisecond)
	defer ticker.Stop()

monitorLoop:
	for {
		select {
		case <-ctx.Done():
			break monitorLoop
		case <-ticker.C:
			var count int
			err := monitorDB.QueryRow(PGLocksCountQuery).Scan(&count)
			if err == nil {
				if count > 1 {
					t.Fatalf("MUTEX VIOLATION: Observed %d concurrent locks held by real OWPROV processes!", count)
				}
			}

			// Check if all instances have completed lock lifecycle to break early
			allReleased := true
			for _, logP := range logs {
				data, err := os.ReadFile(logP)
				if err != nil || !strings.Contains(string(data), "PostgreSQL startup advisory lock released") {
					allReleased = false
					break
				}
			}
			if allReleased {
				break monitorLoop
			}
		}
	}

	// Verify all instances completed lock lifecycle
	var acquiredCount, releasedCount int
	for i, logP := range logs {
		data, err := os.ReadFile(logP)
		if err == nil {
			logStr := string(data)
			if strings.Contains(logStr, "PostgreSQL startup advisory lock acquired") {
				acquiredCount++
			}
			if strings.Contains(logStr, "PostgreSQL startup advisory lock released") {
				releasedCount++
			}
		} else {
			t.Errorf("Failed to read log for instance %d: %v", i+1, err)
		}
	}

	if acquiredCount != instanceCount || releasedCount != instanceCount {
		t.Fatalf("Expected all %d instances to acquire and release lock, got acquired=%d released=%d",
			instanceCount, acquiredCount, releasedCount)
	}
	t.Logf("PASS: Real OWPROV processes maintained strict lock exclusivity without mutex violation.")
}

/*
 * TestPostgresAdvisoryLock_Case3_CrashRecovery
 *
 * DESCRIPTION:
 *   Launches 5 real OWPROV processes, dynamically identifies the PID of the process
 *   currently holding the startup advisory lock, kills that real process with SIGKILL,
 *   and verifies that the remaining 4 real OWPROV processes recover and complete startup.
 */
func TestPostgresAdvisoryLock_Case3_CrashRecovery(t *testing.T) {
	owprovBin := getOwprovBin()
	if _, err := os.Stat(owprovBin); os.IsNotExist(err) {
		t.Skipf("OWPROV binary not found at %s. Skipping real binary test (set OWPROV_BIN to run).", owprovBin)
	}

	tcDB := setupTestCaseDB(t, "tc3")
	const instanceCount = 5
	tmpDir, err := os.MkdirTemp("", "owprov_go_tc3_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	var cmds []*exec.Cmd
	var logs []string

	t.Logf("Launching %d real OWPROV processes for crash recovery test on DB %s...", instanceCount, tcDB)

	for i := 1; i <= instanceCount; i++ {
		instDir := filepath.Join(tmpDir, fmt.Sprintf("inst_%d", i))
		_ = os.MkdirAll(instDir, 0755)
		cfgPath := filepath.Join(instDir, "owprov.properties")
		logPath := filepath.Join(instDir, "inst.log")
		logs = append(logs, logPath)

		if err := writeTestConfig(cfgPath, tcDB, 60, 16200+i, 17200+i, 18200+i); err != nil {
			t.Fatalf("Failed to write config for instance %d: %v", i, err)
		}

		cmd := exec.Command(owprovBin, "--file="+cfgPath)
		f, err := os.Create(logPath)
		if err != nil {
			t.Fatalf("Failed to create log for instance %d: %v", i, err)
		}
		cmd.Stdout = f
		cmd.Stderr = f

		if err := cmd.Start(); err != nil {
			_ = f.Close()
			t.Fatalf("Failed to start instance %d: %v", i, err)
		}
		_ = f.Close()
		cmds = append(cmds, cmd)
	}

	defer func() {
		for _, c := range cmds {
			if c.Process != nil {
				_ = c.Process.Kill()
				_ = c.Wait()
			}
		}
	}()

	// Dynamically identify which process acquired the lock and has NOT released it
	holderIdx := -1
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		for i, logP := range logs {
			data, err := os.ReadFile(logP)
			if err == nil {
				logStr := string(data)
				if strings.Contains(logStr, "PostgreSQL startup advisory lock acquired") &&
					!strings.Contains(logStr, "PostgreSQL startup advisory lock released") {
					holderIdx = i
					break
				}
			}
		}
		if holderIdx != -1 {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}

	if holderIdx == -1 {
		t.Fatalf("Failed to dynamically identify lock-holding OWPROV process.")
	}

	t.Logf("Identified Lock Holder: Real OWPROV Instance %d (PID %d). Sending SIGKILL...", holderIdx+1, cmds[holderIdx].Process.Pid)
	_ = cmds[holderIdx].Process.Kill()
	_ = cmds[holderIdx].Wait()

	// Verify remaining 4 real OWPROV processes complete startup
	recDeadline := time.Now().Add(35 * time.Second)
	recoveredCount := 0

	for time.Now().Before(recDeadline) {
		recoveredCount = 0
		for i, logP := range logs {
			if i != holderIdx {
				data, err := os.ReadFile(logP)
				if err == nil && strings.Contains(string(data), "PostgreSQL startup advisory lock released") {
					recoveredCount++
				}
			}
		}
		if recoveredCount == instanceCount-1 {
			break
		}
		time.Sleep(200 * time.Millisecond)
	}

	if recoveredCount != instanceCount-1 {
		t.Fatalf("Expected %d real OWPROV processes to recover after holder crash, got %d", instanceCount-1, recoveredCount)
	}
	t.Logf("PASS: Remaining %d real OWPROV processes recovered cleanly after holder process crash.", recoveredCount)
}

/*
 * TestPostgresAdvisoryLock_Case4_LockContentionTimeout
 *
 * DESCRIPTION:
 *   Holds the PostgreSQL advisory lock externally via SQL, launches 5 real OWPROV processes
 *   with lock timeout = 3s, and asserts that all 5 real OWPROV processes log the explicit
 *   contention timeout error message and exit.
 */
func TestPostgresAdvisoryLock_Case4_LockContentionTimeout(t *testing.T) {
	owprovBin := getOwprovBin()
	if _, err := os.Stat(owprovBin); os.IsNotExist(err) {
		t.Skipf("OWPROV binary not found at %s. Skipping real binary test (set OWPROV_BIN to run).", owprovBin)
	}

	tcDB := setupTestCaseDB(t, "tc4")
	blockerDB, err := sql.Open("postgres", getDBConnStr(tcDB))
	if err != nil {
		t.Fatalf("Failed to open blocker DB: %v", err)
	}
	defer blockerDB.Close()

	// Pin to a dedicated connection so both lock acquisition and release occur on the same backend session
	conn, err := blockerDB.Conn(context.Background())
	if err != nil {
		t.Fatalf("Failed to acquire dedicated DB connection: %v", err)
	}
	defer conn.Close()

	var acquired bool
	err = conn.QueryRowContext(context.Background(), "SELECT pg_try_advisory_lock("+LockKeyDec+")").Scan(&acquired)
	if err != nil || !acquired {
		t.Fatalf("Failed to acquire external blocking lock: %v", err)
	}
	defer func() {
		var unl bool
		_ = conn.QueryRowContext(context.Background(), "SELECT pg_advisory_unlock("+LockKeyDec+")").Scan(&unl)
	}()

	const instanceCount = 5
	tmpDir, err := os.MkdirTemp("", "owprov_go_tc4_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	var cmds []*exec.Cmd
	var logs []string

	t.Logf("Launching %d real OWPROV processes with lock timeout = 3s while lock is held on DB %s...", instanceCount, tcDB)

	for i := 1; i <= instanceCount; i++ {
		instDir := filepath.Join(tmpDir, fmt.Sprintf("inst_%d", i))
		_ = os.MkdirAll(instDir, 0755)
		cfgPath := filepath.Join(instDir, "owprov.properties")
		logPath := filepath.Join(instDir, "inst.log")
		logs = append(logs, logPath)

		if err := writeTestConfig(cfgPath, tcDB, 3, 16300+i, 17300+i, 18300+i); err != nil {
			t.Fatalf("Failed to write config for instance %d: %v", i, err)
		}

		cmd := exec.Command(owprovBin, "--file="+cfgPath)
		f, err := os.Create(logPath)
		if err != nil {
			t.Fatalf("Failed to create log for instance %d: %v", i, err)
		}
		cmd.Stdout = f
		cmd.Stderr = f

		if err := cmd.Start(); err != nil {
			_ = f.Close()
			t.Fatalf("Failed to start instance %d: %v", i, err)
		}
		_ = f.Close()
		cmds = append(cmds, cmd)
	}

	defer func() {
		for _, c := range cmds {
			if c.Process != nil {
				_ = c.Process.Kill()
				_ = c.Wait()
			}
		}
	}()

	deadline := time.Now().Add(15 * time.Second)
	contentionLoggedCount := 0

	for time.Now().Before(deadline) {
		contentionLoggedCount = 0
		for _, logP := range logs {
			data, err := os.ReadFile(logP)
			if err == nil && strings.Contains(string(data), "lock is held by another instance (observed contention)") {
				contentionLoggedCount++
			}
		}
		if contentionLoggedCount == instanceCount {
			break
		}
		time.Sleep(300 * time.Millisecond)
	}

	if contentionLoggedCount != instanceCount {
		t.Fatalf("Expected all %d real OWPROV processes to log contention timeout, got %d", instanceCount, contentionLoggedCount)
	}
	t.Log("PASS: All 5 real OWPROV processes correctly classified and logged lock contention timeout.")
}

/*
 * TestPostgresAdvisoryLock_Case5_CleanAcquireReleaseLogVerification
 *
 * DESCRIPTION:
 *   Launches a real OWPROV process and verifies log evidence for both explicit
 *   lock acquisition and clean unlock on its dedicated session.
 */
func TestPostgresAdvisoryLock_Case5_CleanAcquireReleaseLogVerification(t *testing.T) {
	owprovBin := getOwprovBin()
	if _, err := os.Stat(owprovBin); os.IsNotExist(err) {
		t.Skipf("OWPROV binary not found at %s. Skipping real binary test (set OWPROV_BIN to run).", owprovBin)
	}

	tcDB := setupTestCaseDB(t, "tc5")
	tmpDir, err := os.MkdirTemp("", "owprov_go_tc5_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	instDir := filepath.Join(tmpDir, "inst_single")
	_ = os.MkdirAll(instDir, 0755)
	cfgPath := filepath.Join(instDir, "owprov.properties")
	logPath := filepath.Join(instDir, "inst.log")

	if err := writeTestConfig(cfgPath, tcDB, 30, 16400, 17400, 18400); err != nil {
		t.Fatalf("Failed to write config: %v", err)
	}

	cmd := exec.Command(owprovBin, "--file="+cfgPath)
	f, err := os.Create(logPath)
	if err != nil {
		t.Fatalf("Failed to create log file: %v", err)
	}
	cmd.Stdout = f
	cmd.Stderr = f

	if err := cmd.Start(); err != nil {
		_ = f.Close()
		t.Fatalf("Failed to start instance: %v", err)
	}
	_ = f.Close()
	defer func() {
		if cmd.Process != nil {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
		}
	}()

	deadline := time.Now().Add(30 * time.Second)
	success := false

	for time.Now().Before(deadline) {
		data, err := os.ReadFile(logPath)
		if err == nil {
			logStr := string(data)
			if strings.Contains(logStr, "PostgreSQL startup advisory lock acquired") &&
				strings.Contains(logStr, "PostgreSQL startup advisory lock released") {
				success = true
				break
			}
		}
		time.Sleep(300 * time.Millisecond)
	}

	if !success {
		data, _ := os.ReadFile(logPath)
		t.Fatalf("Real OWPROV log missing dedicated session lock acquisition or release evidence. Log:\n%s", string(data))
	}

	t.Log("PASS: Real OWPROV binary acquired and cleanly released lock on dedicated session.")
}
