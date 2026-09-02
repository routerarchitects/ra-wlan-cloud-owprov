# OpenWiFi Hierarchical RBAC Integration Test Suite (v1)

This directory contains the automated end-to-end integration test runner and test case definitions for validating OpenWiFi **Hierarchical Role-Based Access Control (RBAC)** scoped access control across `OWSEC` and `OWPROV` microservices.

## Overview

The integration test suite consists of **two complete test suites** (299 total test cases) covering root administrative access, scoped entity trees, fine-grained role permissions, auto-roles, delegation, and default system policy enforcement.

| Test Suite | CSV File | Test Cases | Target User Identity / Scope |
| :--- | :--- | :--- | :--- |
| **ROOT Suite** | `owprov_rbac_v1_root_lab_test_cases.csv` | **143** | Root user (`tip@ucentral.com`) full-scope system management & default policy verification |
| **Non-ROOT Admin Suite** | `owprov_rbac_v1_admin_lab_test_cases.csv` | **156** | Scoped administrative roles (`Admin Full`, `Admin Read`, `NOC`, `CSR`, `Installer`, `Accounting`) & sub-users |

---

## Test Suites Breakdown

### 1. ROOT User Test Suite (143 Test Cases)
- **Bootstrap System & Operators**: Root Entity (`0000-0000-0000`), Operators (`op-l1-a`, `op-l1-b`), Normal Entities, Top-level & Child Venues, Inventory, Configurations.
- **System Default Policies Assignment**: Assigns system default policies (`Admin`, `NOC`, `CSR`, `Installer`) to entity-scoped users and verifies operational boundaries.
- **Read & Update Operations**: Complete attribute inspection and modification across all resource types under ROOT privileges.
- **Delegation & Sub-Users**: Sub-user creation and role delegation under non-root admin accounts.
- **Auto-Role & Shadow Precedence**: Automatic venue role creation upon top-level venue creation and narrow venue-scoped role precedence over entity roles.
- **Constraint & Edge Cases**: Input validation for invalid parent UUIDs, duplicate emails, weak passwords, empty names, and illegal policy deletion blocks.
- **Teardown**: Dependency-ordered deletion of created test objects.

### 2. Non-ROOT Admin & Operational Test Suite (156 Test Cases)
- **Role Provisioning**: ROOT provisions Custom Policies (`ADMIN-FULL`, `ADMIN-READ`, `NOC`, `CSR`, `INSTALLER`, `ACCOUNTING`) and binds corresponding users.
- **Role Enforcement Validation**:
  - `ADMIN-FULL`: Full CRUD operations within entity subtree.
  - `ADMIN-READ`: Read access permitted; write/delete operations blocked (`401/403 Access Denied`).
  - `NOC`: Read on entities/venues, update on inventory/configs; entity creation blocked.
  - `CSR`: Read access on entities/venues; write/delete operations blocked.
  - `INSTALLER`: Read on venues/inventory, update on inventory; entity creation blocked.
  - `ACCOUNTING`: Read access on entities/venues; write/delete operations blocked.
- **Sub-User Scoped Delegation**: Verification that sub-users inherit parent role boundaries.
- **System Default Policies Verification**: Verifies non-root users assigned default system roles strictly respect policy access limits.
- **Teardown**: Complete dependency-ordered cleanup by ROOT.

---

## Prerequisites

1. **Running Microservices**:
   - `OWSEC` (Security Microservice) listening on `https://localhost:16001`
   - `OWPROV` (Provisioning Microservice) listening on `https://localhost:16005`
2. **Environment Toolchain**:
   - Go `1.18+` installed on host.
   - Python `3.x` for CSV generation.

---

## Environment Variables & Configuration

The test runner dynamically loads configurations from environment variables (or `.env` file). The root password defaults to `Iotina@123` with automated fallback to initial boot password (`openwifi` -> `Iotina@123`).

| Variable | Default Value | Description |
| :--- | :--- | :--- |
| `OWSEC_BASE_URL` | `https://localhost:16001/api/v1` | OWSEC Security REST API base URL |
| `OWPROV_BASE_URL` | `https://localhost:16005/api/v1` | OWPROV Provisioning REST API base URL |
| `OWSEC_ROOT_EMAIL` | `tip@ucentral.com` | ROOT account email address |
| `OWSEC_ROOT_PASSWORD` | `Iotina@123` | ROOT account password |
| `TEST_CSV` | `owprov_rbac_v1_root_lab_test_cases.csv` | Path to active CSV test cases file |
| `RESULTS_CSV` | `rbac_root_actual_results.csv` | Output results CSV file |

---

## How to Run

### Step 1: Generate Test Case CSV Files
```bash
python3 build_csv.py
```
*Generates `owprov_rbac_v1_root_lab_test_cases.csv` and `owprov_rbac_v1_admin_lab_test_cases.csv`.*

### Step 2: Run ROOT User Test Suite (143 Test Cases)
```bash
TEST_CSV=owprov_rbac_v1_root_lab_test_cases.csv RESULTS_CSV=rbac_root_actual_results.csv go run main.go
```

### Step 3: Run Non-ROOT Admin Test Suite (156 Test Cases)
```bash
TEST_CSV=owprov_rbac_v1_admin_lab_test_cases.csv RESULTS_CSV=rbac_admin_actual_results.csv go run main.go
```

---

## File Structure

- `build_csv.py`: Python generator script for producing `owprov_rbac_v1_root_lab_test_cases.csv` and `owprov_rbac_v1_admin_lab_test_cases.csv`.
- `main.go`: High-performance Go integration test runner (OAuth2 authentication, TLS handling, placeholder extraction, status code assertions, summary logging).
- `owprov_rbac_v1_root_lab_test_cases.csv`: 143 test cases for ROOT user operations.
- `owprov_rbac_v1_admin_lab_test_cases.csv`: 156 test cases for Non-ROOT Admin and operational role operations.
- `README.md`: Integration test suite documentation.
- `go.mod`: Go module declaration.
