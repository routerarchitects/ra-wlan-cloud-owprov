#!/usr/bin/env python3
"""API-level RBAC scope behavior tests against real OWProv APIs."""

from __future__ import annotations

import os
import json
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

from rbac_scope_harness import (  # noqa: E402
    DENIED,
    ENTITY_A,
    ENTITY_B,
    ENTITY_C,
    ENTITY_D,
    ENTITY_E,
    OPERATOR_A,
    OPERATOR_B,
    OPERATOR_C,
    OPERATOR_D,
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
    ROOT_ENTITY,
    USER_ID_A,
    USER_ID_B,
    USER_ID_C,
    USER_ID_CSR_A,
    VENUE_C,
    assert_status,
    extract_ids,
    new_uuid,
    request,
    reset_observations,
    run_unittest,
    set_scenario,
)

MANAGEMENT_RESOURCES = [
    "entity",
    "venue",
    "operator",
    "inventory",
    "configuration",
    "managementPolicy",
    "managementRole",
]


def policy_payload(policy_id: str, entity_id: str, user_id: str, access: list[str] | None = None) -> dict:
    if access is None:
        access = ["FULL"]
    scope = {
        "type": "entity",
        "entityId": entity_id,
        "includeVenues": True,
        "includeChildEntities": True,
    }
    return {
        "id": policy_id,
        "entity": entity_id,
        "name": f"policy-{policy_id}",
        "entries": [
            {
                "users": [user_id],
                "resources": [resource],
                "access": access,
                "policy": json.dumps(scope, separators=(",", ":")),
            }
            for resource in MANAGEMENT_RESOURCES
        ],
    }


def role_payload(role_id: str, entity_id: str, policy_id: str, user_id: str = USER_ID_B) -> dict:
    return {
        "id": role_id,
        "name": f"role-{role_id}",
        "entity": entity_id,
        "managementPolicy": policy_id,
        "users": [user_id],
    }


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

    def test_admin_policy_role_crud_allows_own_and_direct_child_scope(self) -> None:
        for label, entity_id in (("own", ENTITY_A), ("direct-child", ENTITY_C)):
            with self.subTest(scope=label, resource="managementPolicy"):
                policy_id = new_uuid()
                status, body = request(
                    "POST",
                    f"/api/v1/managementPolicy/{policy_id}",
                    token="token-a",
                    body=policy_payload(policy_id, entity_id, USER_ID_A),
                )
                assert_status(status, 200, body)
                policy_id = body.get("id", policy_id)

                status, body = request("GET", f"/api/v1/managementPolicy/{policy_id}", token="token-a")
                assert_status(status, 200, body)

                status, body = request(
                    "PUT",
                    f"/api/v1/managementPolicy/{policy_id}",
                    token="token-a",
                    body={"id": policy_id, "name": f"policy-{label}-updated", "entity": entity_id},
                )
                assert_status(status, 200, body)

            with self.subTest(scope=label, resource="managementRole"):
                role_id = new_uuid()
                status, body = request(
                    "POST",
                    f"/api/v1/managementRole/{role_id}",
                    token="token-a",
                    body=role_payload(role_id, entity_id, policy_id),
                )
                assert_status(status, 200, body)
                role_id = body.get("id", role_id)

                status, body = request("GET", f"/api/v1/managementRole/{role_id}", token="token-a")
                assert_status(status, 200, body)

                status, body = request(
                    "PUT",
                    f"/api/v1/managementRole/{role_id}",
                    token="token-a",
                    body={"id": role_id, "name": f"role-{label}-updated"},
                )
                assert_status(status, 200, body)

                status, body = request("DELETE", f"/api/v1/managementRole/{role_id}", token="token-a")
                assert_status(status, 200, body)

            with self.subTest(scope=label, resource="managementPolicy-delete"):
                status, body = request("DELETE", f"/api/v1/managementPolicy/{policy_id}", token="token-a")
                assert_status(status, 200, body)

    def test_admin_policy_role_crud_denies_parent_sibling_and_grandchild_scope(self) -> None:
        denied_scopes = (
            ("parent", "token-c", ENTITY_A, POLICY_A, ROLE_A),
            ("sibling", "token-c", ENTITY_D, POLICY_D, ROLE_D),
            ("grandchild", "token-a", ENTITY_E, POLICY_E, ROLE_E),
        )

        for relation, token, entity_id, policy_id, role_id in denied_scopes:
            with self.subTest(relation=relation, resource="managementPolicy"):
                status, body = request("GET", f"/api/v1/managementPolicy/{policy_id}", token=token)
                assert_status(status, DENIED, body)

                create_id = new_uuid()
                status, body = request(
                    "POST",
                    f"/api/v1/managementPolicy/{create_id}",
                    token=token,
                    body=policy_payload(create_id, entity_id, USER_ID_A),
                )
                assert_status(status, DENIED, body)

                status, body = request(
                    "PUT",
                    f"/api/v1/managementPolicy/{policy_id}",
                    token=token,
                    body={"id": policy_id, "name": f"denied-{relation}", "entity": entity_id},
                )
                assert_status(status, DENIED, body)

                status, body = request("DELETE", f"/api/v1/managementPolicy/{policy_id}", token=token)
                assert_status(status, DENIED, body)

            with self.subTest(relation=relation, resource="managementRole"):
                status, body = request("GET", f"/api/v1/managementRole/{role_id}", token=token)
                assert_status(status, DENIED, body)

                create_id = new_uuid()
                status, body = request(
                    "POST",
                    f"/api/v1/managementRole/{create_id}",
                    token=token,
                    body=role_payload(create_id, entity_id, policy_id),
                )
                assert_status(status, DENIED, body)

                status, body = request(
                    "PUT",
                    f"/api/v1/managementRole/{role_id}",
                    token=token,
                    body={"id": role_id, "name": f"denied-{relation}"},
                )
                assert_status(status, DENIED, body)

                status, body = request("DELETE", f"/api/v1/managementRole/{role_id}", token=token)
                assert_status(status, DENIED, body)

    def test_csr_policy_role_read_list_allowed_but_mutation_denied(self) -> None:
        for path, key, expected_id in (
            ("/api/v1/managementPolicy", "managementPolicies", POLICY_A),
            ("/api/v1/managementRole", "roles", ROLE_A),
        ):
            with self.subTest(path=path, action="LIST"):
                status, body = request("GET", path, token="token-csr-a")
                assert_status(status, 200, body)
                self.assertIn(expected_id, {item["id"] for item in body.get(key, [])})

                status, body = request("GET", f"{path}?countOnly=true", token="token-csr-a")
                assert_status(status, 200, body)
                self.assertGreaterEqual(body.get("count", 0), 1)

        for policy_id in (POLICY_A, POLICY_C):
            status, body = request("GET", f"/api/v1/managementPolicy/{policy_id}", token="token-csr-a")
            assert_status(status, 200, body)

        for role_id in (ROLE_A, ROLE_C):
            status, body = request("GET", f"/api/v1/managementRole/{role_id}", token="token-csr-a")
            assert_status(status, 200, body)

        denied_policy_id = new_uuid()
        for method, path, payload in (
            (
                "POST",
                f"/api/v1/managementPolicy/{denied_policy_id}",
                policy_payload(denied_policy_id, ENTITY_A, USER_ID_CSR_A, ["READ", "LIST"]),
            ),
            ("PUT", f"/api/v1/managementPolicy/{POLICY_A}", {"id": POLICY_A, "name": "csr-denied"}),
            ("DELETE", f"/api/v1/managementPolicy/{POLICY_A}", None),
        ):
            status, body = request(method, path, token="token-csr-a", body=payload)
            assert_status(status, 403, body)

        denied_role_id = new_uuid()
        for method, path, payload in (
            (
                "POST",
                f"/api/v1/managementRole/{denied_role_id}",
                role_payload(denied_role_id, ENTITY_A, POLICY_A, USER_ID_CSR_A),
            ),
            ("PUT", f"/api/v1/managementRole/{ROLE_A}", {"id": ROLE_A, "name": "csr-denied"}),
            ("DELETE", f"/api/v1/managementRole/{ROLE_A}", None),
        ):
            status, body = request(method, path, token="token-csr-a", body=payload)
            assert_status(status, 403, body)

    def test_venue_management_policy_attachment_scope_validation(self) -> None:
        allowed_venue_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/venue/{allowed_venue_id}",
            token="token-a",
            body={"id": allowed_venue_id, "name": f"venue-{allowed_venue_id}", "entity": ENTITY_C, "managementPolicy": POLICY_C},
        )
        assert_status(status, 200, body)

        denied_venue_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/venue/{denied_venue_id}",
            token="token-a",
            body={"id": denied_venue_id, "name": f"venue-{denied_venue_id}", "entity": ENTITY_C, "managementPolicy": POLICY_B},
        )
        assert_status(status, (400, 403), body)

        update_venue_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/venue/{update_venue_id}",
            token="token-a",
            body={"id": update_venue_id, "name": f"venue-{update_venue_id}", "entity": ENTITY_C},
        )
        assert_status(status, 200, body)

        status, body = request(
            "PUT",
            f"/api/v1/venue/{update_venue_id}",
            token="token-a",
            body={"id": update_venue_id, "name": f"venue-{update_venue_id}-policy-c", "managementPolicy": POLICY_C},
        )
        assert_status(status, 200, body)

        status, body = request(
            "PUT",
            f"/api/v1/venue/{update_venue_id}",
            token="token-a",
            body={"id": update_venue_id, "name": f"venue-{update_venue_id}-policy-b", "managementPolicy": POLICY_B},
        )
        assert_status(status, (400, 403), body)

        status, body = request(
            "PUT",
            f"/api/v1/venue/{update_venue_id}",
            token="token-a",
            body={"id": update_venue_id, "name": f"venue-{update_venue_id}-policy-d", "managementPolicy": POLICY_D},
        )
        assert_status(status, (400, 403), body)

        no_policy_venue_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/venue/{no_policy_venue_id}",
            token="token-no-policy-access",
            body={"id": no_policy_venue_id, "name": f"venue-{no_policy_venue_id}", "entity": ENTITY_C, "managementPolicy": POLICY_C},
        )
        assert_status(status, 403, body)

        root_venue_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/venue/{root_venue_id}",
            token="root-token",
            body={"id": root_venue_id, "name": f"venue-{root_venue_id}", "entity": ENTITY_B, "managementPolicy": POLICY_B},
        )
        assert_status(status, 200, body)

    def test_management_role_update_scope_policy_validation_and_no_partial_mutation(self) -> None:
        policy_c_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/managementPolicy/{policy_c_id}",
            token="root-token",
            body=policy_payload(policy_c_id, ENTITY_C, USER_ID_B),
        )
        assert_status(status, 200, body)

        policy_d_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/managementPolicy/{policy_d_id}",
            token="root-token",
            body=policy_payload(policy_d_id, ENTITY_D, USER_ID_B),
        )
        assert_status(status, 200, body)

        role_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/managementRole/{role_id}",
            token="root-token",
            body=role_payload(role_id, ENTITY_C, policy_c_id, USER_ID_B),
        )
        assert_status(status, 200, body)
        role_id = body.get("id", role_id)

        status, body = request(
            "PUT",
            f"/api/v1/managementRole/{role_id}",
            token="token-a",
            body={"id": role_id, "name": "role-c-policy-c", "managementPolicy": policy_c_id},
        )
        assert_status(status, 200, body)

        status, body = request(
            "PUT",
            f"/api/v1/managementRole/{role_id}",
            token="token-a",
            body={"id": role_id, "name": "role-c-policy-b", "managementPolicy": POLICY_B},
        )
        assert_status(status, (400, 403), body)

        status, body = request("GET", f"/api/v1/managementRole/{role_id}", token="root-token")
        assert_status(status, 200, body)
        self.assertEqual(body.get("entity"), ENTITY_C)
        self.assertEqual(body.get("managementPolicy"), policy_c_id)

        status, body = request(
            "PUT",
            f"/api/v1/managementRole/{role_id}",
            token="token-a",
            body={"id": role_id, "name": "role-d-policy-d", "entity": ENTITY_D, "managementPolicy": policy_d_id},
        )
        assert_status(status, 200, body)

        status, body = request(
            "PUT",
            f"/api/v1/managementRole/{role_id}",
            token="token-a",
            body={"id": role_id, "name": "role-b-policy-b", "entity": ENTITY_B, "managementPolicy": POLICY_B},
        )
        assert_status(status, 403, body)

        status, body = request(
            "PUT",
            f"/api/v1/managementRole/{role_id}",
            token="token-a",
            body={"id": role_id, "name": "role-d-policy-c", "managementPolicy": policy_c_id},
        )
        assert_status(status, 400, body)

        status, body = request(
            "PUT",
            f"/api/v1/managementRole/{role_id}",
            token="token-a",
            body={"id": role_id, "name": "role-c-no-policy-change", "entity": ENTITY_C},
        )
        assert_status(status, 400, body)

        status, body = request(
            "PUT",
            f"/api/v1/managementRole/{role_id}",
            token="token-csr-a",
            body={"id": role_id, "name": "csr-update-denied"},
        )
        assert_status(status, 403, body)

    def test_management_role_update_duplicate_check_uses_final_candidate(self) -> None:
        role_id = new_uuid()
        status, body = request(
            "POST",
            f"/api/v1/managementRole/{role_id}",
            token="root-token",
            body=role_payload(role_id, ENTITY_C, POLICY_C, USER_ID_B),
        )
        assert_status(status, 200, body)

        status, body = request(
            "PUT",
            f"/api/v1/managementRole/{role_id}",
            token="token-a",
            body={"id": role_id, "name": "duplicate-final-candidate", "users": [USER_ID_C]},
        )
        assert_status(status, 400, body)

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
