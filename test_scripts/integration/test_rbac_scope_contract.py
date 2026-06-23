#!/usr/bin/env python3
"""RBAC scope contract tests for auth, direct access, and validation behavior."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

from rbac_scope_harness import (  # noqa: E402
    DENIED,
    ENTITY_B,
    POLICY_B,
    ROLE_B,
    assert_status,
    extract_ids,
    request,
    reset_observations,
    run_unittest,
    set_scenario,
)


class RBACScopeContractTest(unittest.TestCase):
    def setUp(self) -> None:
        set_scenario("contract")
        reset_observations()

    def test_missing_auth_is_denied(self) -> None:
        status, body = request("GET", f"/api/v1/entity/{ENTITY_B}", token=None)
        assert_status(status, 403, body)
        self.assertNotIn("name", body)

    def test_bad_auth_is_denied(self) -> None:
        status, body = request("GET", f"/api/v1/entity/{ENTITY_B}", token="bad-token")
        assert_status(status, 403, body)

    def test_direct_uuid_access_cannot_bypass_rbac(self) -> None:
        status, body = request("GET", f"/api/v1/managementPolicy/{POLICY_B}", token="token-a")
        assert_status(status, DENIED, body)

        status, body = request(
            "PUT",
            f"/api/v1/managementPolicy/{POLICY_B}",
            token="token-a",
            body={"name": "attempted-sibling-update"},
        )
        assert_status(status, DENIED, body)

        status, body = request("DELETE", f"/api/v1/managementRole/{ROLE_B}", token="token-a")
        assert_status(status, DENIED, body)

    def test_invalid_uuid_and_malformed_request_are_rejected(self) -> None:
        for path in (
            "/api/v1/entity/not-a-real-entity",
            "/api/v1/operator/not-a-real-operator",
            "/api/v1/managementPolicy/not-a-real-policy",
            "/api/v1/managementRole/not-a-real-role",
        ):
            status, body = request("GET", path, token="token-a")
            assert_status(status, 404, body)

        status, body = request("GET", "/api/v1/entity/bad'id", token="token-a")
        assert_status(status, 400, body)

    def test_unsafe_query_parameters_do_not_widen_visibility(self) -> None:
        status, body = request(
            "GET",
            "/api/v1/entity?entity=' OR '1'='1&venue=anything;DROP TABLE entities",
            token="token-a",
        )
        assert_status(status, 200, body)
        ids = extract_ids(body)
        self.assertIn("entity-c", ids)
        self.assertIn("entity-d", ids)
        self.assertNotIn("entity-b", ids)
        self.assertNotIn("entity-e", ids)


if __name__ == "__main__":
    run_unittest("__main__")
