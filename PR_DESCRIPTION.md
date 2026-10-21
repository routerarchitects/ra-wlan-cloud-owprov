# Upgrade OWProv Docker Environment to Debian 13

## Summary
This PR upgrades the OWProv Docker build and runtime environment from **Debian 11.5-slim** to **Debian 13-slim**.

The change updates Debian package dependencies for Debian 13 compatibility, adds necessary standard library headers for GCC 14 compatibility, reduces development packages in the runtime image, improves APT reliability and cleanup, pre-creates required Kafka topics for librdkafka 2.x in CI, and adds an active microservice registration retry loop in CI so that the OWProv build, service startup, brownfield database migrations, and integration test suites pass reliably without altering production Kafka runtime settings.

## Changes

### Debian 13-slim Base Image
Updated the Docker base image from:
```dockerfile
ARG DEBIAN_VERSION=11.5-slim
```
to:
```dockerfile
ARG DEBIAN_VERSION=13-slim
```
Using `13-slim` tracks the active Debian 13 upstream release and ensures base image security patches and updates are automatically applied. The same base image is used across both build and runtime stages.

### Build Dependency Updates
Updated Debian package names for compatibility with Debian 13 and removed obsolete workarounds.

- Replaced:
  ```text
  libmariadbclient-dev-compat
  ```
  with:
  ```text
  libmariadb-dev-compat
  ```
- Removed the deprecated `bullseye-security` repository `sed` removal workaround.
- The build image continues to install the required development dependencies for:
  - PostgreSQL (`libpq-dev`)
  - MariaDB (`libmariadb-dev`, `libmariadb-dev-compat`)
  - librdkafka (`librdkafka-dev`)
  - Boost (`libboost-all-dev`)
  - OpenSSL (`libssl-dev`)
  - zlib (`zlib1g-dev`)
  - nlohmann-json (`nlohmann-json3-dev`)
  - curl (`libcurl4-openssl-dev`)
  - fmt (`libfmt-dev`)
  - POCO
  - cppkafka
  - Valijson

### Runtime Dependency Updates
Reduced development packages in the final runtime image where only shared runtime libraries are required.

Updated:
```text
libmariadb-dev-compat
libfmt7
```
to:
```text
libmariadb3
libfmt10
```
This minimizes the final runtime image attack surface and keeps it focused strictly on libraries required by the OWProv daemon.

### APT Reliability and Cleanup
APT commands now use retry handling:
```text
-o Acquire::Retries=5
```
and clean up package metadata after installation:
```bash
rm -rf /var/lib/apt/lists/*
```
This improves network resilience during package installation while eliminating unnecessary APT cache layers in the resulting image.

### Shared Library Discovery
Added:
```dockerfile
RUN ldconfig
```
after copying locally built POCO and cppkafka libraries into `/usr/local/lib`. This ensures the linker cache is refreshed prior to building OWProv.

### Compiler Compatibility (GCC 14 / Debian 13)
GCC 14 in Debian 13 no longer transitively includes `<cstdint>` and `<optional>`. Added explicit standard includes:
- `#include <cstdint>` in:
  - `src/framework/OpenWifiTypes.h`
  - `src/libs/croncpp.h`
  (for `uint32_t`, `uint64_t`, and `int64_t`)
- `#include <optional>` in:
  - `src/framework/MicroService.h`
  - `src/framework/MicroServiceFuncs.h`
  - `src/framework/MicroServiceFuncs.cpp`
  (for `std::optional`)

## CI & Kafka Compatibility (Debian 13 / librdkafka 2.x)

As part of the Debian 13 upgrade, `librdkafka` is upgraded from 1.6.x to 2.x. To maintain strict parity with production configuration and avoid altering production Kafka settings (`src/framework/KafkaManager.cpp` remains 100% untouched):

1. **Pre-creation of standard Kafka topics in CI**:
   - In librdkafka 2.x, `allow.auto.create.topics` defaults to `false`. When `owprov` boots and subscribes to its standard microservice event topics, librdkafka rejects subscriptions with `Broker: Unknown topic or partition` if those topics do not exist.
   - Pre-creating the 9 standard service topics (`command`, `connection`, `device_event_queue`, `device_telemetry`, `healthcheck`, `provisioning_change`, `service_events`, `state`, `wifiscan`) in CI mirrors production infrastructure and satisfies librdkafka 2.x requirements without modifying application defaults.

2. **Continuous microservice registration in CI**:
   - In production, OpenWiFi microservices continuously broadcast periodic heartbeat/join events via `EventBusManager`.
   - In CI, lightweight mock services (`mock_owsec`, `mock_fms`) do not run background heartbeat threads. Following the container restart in Step 16 (which tests daemon restart idempotency), a Kafka consumer rebalance occurs (5–15 seconds).
   - Under standard `auto.offset.reset = "latest"`, a single one-shot message published during rebalance is missed, causing `owprov` to never register `owsec` and return HTTP 403 on subsequent authenticated test requests.
   - Step 17 now polls and republishes `join` events in a 30-second loop until `owprov` logs confirm active registration of **both** `owsec` and `owfms`. This reliably accommodates consumer rebalancing under standard `auto.offset.reset = "latest"` without race conditions or false negatives.

3. **Runtime Shared-Library Validation**:
   - Added explicit shared-library dependency verification to ensure no dynamic symbols are missing:
   ```bash
   docker run --rm --entrypoint /bin/bash owprov:test \
     -c 'ldd /openwifi/owprov && ! ldd /openwifi/owprov | grep -q "not found"'
   ```

## Production Integrity & Non-Regressions
- `src/framework/KafkaManager.cpp` remains **100% identical to `origin/main`** (0 diff).
- Production consumer behavior is fully preserved (`auto.offset.reset = "latest"`, `allow.auto.create.topics` unchanged).
- No workarounds or test-specific configuration flags were injected into production source code.

## Validation
Validated against Debian 13-slim:
- Build OWProv C++ Docker image: passed
- Runtime shared-library (`ldd`) validation: passed
- Start mock services (`mock_owsec` on port 16001, `mock_fms` on port 16004): passed
- PostgreSQL container startup: passed
- Brownfield schema migration (v1 → v2) and corrupt role auto-purge: passed
- Daemon restart idempotency: passed
- Dual microservice registration (`owsec` + `owfms`): passed
- Combined Lab Test Suite (`main.go` - Root + Admin): passed
- RBAC Immutability, Policy Protection, and v1/v2 role tests: passed

## Scope
This PR is limited to the Debian 13 migration, compiler compatibility, and CI integration test reliability.

It does not change:
- OWProv REST API endpoints or behavior
- Database schema migration logic or entity structures
- Management role or policy RBAC enforcement
- Production Kafka consumer or producer configuration

## Expected Result
After this PR is merged:
- OWProv build containers use Debian 13-slim.
- OWProv runtime containers use Debian 13-slim.
- Debian 13-compatible MariaDB and fmt packages are used.
- Runtime images avoid unnecessary development packages.
- GCC 14 compilation warnings and missing header errors are resolved.
- CI builds and validates OWProv using Debian 13-slim.
- Runtime shared-library validation executes as part of CI.
- All lab and RBAC integration tests continue to pass reliably.

