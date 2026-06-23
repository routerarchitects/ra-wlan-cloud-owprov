#!/usr/bin/env python3
"""API-level RBAC scope behavior tests using deterministic fake OWProv data."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

from rbac_scope_harness import (  # noqa: E402
    DENIED,
    ENTITY_A,
    ENTITY_A_NORMAL,
    ENTITY_B,
    ENTITY_C,
    ENTITY_C_NORMAL,
    ENTITY_D,
    ENTITY_D_NORMAL,
    ENTITY_E,
    ENTITY_E_NORMAL,
    ENTITY_J,
    ENTITY_J_NORMAL,
    ENTITY_PLAIN_GRANDCHILD,
    LOCATION_C,
    OPERATOR_A,
    OPERATOR_B,
    OPERATOR_C,
    OPERATOR_D,
    OPERATOR_E,
    OPERATOR_J,
    POLICY_A,
    POLICY_B,
    POLICY_C,
    POLICY_D,
    POLICY_E,
    ROLE_A,
    ROLE_B,
    ROLE_C,
    ROLE_D,
    ROLE_E,
    USER_ID_A,
    USER_ID_C,
    VENUE_A,
    VENUE_B,
    VENUE_C,
    VENUE_D,
    VENUE_E,
    assert_status,
    extract_ids,
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
        for expected in (ENTITY_A, ENTITY_B, ENTITY_C, ENTITY_D, ENTITY_E):
            self.assertIn(expected, ids)

        for path in (
            f"/api/v1/entity/{ENTITY_B}",
            f"/api/v1/entity/{ENTITY_E}",
            f"/api/v1/managementPolicy/{POLICY_B}",
            f"/api/v1/managementRole/{ROLE_E}",
        ):
            status, body = request("GET", path, token="root-token")
            assert_status(status, 200, body)

    def test_entity_visibility_follows_operator_scope(self) -> None:
        status, body = request("GET", "/api/v1/entity", token="token-a")
        assert_status(status, 200, body)
        ids = extract_ids(body)
        self.assertIn(ENTITY_A, ids)
        self.assertIn(ENTITY_A_NORMAL, ids)
        self.assertIn(ENTITY_C, ids)
        self.assertIn(ENTITY_C_NORMAL, ids)
        self.assertIn(ENTITY_D, ids)
        self.assertIn(ENTITY_D_NORMAL, ids)
        self.assertIn(ENTITY_PLAIN_GRANDCHILD, ids)
        self.assertNotIn(ENTITY_B, ids)
        self.assertNotIn(ENTITY_E, ids)
        self.assertNotIn(ENTITY_E_NORMAL, ids)
        self.assertNotIn(ENTITY_J, ids)
        self.assertNotIn(ENTITY_J_NORMAL, ids)
        for entity in body["entities"]:
            self.assertNotIn(ENTITY_E, entity.get("children", []))

        for entity_id in (ENTITY_A, ENTITY_A_NORMAL, ENTITY_C, ENTITY_C_NORMAL, ENTITY_D, ENTITY_D_NORMAL, ENTITY_PLAIN_GRANDCHILD):
            status, body = request("GET", f"/api/v1/entity/{entity_id}", token="token-a")
            assert_status(status, 200, body)

        for entity_id in (ENTITY_B, ENTITY_E, ENTITY_E_NORMAL, ENTITY_J, ENTITY_J_NORMAL):
            status, body = request("GET", f"/api/v1/entity/{entity_id}", token="token-a")
            assert_status(status, DENIED, body)

        for entity_id in (ENTITY_C, ENTITY_C_NORMAL, ENTITY_PLAIN_GRANDCHILD, ENTITY_E, ENTITY_E_NORMAL):
            status, body = request("GET", f"/api/v1/entity/{entity_id}", token="token-c")
            assert_status(status, 200, body)
        for entity_id in (ENTITY_D, ENTITY_J, ENTITY_J_NORMAL):
            status, body = request("GET", f"/api/v1/entity/{entity_id}", token="token-c")
            assert_status(status, DENIED, body)
        status, body = request("GET", f"/api/v1/entity/{ENTITY_C}", token="token-e")
        assert_status(status, DENIED, body)
        status, body = request("GET", f"/api/v1/entity/{ENTITY_A}", token="token-e")
        assert_status(status, DENIED, body)

    def test_operator_api_scope(self) -> None:
        status, body = request("GET", "/api/v1/operator", token="token-a")
        assert_status(status, 200, body)
        ids = extract_ids(body)
        self.assertIn(OPERATOR_C, ids)
        self.assertIn(OPERATOR_D, ids)
        self.assertNotIn(OPERATOR_B, ids)
        self.assertNotIn(OPERATOR_E, ids)
        for operator in body["operators"]:
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

        status, body = request(
            "POST",
            "/api/v1/operator",
            token="token-a",
            body={"id": "operator-created-under-c", "name": "child-c", "entityId": ENTITY_C},
        )
        assert_status(status, 200, body)
        self.assertEqual(body["entityId"], "entity-for-operator-created-under-c")
        self.assertEqual(body["parentOperatorId"], OPERATOR_C)

        for operator_id in (OPERATOR_B, OPERATOR_E):
            status, body = request("GET", f"/api/v1/operator/{operator_id}", token="token-a")
            assert_status(status, DENIED, body)
            status, body = request("DELETE", f"/api/v1/operator/{operator_id}", token="token-a")
            assert_status(status, DENIED, body)

        status, body = request("GET", f"/api/v1/operator/{OPERATOR_E}", token="token-c")
        assert_status(status, 200, body)
        status, body = request("GET", f"/api/v1/operator/{OPERATOR_J}", token="token-c")
        assert_status(status, DENIED, body)

        status, body = request(
            "POST",
            "/api/v1/operator",
            token="token-e",
            body={"id": "operator-created-under-e", "name": "child-e", "entityId": ENTITY_E},
        )
        assert_status(status, 200, body)
        self.assertEqual(body["parentOperatorId"], OPERATOR_E)
        status, body = request("GET", "/api/v1/operator", token="token-e")
        assert_status(status, 200, body)
        ids = extract_ids(body)
        self.assertIn("operator-created-under-e", ids)
        self.assertIn(OPERATOR_J, ids)
        self.assertNotIn(OPERATOR_C, ids)

    def test_management_policy_api_scope(self) -> None:
        for entity_id in (ENTITY_C, ENTITY_D):
            status, body = request(
                "POST",
                "/api/v1/managementPolicy",
                token="token-a",
                body={"entity": entity_id, "name": f"policy-{entity_id}"},
            )
            assert_status(status, 200, body)
            created_id = body["id"]
            status, body = request("GET", f"/api/v1/managementPolicy/{created_id}", token="token-a")
            assert_status(status, 200, body)
            status, body = request(
                "PUT",
                f"/api/v1/managementPolicy/{created_id}",
                token="token-a",
                body={"name": "updated"},
            )
            assert_status(status, 200, body)
            status, body = request("DELETE", f"/api/v1/managementPolicy/{created_id}", token="token-a")
            assert_status(status, 200, body)

        for policy_id in (POLICY_B, POLICY_E):
            status, body = request("GET", f"/api/v1/managementPolicy/{policy_id}", token="token-a")
            assert_status(status, DENIED, body)

        status, body = request("GET", f"/api/v1/managementPolicy/{POLICY_E}", token="token-c")
        assert_status(status, 200, body)

        for method, path, payload in (
            ("GET", "/api/v1/managementPolicy", None),
            ("GET", f"/api/v1/managementPolicy/{POLICY_C}", None),
            ("POST", "/api/v1/managementPolicy", {"entity": ENTITY_C}),
            ("PUT", f"/api/v1/managementPolicy/{POLICY_C}", {"name": "denied"}),
            ("DELETE", f"/api/v1/managementPolicy/{POLICY_C}", None),
        ):
            status, body = request(method, path, token="token-no-policy-access", body=payload)
            assert_status(status, 403, body)

    def test_management_role_api_scope_and_policy_validation(self) -> None:
        for entity_id, policy_id in ((ENTITY_C, POLICY_C), (ENTITY_D, POLICY_D)):
            status, body = request(
                "POST",
                "/api/v1/managementRole",
                token="token-a",
                body={"entity": entity_id, "managementPolicy": policy_id, "users": [USER_ID_A]},
            )
            assert_status(status, 200, body)
            created_id = body["id"]
            status, body = request("GET", f"/api/v1/managementRole/{created_id}", token="token-a")
            assert_status(status, 200, body)
            status, body = request(
                "PUT",
                f"/api/v1/managementRole/{created_id}",
                token="token-a",
                body={"name": "updated", "entity": entity_id, "managementPolicy": policy_id},
            )
            assert_status(status, 200, body)
            status, body = request("DELETE", f"/api/v1/managementRole/{created_id}", token="token-a")
            assert_status(status, 200, body)

        for role_id in (ROLE_B, ROLE_E):
            status, body = request("GET", f"/api/v1/managementRole/{role_id}", token="token-a")
            assert_status(status, DENIED, body)

        status, body = request("GET", f"/api/v1/managementRole/{ROLE_E}", token="token-c")
        assert_status(status, 200, body)

        status, body = request(
            "POST",
            "/api/v1/managementRole",
            token="token-a",
            body={"entity": ENTITY_C, "managementPolicy": POLICY_B, "users": [USER_ID_A]},
        )
        assert_status(status, (400, 403), body)

        status, body = request(
            "POST",
            "/api/v1/managementRole",
            token="token-a",
            body={"entity": ENTITY_C, "managementPolicy": POLICY_C, "users": [USER_ID_A]},
        )
        assert_status(status, 200, body)

        status, body = request(
            "POST",
            "/api/v1/managementRole",
            token="token-a",
            body={"entity": ENTITY_E, "managementPolicy": POLICY_E, "users": [USER_ID_A]},
        )
        assert_status(status, DENIED, body)

        status, body = request(
            "POST",
            "/api/v1/managementRole",
            token="token-c",
            body={"entity": ENTITY_E, "managementPolicy": POLICY_E, "users": [USER_ID_C]},
        )
        assert_status(status, 200, body)

        for method, path, payload in (
            ("GET", "/api/v1/managementRole", None),
            ("GET", f"/api/v1/managementRole/{ROLE_C}", None),
            ("POST", "/api/v1/managementRole", {"entity": ENTITY_C, "managementPolicy": POLICY_C}),
            ("PUT", f"/api/v1/managementRole/{ROLE_C}", {"entity": ENTITY_C, "managementPolicy": POLICY_C}),
            ("DELETE", f"/api/v1/managementRole/{ROLE_C}", None),
        ):
            status, body = request(method, path, token="token-no-role-access", body=payload)
            assert_status(status, 403, body)

    def test_lists_are_filtered_before_count_and_pagination(self) -> None:
        list_expectations = (
            ("/api/v1/entity", "entities", {ENTITY_A, ENTITY_A_NORMAL, ENTITY_C, ENTITY_C_NORMAL, ENTITY_D, ENTITY_D_NORMAL, ENTITY_PLAIN_GRANDCHILD}, {ENTITY_B, ENTITY_E, ENTITY_E_NORMAL, ENTITY_J, ENTITY_J_NORMAL}),
            ("/api/v1/venue", "venues", {VENUE_A, VENUE_C, VENUE_D}, {VENUE_B, VENUE_E}),
            ("/api/v1/managementPolicy", "managementPolicies", {POLICY_A, POLICY_C, POLICY_D}, {POLICY_B, POLICY_E}),
            ("/api/v1/managementRole", "roles", {ROLE_A, ROLE_C, ROLE_D}, {ROLE_B, ROLE_E}),
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

    def test_venue_location_access(self) -> None:
        status, body = request("GET", f"/api/v1/location?locationsForVenue={VENUE_C}", token="token-a")
        assert_status(status, 200, body)
        self.assertEqual(body["locations"][0]["id"], LOCATION_C)

        status, body = request("GET", f"/api/v1/location?locationsForVenue={VENUE_B}", token="token-a")
        assert_status(status, 403, body)

        status, body = request("GET", "/api/v1/location?locationsForVenue=unknown-venue", token="token-a")
        assert_status(status, 404, body)

        status, body = request("GET", f"/api/v1/location?locationsForVenue={VENUE_E}", token="token-a")
        assert_status(status, DENIED, body)


if __name__ == "__main__":
    run_unittest("__main__")
