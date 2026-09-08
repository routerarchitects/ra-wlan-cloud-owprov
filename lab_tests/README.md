# OpenWiFi Hierarchical RBAC Integration Test Suite (v1) - ROOT Suite

This directory contains the automated end-to-end integration test runner and test case definitions for validating OpenWiFi Hierarchical RBAC scoped access control across `OWSEC` and `OWPROV` microservices under the `ROOT` user identity.

## Overview

The ROOT test suite validates **102 complete test scenarios** covering:

- **Entity & Operator Hierarchy**: Creating and verifying Root Entity, Operator Entities (`op-l1-a`, `op-l1-b`), and 3-level Entity tree limits.
- **Venue & Device Scoping**: Managing top-level venues, nested child venues, inventory devices (`devClass: venue`), and configuration profiles (`asus_rt-ax53u`).
- **Policy & Role Bindings**: Creating management policies, management roles, venue-scoped roles, and testing role delegation.
- **Shadow Role Precedence**: Verifying that narrow venue-scoped roles take precedence over broad entity roles (`ROOT-SHADOW-0001`, `ROOT-SHADOW-0002`).
- **Subscriber Portal Operations**: Validating subscriber signup (`/api/v1/signup`), listing (`/api/v1/subusers`), updating, and deletion (`/api/v1/subuser/{id}`).
- **Cascade Teardown Cleanup**: Dependency-ordered deletion of all created test entities.

---

## Prerequisites

1. **Running Microservices**:
   - `OWSEC` (Security Microservice) listening on `https://localhost:16001`
   - `OWPROV` (Provisioning Microservice) listening on `https://localhost:16005`
2. **Go Toolchain**:
   - Go `1.18+` installed on host.

---

## Environment Variables & `.env` Setup

The test runner dynamically loads configurations from environment variables (or `.env` file). The root password is configurable and defaults to `Iotina@123` with automated fallback to first-time boot password initialization (`openwifi` -> `Iotina@123`).

| Variable | Default Value | Description |
| :--- | :--- | :--- |
| `OWSEC_BASE_URL` | `https://localhost:16001/api/v1` | OWSEC Security REST API base URL |
| `OWPROV_BASE_URL` | `https://localhost:16005/api/v1` | OWPROV Provisioning REST API base URL |
| `OWSEC_ROOT_EMAIL` | `tip@ucentral.com` | ROOT account email address |
| `OWSEC_ROOT_PASSWORD` | `Iotina@123` | ROOT account password |
| `TEST_CSV` | `owprov_rbac_v1_root_lab_test_cases.csv` | Path to CSV test cases file |
| `RESULTS_CSV` | `rbac_root_actual_results.csv` | Output results CSV file |

### Setting Up `.env` (Optional)

You can create or modify a `.env` file in the test directory to customize your local lab credentials:

```bash
OWSEC_BASE_URL=https://localhost:16001/api/v1
OWPROV_BASE_URL=https://localhost:16005/api/v1
OWSEC_ROOT_EMAIL=tip@ucentral.com
OWSEC_ROOT_PASSWORD=Iotina@123
```

---

## How to Run

### Execute the ROOT User Test Suite (102 Test Cases)

```bash
TEST_CSV=owprov_rbac_v1_root_lab_test_cases.csv go run main.go
```

* **Output Results**: Detailed test execution output is logged to stdout and written to `rbac_root_actual_results.csv`.

---

## File Structure

- `main.go`: Go test runner engine (handles OAuth2 authentication, TLS communication, dynamic ID placeholder substitution, status code assertions, and result logging).
- `owprov_rbac_v1_root_lab_test_cases.csv`: 102 test case definitions for ROOT user operations.
- `go.mod`: Go module definition.
