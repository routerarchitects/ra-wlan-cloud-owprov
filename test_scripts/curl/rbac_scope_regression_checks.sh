#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

required_env=(
  OWSEC_BASE_URL
  OW_ROOT_USERNAME
  OW_ROOT_PASSWORD
)

missing=()
for name in "${required_env[@]}"; do
  if [[ -z "${!name:-}" ]]; then
    missing+=("${name}")
  fi
done

if (( ${#missing[@]} > 0 )); then
  printf 'Missing required environment variables: %s\n' "${missing[*]}" >&2
  printf 'Example:\n' >&2
  printf '  export OWSEC_BASE_URL="https://openwifi.wlan.local:16001/api/v1"\n' >&2
  printf '  export OWPROV_BASE_URL="https://openwifi.wlan.local:16005/api/v1"\n' >&2
  printf '  export OW_ROOT_USERNAME="tip@ucentral.com"\n' >&2
  printf '  export OW_ROOT_PASSWORD="Iotina@123"\n' >&2
  exit 2
fi

export OWPROV_BASE_URL="${OWPROV_BASE_URL:-https://openwifi.wlan.local:16005/api/v1}"
export OW_TEST_USER_PASSWORD="${OW_TEST_USER_PASSWORD:-RbacPass123%}"
export OW_RBAC_RUN_ID="${OW_RBAC_RUN_ID:-$(date -u +%Y%m%dT%H%M%S%N)}"

cd "${ROOT_DIR}/tests/rbac"

go test . \
  -run 'TestRBAC(ManagementPolicyListIsFiltered|ManagementPolicyObjectAccess|ManagementRoleObjectAccess|OperatorCreateExplicitParentSelection|OperatorEntityEndpoint|DirectObjectAccessIsDenied|ManagementPolicyAndRoleDirectChildScope|ManagementRolePolicyLinkAuthorization)$' \
  -count=1 \
  -v \
  "$@"
