# Tier 1 subscriber RBAC verification

These tests cover the five active endpoints in `requirements/subscriber_rbac.md`.
Tier 2 direct profile and venue capabilities remain denied until their follow-up
implementation. Existing UserPortal collection calls using internal credentials
retain their existing handler path; the HTTP tests below deliberately use Bearer
tokens to exercise subscriber authorization.

## Local capability and database checks

With the normal build dependencies installed:

```sh
cmake --build <build-directory> --target subscriber_access_test
ctest --test-dir <build-directory> -R '^subscriber_access_test$' --output-on-failure
```

The same test can run independently of Kafka and the application build:

```sh
c++ -std=c++17 -Isrc tests/framework_tests/SubscriberAccess_test.cpp \
  -o /tmp/owprov-subscriber-access-test \
  -lPocoDataSQLite -lPocoData -lPocoNet -lPocoFoundation -lfmt
/tmp/owprov-subscriber-access-test
```

It checks exact method/path grants, foreign account deletion denial, administrative
field restrictions, query-string resource spoofing, subscriber-isolated SQL
pagination, empty pages, and database count/retrieval failures using SQLite.
The production venue-eligibility helper is also checked with 0, 100, 101 and 201
venues, a populated third page, and failures on later or empty final pages.

## HTTP integration checks

Run against a test deployment containing a subscriber with a valid token:

```sh
cd tests/subscriber_rbac
export OWPROV_SUBSCRIBER_URL=https://owprov.test:16005/api/v1
export SUBSCRIBER_TOKEN='<subscriber bearer token>'
export SUBSCRIBER_ID='<authenticated subscriber UUID>'
go test -v ./...
```

The suite skips when these three variables are absent. It uses Go 1.21 or later
and has no third-party dependencies. Set `OWPROV_TEST_INSECURE_TLS=1` only for a
test deployment using a self-signed certificate.

Optional fixture variables:

| Variable | Purpose |
|---|---|
| `SUBSCRIBER_DEVICE_ID` | Existing owned device UUID or serial; enables positive lookup. |
| `FOREIGN_SUBSCRIBER_DEVICE_ID` | Device owned by another subscriber; verifies identical missing/foreign GET and DELETE responses. |
| `SUBSCRIBER_CLAIM_SERIAL` | Available inventory serial dedicated to a claim/unclaim test; cleanup unclaims the device. |
| `SUBSCRIBER_CLAIM_GROUP` | `olg` (default, for an empty account) or `ap` (for an account with a root gateway). |
| `SUBSCRIBER_CLAIM_CONFIGURATION` | Initial configuration array in JSON, matching the UserPortal onboarding payload. |
| `SUBSCRIBER_DELETE_ACCOUNT` | Set to `1` only to delete the disposable fixture account after the other tests. |
| `OWPROV_INTERNAL_URL`, `OWPROV_INTERNAL_KEY`, `OWPROV_INTERNAL_NAME` | Enable internal registration routing checks: invalid or missing credentials reject even with a subscriber Bearer token; valid credentials reach the existing signup validation without creating an account. |

Claim/unclaim follows the existing inventory lifecycle, including inventory removal
on unclaim. Provision inventory through the existing activation workflow before
repeating the claim test.

Deployment acceptance also requires the existing internal registration/email
verification flow and administrative RBAC regression suite to pass against the
same build. For account deletion, include fixtures with more than 100 empty venues,
a populated venue after the first page, and injected database failures; failed
eligibility checks must leave the account and venues intact.
