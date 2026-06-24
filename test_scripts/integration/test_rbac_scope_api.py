#!/usr/bin/env python3
"""API-level RBAC scope behavior tests against real OWProv APIs."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

from rbac_scope_harness import (  # noqa: E402
    DENIED,
    ENTITY_A,
    ENTITY_B,
    ENTITY_C,
    ENTITY_D,
    OPERATOR_A,
    OPERATOR_B,
    OPERATOR_C,
    OPERATOR_D,
    POLICY_A,
    POLICY_B,
    POLICY_C,
    POLICY_D,
    ROLE_A,
    ROLE_B,
    ROLE_C,
    ROLE_D,
    ROOT_ENTITY,
    USER_ID_A,
    assert_status,
    extract_ids,
    new_uuid,
    request,
    reset_observations,
    run_unittest,
    set_scenario,
)


class RBACScopeAPITest(unittest.TestCase):
    def setUp(self) -> None:
        set_scenario("api")
        reset_observations()

    def test_root_can_access_all_scopes(self) -> None:
        status, body = request("GET", "/api/v1/entity", token="root-token")
        assert_status(status, 200, body)
        ids = extract_ids(body)
        for expected in (ROOT_ENTITY, ENTITY_A, ENTITY_B, ENTITY_C, ENTITY_D):
            self.assertIn(expected, ids)

        for path in (
            f"/api/v1/entity/{ENTITY_B}",
            f"/api/v1/entity/{ENTITY_C}",
            f"/api/v1/managementPolicy/{POLICY_B}",
            f"/api/v1/managementRole/{ROLE_D}",
        ):
            status, body = request("GET", path, token="root-token")
            assert_status(status, 200, body)

    def test_entity_visibility_follows_operator_scope(self) -> None:
        status, body = request("GET", "/api/v1/entity", token="token-a")
        assert_status(status, 200, body)
        ids = extract_ids(body)
        self.assertIn(ENTITY_A, ids)
        self.assertIn(ENTITY_C, ids)
        self.assertIn(ENTITY_D, ids)
        self.assertNotIn(ENTITY_B, ids)

        for entity_id in (ENTITY_A, ENTITY_C, ENTITY_D):
            status, body = request("GET", f"/api/v1/entity/{entity_id}", token="token-a")
            assert_status(status, 200, body)

        for entity_id in (ENTITY_B,):
            status, body = request("GET", f"/api/v1/entity/{entity_id}", token="token-a")
            assert_status(status, DENIED, body)

        for entity_id in (ENTITY_C,):
            status, body = request("GET", f"/api/v1/entity/{entity_id}", token="token-c")
            assert_status(status, 200, body)
        for entity_id in (ENTITY_A, ENTITY_D, ENTITY_B):
            status, body = request("GET", f"/api/v1/entity/{entity_id}", token="token-c")
            assert_status(status, DENIED, body)

    def test_operator_api_scope(self) -> None:
        status, body = request("GET", "/api/v1/operator", token="token-a")
        assert_status(status, 200, body)
        ids = extract_ids(body)
        self.assertIn(OPERATOR_C, ids)
        self.assertIn(OPERATOR_D, ids)
        self.assertNotIn(OPERATOR_B, ids)
        for operator in body["operators"]:
            if operator["id"] != OPERATOR_A:
                self.assertEqual(operator["parentOperatorId"], OPERATOR_A)

        for operator_id in (OPERATOR_C, OPERATOR_D):
            status, body = request("GET", f"/api/v1/operator/{operator_id}", token="token-a")
            assert_status(status, 200, body)
            status, body = request(
                "PUT",
                f"/api/v1/operator/{operator_id}",
                token="token-a",
                body={"name": f"{operator_id}-updated"},
            )
            assert_status(status, 200, body)

        for operator_id in (OPERATOR_B,):
            status, body = request("GET", f"/api/v1/operator/{operator_id}", token="token-a")
            assert_status(status, DENIED, body)
            status, body = request("DELETE", f"/api/v1/operator/{operator_id}", token="token-a")
            assert_status(status, DENIED, body)

        status, body = request("GET", f"/api/v1/operator/{OPERATOR_D}", token="token-c")
        assert_status(status, DENIED, body)

    def test_management_policy_api_scope(self) -> None:
        for entity_id in (ENTITY_C, ENTITY_D):
            created_id = new_uuid()
            status, body = request(
                "POST",
                f"/api/v1/managementPolicy/{created_id}",
                token="token-a",
                body={
                    "id": created_id,
                    "entity": entity_id,
                    "name": f"policy-{created_id}",
                    "entries": [
                        {
                            "users": [USER_ID_A],
                            "resources": ["entity", "operator", "venue", "managementPolicy", "managementRole"],
                            "access": ["READ", "LIST", "CREATE", "MODIFY", "DELETE"],
                        }
                    ],
                },
            )
            assert_status(status, 200, body)
            actual_id = body.get("id", created_id)
            status, body = request("GET", f"/api/v1/managementPolicy/{actual_id}", token="token-a")
            assert_status(status, 200, body)
            status, body = request(
                "PUT",
                f"/api/v1/managementPolicy/{actual_id}",
                token="token-a",
                body={"id": actual_id, "name": "updated"},
            )
            assert_status(status, 200, body)
            status, body = request("DELETE", f"/api/v1/managementPolicy/{actual_id}", token="token-a")
            assert_status(status, 200, body)

        for policy_id in (POLICY_B,):
            status, body = request("GET", f"/api/v1/managementPolicy/{policy_id}", token="token-a")
            assert_status(status, DENIED, body)

        status, body = request("GET", f"/api/v1/managementPolicy/{POLICY_C}", token="token-c")
        assert_status(status, 200, body)

        # Test base list endpoint returns 200 with empty list
        status, body = request("GET", "/api/v1/managementPolicy", token="token-no-policy-access")
        assert_status(status, 200, body)
        self.assertEqual(body.get("managementPolicies", []), [])

        denied_create_id = new_uuid()
        for method, path, payload in (
            ("GET", f"/api/v1/managementPolicy/{POLICY_C}", None),
            ("POST", f"/api/v1/managementPolicy/{denied_create_id}", {"id": denied_create_id, "entity": ENTITY_C, "name": "denied-policy"}),
            ("PUT", f"/api/v1/managementPolicy/{POLICY_C}", {"id": POLICY_C, "name": "denied"}),
            ("DELETE", f"/api/v1/managementPolicy/{POLICY_C}", None),
        ):
            status, body = request(method, path, token="token-no-policy-access", body=payload)
            assert_status(status, 403, body)

    def test_management_role_api_scope_and_policy_validation(self) -> None:
        for entity_id, policy_id in ((ENTITY_C, POLICY_C), (ENTITY_D, POLICY_D)):
            created_id = new_uuid()
            status, body = request(
                "POST",
                f"/api/v1/managementRole/{created_id}",
                token="token-a",
                body={
                    "id": created_id,
                    "name": f"role-{created_id}",
                    "entity": entity_id,
                    "managementPolicy": policy_id,
                    "users": [USER_ID_A],
                },
            )
            assert_status(status, 200, body)
            actual_id = body.get("id", created_id)
            status, body = request("GET", f"/api/v1/managementRole/{actual_id}", token="token-a")
            assert_status(status, 200, body)
            status, body = request(
                "PUT",
                f"/api/v1/managementRole/{actual_id}",
                token="token-a",
                body={"id": actual_id, "name": "updated"},
            )
            assert_status(status, 200, body)
            status, body = request("DELETE", f"/api/v1/managementRole/{actual_id}", token="token-a")
            assert_status(status, 200, body)

        for role_id in (ROLE_B,):
            status, body = request("GET", f"/api/v1/managementRole/{role_id}", token="token-a")
            assert_status(status, DENIED, body)

        status, body = request("GET", f"/api/v1/managementRole/{ROLE_C}", token="token-c")
        assert_status(status, 200, body)

        bad_policy_role_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/managementRole/{bad_policy_role_id}",
            token="token-a",
            body={
                "id": bad_policy_role_id,
                "name": f"role-{bad_policy_role_id}",
                "entity": ENTITY_C,
                "managementPolicy": POLICY_B,
                "users": [USER_ID_A],
            },
        )
        assert_status(status, (400, 403), body)

        allowed_role_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/managementRole/{allowed_role_id}",
            token="token-a",
            body={
                "id": allowed_role_id,
                "name": f"role-{allowed_role_id}",
                "entity": ENTITY_C,
                "managementPolicy": POLICY_C,
                "users": [USER_ID_A],
            },
        )
        assert_status(status, 200, body)
        actual_id = body.get("id", allowed_role_id)
        status, body = request("DELETE", f"/api/v1/managementRole/{actual_id}", token="token-a")
        assert_status(status, 200, body)

        denied_role_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/managementRole/{denied_role_id}",
            token="token-a",
            body={
                "id": denied_role_id,
                "name": f"role-{denied_role_id}",
                "entity": ENTITY_B,
                "managementPolicy": POLICY_B,
                "users": [USER_ID_A],
            },
        )
        assert_status(status, DENIED, body)

        # Test base list endpoint returns 200 with empty list
        status, body = request("GET", "/api/v1/managementRole", token="token-no-role-access")
        assert_status(status, 200, body)
        self.assertEqual(body.get("roles", []), [])

        denied_create_id = new_uuid()
        for method, path, payload in (
            ("GET", f"/api/v1/managementRole/{ROLE_C}", None),
            ("POST", f"/api/v1/managementRole/{denied_create_id}", {"id": denied_create_id, "entity": ENTITY_C, "managementPolicy": POLICY_C, "name": "denied-role"}),
            ("PUT", f"/api/v1/managementRole/{ROLE_C}", {"id": ROLE_C, "name": "denied"}),
            ("DELETE", f"/api/v1/managementRole/{ROLE_C}", None),
        ):
            status, body = request(method, path, token="token-no-role-access", body=payload)
            assert_status(status, 403, body)

    def test_lists_are_filtered_before_count_and_pagination(self) -> None:
        list_expectations = (
            ("/api/v1/entity", "entities", {ENTITY_A, ENTITY_C, ENTITY_D}, {ENTITY_B}),
            ("/api/v1/managementPolicy", "managementPolicies", {POLICY_A, POLICY_C, POLICY_D}, {POLICY_B}),
            ("/api/v1/managementRole", "roles", {ROLE_A, ROLE_C, ROLE_D}, {ROLE_B}),
        )

        for path, _, allowed, denied in list_expectations:
            status, body = request("GET", path, token="token-a")
            assert_status(status, 200, body)
            ids = extract_ids(body)
            self.assertTrue(allowed.issubset(ids), f"{path} missing {allowed - ids}")
            self.assertTrue(ids.isdisjoint(denied), f"{path} leaked {ids & denied}")

            status, body = request("GET", f"{path}?countOnly=true", token="token-a")
            assert_status(status, 200, body)
            self.assertEqual(body["count"], len(allowed))

        status, body = request("GET", "/api/v1/managementPolicy?limit=1", token="token-a")
        assert_status(status, 200, body)
        self.assertEqual(len(body["managementPolicies"]), 1)
        self.assertIn(body["managementPolicies"][0]["id"], {POLICY_A, POLICY_C, POLICY_D})

    def test_seeded_venue_list_is_empty(self) -> None:
        status, body = request("GET", "/api/v1/venue", token="token-a")
        assert_status(status, 200, body)
        self.assertEqual(body["venues"], [])

        status, body = request("GET", "/api/v1/location?locationsForVenue=unknown-venue", token="token-a")
        assert_status(status, 200, body)
        self.assertEqual(body["locations"], [])


if __name__ == "__main__":
    run_unittest("__main__")
