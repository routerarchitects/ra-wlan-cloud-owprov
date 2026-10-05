#!/bin/bash
#
# Instance identity integration and docker-entrypoint templating test
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ENTRYPOINT="$REPO_ROOT/docker-entrypoint.sh"
TEMPLATE="$REPO_ROOT/owprov.properties.tmpl"

TEST_TMP_DIR=$(mktemp -d /tmp/owprov-identity-test.XXXXXX)
trap 'rm -rf "$TEST_TMP_DIR"' EXIT

# Provide envsubst shim if not installed in the test environment (e.g., in minimal build containers)
if ! command -v envsubst &>/dev/null; then
    mkdir -p "$TEST_TMP_DIR/bin"
    cat << 'SHIM' > "$TEST_TMP_DIR/bin/envsubst"
#!/usr/bin/env python3
import os, sys, re
pattern = re.compile(r'\$\{([A-Za-z0-9_]+)\}')
for line in sys.stdin:
    sys.stdout.write(pattern.sub(lambda m: os.environ.get(m.group(1), ''), line))
SHIM
    chmod +x "$TEST_TMP_DIR/bin/envsubst"
    export PATH="$TEST_TMP_DIR/bin:$PATH"
fi

echo "=== Running Instance Identity Integration Tests ==="

export TEMPLATE_CONFIG='true'
export OWPROV_TEMPLATE="$TEMPLATE"
export OWPROV_CONFIG="$TEST_TMP_DIR"
export OWPROV_ROOT="$TEST_TMP_DIR"

# Test 1: Valid OWPROV_SLOT_ID is correctly substituted into owprov.properties
echo "Test 1: Valid slot substitution..."
export OWPROV_SLOT_ID="owprov-1"
bash "$ENTRYPOINT" true

if ! grep -q "^openwifi.system.slot.id = owprov-1$" "$OWPROV_CONFIG/owprov.properties"; then
    echo "FAILED: openwifi.system.slot.id was not set to owprov-1 in owprov.properties"
    exit 1
fi
echo "  PASS: Valid slot substitution"

# Test 2: Empty OWPROV_SLOT_ID renders empty slot property
echo "Test 2: Empty slot substitution..."
export OWPROV_SLOT_ID=""
rm -f "$OWPROV_CONFIG/owprov.properties"
bash "$ENTRYPOINT" true

if ! grep -q "^openwifi.system.slot.id = $" "$OWPROV_CONFIG/owprov.properties"; then
    echo "FAILED: openwifi.system.slot.id was not empty when OWPROV_SLOT_ID was empty"
    exit 1
fi
echo "  PASS: Empty slot substitution"

# Test 3: Newline/property injection in OWPROV_SLOT_ID must be rejected
echo "Test 3: Newline injection rejection..."
export OWPROV_SLOT_ID=$'owprov-1\nopenwifi.security.restapi.disable = true'
set +e
ERR_OUT=$(bash "$ENTRYPOINT" true 2>&1)
EXIT_CODE=$?
set -e

if [ $EXIT_CODE -eq 0 ]; then
    echo "FAILED: docker-entrypoint.sh should have rejected newline injection in OWPROV_SLOT_ID"
    exit 1
fi
if [[ "$ERR_OUT" != *"OWPROV_SLOT_ID contains invalid characters"* ]]; then
    echo "FAILED: Unexpected error message for newline injection: $ERR_OUT"
    exit 1
fi
echo "  PASS: Newline injection rejection"

# Test 4: Space in OWPROV_SLOT_ID must be rejected
echo "Test 4: Space rejection..."
export OWPROV_SLOT_ID="owprov 1"
set +e
ERR_OUT=$(bash "$ENTRYPOINT" true 2>&1)
EXIT_CODE=$?
set -e

if [ $EXIT_CODE -eq 0 ]; then
    echo "FAILED: docker-entrypoint.sh should have rejected spaces in OWPROV_SLOT_ID"
    exit 1
fi
echo "  PASS: Space rejection"

# Test 5: Special characters (@, #, ;, =) must be rejected
echo "Test 5: Special character rejection..."
for BAD in "owprov@1" "owprov#1" "owprov;id" "owprov=evil" "owprov/1"; do
    export OWPROV_SLOT_ID="$BAD"
    set +e
    ERR_OUT=$(bash "$ENTRYPOINT" true 2>&1)
    EXIT_CODE=$?
    set -e
    if [ $EXIT_CODE -eq 0 ]; then
        echo "FAILED: docker-entrypoint.sh should have rejected '$BAD'"
        exit 1
    fi
done
echo "  PASS: Special character rejection"

# Test 6: Length exceeding 64 characters must be rejected
echo "Test 6: Length cap rejection (>64 characters)..."
export OWPROV_SLOT_ID="a012345678901234567890123456789012345678901234567890123456789012345"
set +e
ERR_OUT=$(bash "$ENTRYPOINT" true 2>&1)
EXIT_CODE=$?
set -e

if [ $EXIT_CODE -eq 0 ]; then
    echo "FAILED: docker-entrypoint.sh should have rejected slot ID exceeding 64 chars"
    exit 1
fi
if [[ "$ERR_OUT" != *"exceeds maximum allowed"* ]]; then
    echo "FAILED: Unexpected error message for length cap: $ERR_OUT"
    exit 1
fi
echo "  PASS: Length cap rejection"

# Test 7: Length of exactly 64 characters with valid characters must succeed
echo "Test 7: 64-character valid slot ID..."
export OWPROV_SLOT_ID="a012345678901234567890123456789012345678901234567890123456789012"
rm -f "$OWPROV_CONFIG/owprov.properties"
bash "$ENTRYPOINT" true

if ! grep -q "^openwifi.system.slot.id = $OWPROV_SLOT_ID$" "$OWPROV_CONFIG/owprov.properties"; then
    echo "FAILED: 64-char slot ID was not correctly templated"
    exit 1
fi
echo "  PASS: 64-character valid slot ID"

# Test 8: All allowed character classes (letters, numbers, dot, underscore, hyphen)
echo "Test 8: Character allowlist verification..."
export OWPROV_SLOT_ID="OWPROV_1.east-zone_02"
rm -f "$OWPROV_CONFIG/owprov.properties"
bash "$ENTRYPOINT" true

if ! grep -q "^openwifi.system.slot.id = OWPROV_1.east-zone_02$" "$OWPROV_CONFIG/owprov.properties"; then
    echo "FAILED: Allowed character slot was not correctly templated"
    exit 1
fi
echo "  PASS: Character allowlist verification"

# Test 9: Verify non-slot defaults are correctly exported and templated (not blanked)
echo "Test 9: Non-slot default property templating..."
unset OWPROV_SLOT_ID
rm -f "$OWPROV_CONFIG/owprov.properties"
bash "$ENTRYPOINT" true

if ! grep -q "^openwifi.system.uri.private = https://localhost:17005$" "$OWPROV_CONFIG/owprov.properties"; then
    echo "FAILED: openwifi.system.uri.private was blank or incorrect in default templated config"
    exit 1
fi
if ! grep -q "^openwifi.system.data = \$OWPROV_ROOT/data$" "$OWPROV_CONFIG/owprov.properties"; then
    echo "FAILED: openwifi.system.data was blank or incorrect in default templated config"
    exit 1
fi
if ! grep -q "^openwifi.restapi.host.0.port = 16005$" "$OWPROV_CONFIG/owprov.properties"; then
    echo "FAILED: openwifi.restapi.host.0.port was blank or incorrect in default templated config"
    exit 1
fi
if ! grep -q "^storage.type = sqlite$" "$OWPROV_CONFIG/owprov.properties"; then
    echo "FAILED: storage.type was blank or incorrect in default templated config"
    exit 1
fi
echo "  PASS: Non-slot default property templating"

echo "=== All Instance Identity Integration Tests Passed ==="
exit 0

