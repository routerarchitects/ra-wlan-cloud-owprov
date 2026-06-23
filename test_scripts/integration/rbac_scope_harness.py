#!/usr/bin/env python3
"""Repo-local fake OWProv RBAC API harness used by Python contract tests."""

from __future__ import annotations

import json
import os
import sys
import unittest
from copy import deepcopy
from dataclasses import dataclass, field
from typing import Any
from urllib.parse import parse_qs, urlparse

ROOT_ENTITY = "entity-root"
ENTITY_A = "entity-a"
ENTITY_B = "entity-b"
ENTITY_C = "entity-c"
ENTITY_D = "entity-d"
ENTITY_E = "entity-e"
ENTITY_J = "entity-j"
ENTITY_A_NORMAL = "entity-a-normal"
ENTITY_C_NORMAL = "entity-c-normal"
ENTITY_D_NORMAL = "entity-d-normal"
ENTITY_E_NORMAL = "entity-e-normal"
ENTITY_J_NORMAL = "entity-j-normal"
ENTITY_PLAIN_GRANDCHILD = "entity-plain-grandchild"

OPERATOR_DEFAULT = "operator-default"
OPERATOR_A = "operator-a"
OPERATOR_B = "operator-b"
OPERATOR_C = "operator-c"
OPERATOR_D = "operator-d"
OPERATOR_E = "operator-e"
OPERATOR_J = "operator-j"

VENUE_A = "venue-a"
VENUE_B = "venue-b"
VENUE_C = "venue-c"
VENUE_D = "venue-d"
VENUE_E = "venue-e"

LOCATION_A = "location-a"
LOCATION_B = "location-b"
LOCATION_C = "location-c"
LOCATION_D = "location-d"
LOCATION_E = "location-e"

POLICY_A = "policy-a"
POLICY_B = "policy-b"
POLICY_C = "policy-c"
POLICY_D = "policy-d"
POLICY_E = "policy-e"

ROLE_A = "role-a"
ROLE_B = "role-b"
ROLE_C = "role-c"
ROLE_D = "role-d"
ROLE_E = "role-e"

USER_ID_A = "user-a"
USER_ID_B = "user-b"
USER_ID_C = "user-c"
USER_ID_D = "user-d"
USER_ID_E = "user-e"
USER_ID_NO_POLICY = "user-no-policy-access"
USER_ID_NO_ROLE = "user-no-role-access"

DENIED = (403, 404)
UNAUTHORIZED = (403,)


@dataclass(frozen=True)
class FakeUser:
    id: str
    token: str
    owner_operator: str
    root: bool = False
    can_policy: bool = True
    can_role: bool = True


@dataclass
class FakeOWProv:
    scenario: str = "default"
    observations: list[dict[str, Any]] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.entities = {
            ROOT_ENTITY: {
                "id": ROOT_ENTITY,
                "name": "root",
                "parent": "",
                "operatorId": "",
                "children": [ENTITY_A, ENTITY_B],
            },
            ENTITY_A: {
                "id": ENTITY_A,
                "name": "A",
                "parent": ROOT_ENTITY,
                "operatorId": OPERATOR_A,
                "children": [ENTITY_C, ENTITY_D, ENTITY_A_NORMAL],
            },
            ENTITY_B: {"id": ENTITY_B, "name": "B", "parent": ROOT_ENTITY, "operatorId": OPERATOR_B, "children": []},
            ENTITY_C: {
                "id": ENTITY_C,
                "name": "C",
                "parent": ENTITY_A,
                "operatorId": OPERATOR_C,
                "children": [ENTITY_E, ENTITY_C_NORMAL, ENTITY_PLAIN_GRANDCHILD],
            },
            ENTITY_D: {"id": ENTITY_D, "name": "D", "parent": ENTITY_A, "operatorId": OPERATOR_D, "children": [ENTITY_D_NORMAL]},
            ENTITY_E: {"id": ENTITY_E, "name": "E", "parent": ENTITY_C, "operatorId": OPERATOR_E, "children": [ENTITY_E_NORMAL, ENTITY_J]},
            ENTITY_J: {"id": ENTITY_J, "name": "J", "parent": ENTITY_E, "operatorId": OPERATOR_J, "children": [ENTITY_J_NORMAL]},
            ENTITY_A_NORMAL: {
                "id": ENTITY_A_NORMAL,
                "name": "a-normal",
                "parent": ENTITY_A,
                "operatorId": "",
                "children": [],
            },
            ENTITY_C_NORMAL: {
                "id": ENTITY_C_NORMAL,
                "name": "c-normal",
                "parent": ENTITY_C,
                "operatorId": "",
                "children": [],
            },
            ENTITY_D_NORMAL: {
                "id": ENTITY_D_NORMAL,
                "name": "d-normal",
                "parent": ENTITY_D,
                "operatorId": "",
                "children": [],
            },
            ENTITY_E_NORMAL: {
                "id": ENTITY_E_NORMAL,
                "name": "e-normal",
                "parent": ENTITY_E,
                "operatorId": "",
                "children": [],
            },
            ENTITY_J_NORMAL: {
                "id": ENTITY_J_NORMAL,
                "name": "j-normal",
                "parent": ENTITY_J,
                "operatorId": "",
                "children": [],
            },
            ENTITY_PLAIN_GRANDCHILD: {
                "id": ENTITY_PLAIN_GRANDCHILD,
                "name": "plain-grandchild",
                "parent": ENTITY_C,
                "operatorId": "",
                "children": [],
            },
        }
        self.operators = {
            OPERATOR_DEFAULT: {
                "id": OPERATOR_DEFAULT,
                "name": "default-operator",
                "entityId": ROOT_ENTITY,
                "parentOperatorId": "",
            },
            OPERATOR_A: {"id": OPERATOR_A, "name": "operator-A", "entityId": ENTITY_A, "parentOperatorId": OPERATOR_DEFAULT},
            OPERATOR_B: {"id": OPERATOR_B, "name": "operator-B", "entityId": ENTITY_B, "parentOperatorId": OPERATOR_DEFAULT},
            OPERATOR_C: {"id": OPERATOR_C, "name": "operator-C", "entityId": ENTITY_C, "parentOperatorId": OPERATOR_A},
            OPERATOR_D: {"id": OPERATOR_D, "name": "operator-D", "entityId": ENTITY_D, "parentOperatorId": OPERATOR_A},
            OPERATOR_E: {"id": OPERATOR_E, "name": "operator-E", "entityId": ENTITY_E, "parentOperatorId": OPERATOR_C},
            OPERATOR_J: {"id": OPERATOR_J, "name": "operator-J", "entityId": ENTITY_J, "parentOperatorId": OPERATOR_E},
        }
        self.venues = {
            VENUE_A: {"id": VENUE_A, "name": "venue-A", "entity": ENTITY_A},
            VENUE_B: {"id": VENUE_B, "name": "venue-B", "entity": ENTITY_B},
            VENUE_C: {"id": VENUE_C, "name": "venue-C", "entity": ENTITY_C},
            VENUE_D: {"id": VENUE_D, "name": "venue-D", "entity": ENTITY_D},
            VENUE_E: {"id": VENUE_E, "name": "venue-E", "entity": ENTITY_E},
        }
        self.locations = {
            LOCATION_A: {"id": LOCATION_A, "name": "location-A", "venue": VENUE_A, "entity": ENTITY_A},
            LOCATION_B: {"id": LOCATION_B, "name": "location-B", "venue": VENUE_B, "entity": ENTITY_B},
            LOCATION_C: {"id": LOCATION_C, "name": "location-C", "venue": VENUE_C, "entity": ENTITY_C},
            LOCATION_D: {"id": LOCATION_D, "name": "location-D", "venue": VENUE_D, "entity": ENTITY_D},
            LOCATION_E: {"id": LOCATION_E, "name": "location-E", "venue": VENUE_E, "entity": ENTITY_E},
        }
        self.policies = {
            POLICY_A: self._policy(POLICY_A, ENTITY_A),
            POLICY_B: self._policy(POLICY_B, ENTITY_B),
            POLICY_C: self._policy(POLICY_C, ENTITY_C),
            POLICY_D: self._policy(POLICY_D, ENTITY_D),
            POLICY_E: self._policy(POLICY_E, ENTITY_E),
        }
        self.roles = {
            ROLE_A: self._role(ROLE_A, ENTITY_A, POLICY_A, [USER_ID_A]),
            ROLE_B: self._role(ROLE_B, ENTITY_B, POLICY_B, [USER_ID_B]),
            ROLE_C: self._role(ROLE_C, ENTITY_C, POLICY_C, [USER_ID_C]),
            ROLE_D: self._role(ROLE_D, ENTITY_D, POLICY_D, [USER_ID_D]),
            ROLE_E: self._role(ROLE_E, ENTITY_E, POLICY_E, [USER_ID_E]),
        }
        self.observations.clear()

    @staticmethod
    def _policy(policy_id: str, entity: str) -> dict[str, Any]:
        return {
            "id": policy_id,
            "name": policy_id,
            "entity": entity,
            "venue": "",
            "entries": [
                {
                    "users": [],
                    "resources": ["entity", "operator", "venue", "managementPolicy", "managementRole"],
                    "access": ["READ", "LIST", "CREATE", "UPDATE", "MODIFY", "DELETE"],
                }
            ],
        }

    @staticmethod
    def _role(role_id: str, entity: str, policy: str, users: list[str]) -> dict[str, Any]:
        return {
            "id": role_id,
            "name": role_id,
            "entity": entity,
            "venue": "",
            "managementPolicy": policy,
            "users": users,
        }

    @property
    def users(self) -> dict[str, FakeUser]:
        return {
            "root-token": FakeUser("user-root", "root-token", "", root=True),
            "token-a": FakeUser(USER_ID_A, "token-a", OPERATOR_A),
            "token-b": FakeUser(USER_ID_B, "token-b", OPERATOR_B),
            "token-c": FakeUser(USER_ID_C, "token-c", OPERATOR_C),
            "token-d": FakeUser(USER_ID_D, "token-d", OPERATOR_D),
            "token-e": FakeUser(USER_ID_E, "token-e", OPERATOR_E),
            "token-no-policy-access": FakeUser(
                USER_ID_NO_POLICY, "token-no-policy-access", OPERATOR_A, can_policy=False
            ),
            "token-no-role-access": FakeUser(
                USER_ID_NO_ROLE, "token-no-role-access", OPERATOR_A, can_role=False
            ),
        }

    def set_scenario(self, name: str) -> None:
        self.scenario = name
        self.reset()

    def reset_observations(self) -> None:
        self.observations.clear()

    def request(self, method: str, path: str, token: str | None = "token-a", body: Any = None) -> tuple[int, Any]:
        parsed = urlparse(path)
        query = parse_qs(parsed.query)
        api_path = parsed.path
        self.observations.append({"method": method, "path": path, "token": token or ""})

        user = self._authenticate(token)
        if user is None:
            return 403, {"errorCode": 403, "errorDetails": "access denied"}

        if not api_path.startswith("/api/v1/"):
            return 404, {"errorCode": 404, "errorDetails": "not found"}

        parts = [part for part in api_path[len("/api/v1/") :].split("/") if part]
        if not parts:
            return 404, {"errorCode": 404, "errorDetails": "not found"}

        try:
            resource = parts[0]
            resource_id = parts[1] if len(parts) > 1 else ""
            if resource == "entity":
                return self._entity(method, resource_id, query, user)
            if resource == "operator":
                return self._operator(method, resource_id, user, body or {})
            if resource == "managementPolicy":
                return self._management_policy(method, resource_id, query, user, body or {})
            if resource == "managementRole":
                return self._management_role(method, resource_id, query, user, body or {})
            if resource == "venue":
                return self._venue(method, resource_id, query, user)
            if resource == "location":
                return self._location(method, query, user)
        except ValueError:
            return 400, {"errorCode": 400, "errorDetails": "malformed request"}

        return 404, {"errorCode": 404, "errorDetails": "not found"}

    def _authenticate(self, token: str | None) -> FakeUser | None:
        if not token or token == "bad-token":
            return None
        return self.users.get(token)

    def _owner_entity(self, user: FakeUser) -> str:
        if user.root:
            return ROOT_ENTITY
        return self.operators[user.owner_operator]["entityId"]

    def _parent_operator_id(self, entity_id: str) -> str:
        owner = self._owning_operator(entity_id)
        return owner or (OPERATOR_DEFAULT if entity_id == ROOT_ENTITY else "")

    def _ancestors(self, entity_id: str) -> list[str]:
        if entity_id not in self.entities:
            raise ValueError(entity_id)
        ancestors = []
        current = entity_id
        while current:
            ancestors.append(current)
            current = self.entities[current]["parent"]
        return ancestors

    def _is_descendant(self, target_entity: str, ancestor_entity: str) -> bool:
        return ancestor_entity in self._ancestors(target_entity)

    def _direct_child_of(self, target_entity: str, parent_entity: str) -> bool:
        return self.entities[target_entity]["parent"] == parent_entity

    def _owning_operator(self, entity_id: str) -> str:
        current = entity_id
        while current:
            entity = self.entities[current]
            if entity["operatorId"]:
                return entity["operatorId"]
            current = entity["parent"]
        return ""

    def _operator_scope_allowed(self, user: FakeUser, entity_id: str) -> bool:
        if user.root:
            return True
        owner_operator = self._owning_operator(entity_id)
        if not owner_operator:
            return False
        if owner_operator == user.owner_operator:
            return True
        return self.operators[owner_operator].get("parentOperatorId") == user.owner_operator

    def _authorized_entity(self, user: FakeUser, entity_id: str) -> bool:
        if user.root:
            return True
        if entity_id not in self.entities:
            return False
        return self._operator_scope_allowed(user, entity_id)

    def _visible_entities(self, user: FakeUser) -> list[str]:
        if user.root:
            return sorted(self.entities)
        return sorted(
            entity_id
            for entity_id in self.entities
            if self._operator_scope_allowed(user, entity_id)
        )

    def _can(self, user: FakeUser, resource: str, action: str, entity_id: str) -> bool:
        if user.root:
            return True
        if resource == "managementPolicy" and not user.can_policy:
            return False
        if resource == "managementRole" and not user.can_role:
            return False
        return self._authorized_entity(user, entity_id)

    def _entity(self, method: str, entity_id: str, query: dict[str, list[str]], user: FakeUser) -> tuple[int, Any]:
        if method != "GET":
            return 404, {"errorCode": 404}
        if entity_id:
            if entity_id not in self.entities:
                return self._invalid_id(entity_id)
            if not self._can(user, "entity", "READ", entity_id):
                return 403, {"errorCode": 403}
            return 200, deepcopy(self.entities[entity_id])

        ids = self._visible_entities(user)
        if self._unsafe_filter(query):
            ids = [entity_id for entity_id in ids if entity_id in self.entities]
        if query.get("countOnly", ["false"])[0].lower() == "true":
            return 200, {"count": len(ids)}
        entities = [deepcopy(self.entities[entity_id]) for entity_id in ids]
        if not user.root:
            for entity in entities:
                entity["children"] = [
                    child_id
                    for child_id in entity.get("children", [])
                    if self._operator_scope_allowed(user, child_id)
                ]
        return 200, {"entities": entities}

    def _operator(self, method: str, operator_id: str, user: FakeUser, body: dict[str, Any]) -> tuple[int, Any]:
        if method == "GET" and not operator_id:
            if user.root:
                ids = list(self.operators)
            else:
                ids = [
                    oid
                    for oid, op in self.operators.items()
                    if op.get("parentOperatorId") == user.owner_operator
                ]
            return 200, {"operators": [deepcopy(self.operators[oid]) for oid in ids]}

        if method == "GET":
            if operator_id not in self.operators:
                return self._invalid_id(operator_id)
            op = self.operators[operator_id]
            if not self._can(user, "operator", "READ", op["entityId"]):
                return 403, {"errorCode": 403}
            return 200, deepcopy(op)

        target_entity = body.get("entityId") or self.operators.get(operator_id, {}).get("entityId")
        if not target_entity or target_entity not in self.entities:
            return 400, {"errorCode": 400}
        if not self._can(user, "operator", method, target_entity):
            return 403, {"errorCode": 403}

        if method == "POST":
            new_id = body.get("id") or f"operator-created-{len(self.operators) + 1}"
            entity_id = f"entity-for-{new_id}"
            self.entities[entity_id] = {
                "id": entity_id,
                "name": f"Operator-entity:{new_id}",
                "parent": target_entity,
                "operatorId": new_id,
                "children": [],
            }
            self.entities[target_entity].setdefault("children", []).append(entity_id)
            self.operators[new_id] = {
                "id": new_id,
                "name": body.get("name", new_id),
                "entityId": entity_id,
                "parentOperatorId": self._parent_operator_id(target_entity),
            }
            return 200, deepcopy(self.operators[new_id])

        if operator_id not in self.operators:
            return self._invalid_id(operator_id)
        if method == "PUT":
            self.operators[operator_id]["name"] = body.get("name", self.operators[operator_id]["name"])
            return 200, deepcopy(self.operators[operator_id])
        if method == "DELETE":
            return 200, {"ok": True}
        return 404, {"errorCode": 404}

    def _management_policy(
        self, method: str, policy_id: str, query: dict[str, list[str]], user: FakeUser, body: dict[str, Any]
    ) -> tuple[int, Any]:
        if method == "GET" and not policy_id:
            if not user.root and not user.can_policy:
                return 403, {"errorCode": 403}
            visible = set(self._visible_entities(user))
            ids = [
                pid
                for pid, policy in self.policies.items()
                if policy["entity"] in visible
            ]
            if query.get("countOnly", ["false"])[0].lower() == "true":
                return 200, {"count": len(ids)}
            offset = int(query.get("offset", ["0"])[0])
            limit = int(query.get("limit", [str(len(ids))])[0])
            page = ids[offset : offset + limit]
            return 200, {"managementPolicies": [deepcopy(self.policies[pid]) for pid in page]}

        if method == "GET":
            if policy_id not in self.policies:
                return self._invalid_id(policy_id)
            policy = self.policies[policy_id]
            if not self._can(user, "managementPolicy", "READ", policy["entity"]):
                return 403, {"errorCode": 403}
            return 200, deepcopy(policy)

        target_entity = body.get("entity") or self.policies.get(policy_id, {}).get("entity")
        if not target_entity or target_entity not in self.entities:
            return 400, {"errorCode": 400}
        action = {"POST": "CREATE", "PUT": "UPDATE", "DELETE": "DELETE"}.get(method, method)
        if not self._can(user, "managementPolicy", action, target_entity):
            return 403, {"errorCode": 403}
        if method == "POST":
            new_id = policy_id or body.get("id") or f"policy-created-{len(self.policies) + 1}"
            self.policies[new_id] = self._policy(new_id, target_entity)
            return 200, deepcopy(self.policies[new_id])
        if policy_id not in self.policies:
            return self._invalid_id(policy_id)
        if method == "PUT":
            self.policies[policy_id]["name"] = body.get("name", self.policies[policy_id]["name"])
            return 200, deepcopy(self.policies[policy_id])
        if method == "DELETE":
            self.policies.pop(policy_id, None)
            return 200, {"ok": True}
        return 404, {"errorCode": 404}

    def _management_role(
        self, method: str, role_id: str, query: dict[str, list[str]], user: FakeUser, body: dict[str, Any]
    ) -> tuple[int, Any]:
        if method == "GET" and not role_id:
            if not user.root and not user.can_role:
                return 403, {"errorCode": 403}
            visible = set(self._visible_entities(user))
            ids = [
                rid
                for rid, role in self.roles.items()
                if role["entity"] in visible
            ]
            if query.get("countOnly", ["false"])[0].lower() == "true":
                return 200, {"count": len(ids)}
            offset = int(query.get("offset", ["0"])[0])
            limit = int(query.get("limit", [str(len(ids))])[0])
            page = ids[offset : offset + limit]
            return 200, {"roles": [deepcopy(self.roles[rid]) for rid in page]}

        if method == "GET":
            if role_id not in self.roles:
                return self._invalid_id(role_id)
            role = self.roles[role_id]
            if not self._can(user, "managementRole", "READ", role["entity"]):
                return 403, {"errorCode": 403}
            return 200, deepcopy(role)

        target_entity = body.get("entity") or self.roles.get(role_id, {}).get("entity")
        if not target_entity or target_entity not in self.entities:
            return 400, {"errorCode": 400}
        policy_id = body.get("managementPolicy") or self.roles.get(role_id, {}).get("managementPolicy", "")
        if policy_id and policy_id not in self.policies:
            return 400, {"errorCode": 400, "errorDetails": "unknown managementPolicy"}
        if policy_id and self.policies[policy_id]["entity"] != target_entity:
            return 400, {"errorCode": 400, "errorDetails": "policy scope mismatch"}

        action = {"POST": "CREATE", "PUT": "UPDATE", "DELETE": "DELETE"}.get(method, method)
        if not self._can(user, "managementRole", action, target_entity):
            return 403, {"errorCode": 403}
        if method == "POST":
            new_id = role_id or body.get("id") or f"role-created-{len(self.roles) + 1}"
            self.roles[new_id] = self._role(new_id, target_entity, policy_id, body.get("users", []))
            return 200, deepcopy(self.roles[new_id])
        if role_id not in self.roles:
            return self._invalid_id(role_id)
        if method == "PUT":
            self.roles[role_id]["name"] = body.get("name", self.roles[role_id]["name"])
            return 200, deepcopy(self.roles[role_id])
        if method == "DELETE":
            self.roles.pop(role_id, None)
            return 200, {"ok": True}
        return 404, {"errorCode": 404}

    def _venue(self, method: str, venue_id: str, query: dict[str, list[str]], user: FakeUser) -> tuple[int, Any]:
        if method != "GET":
            return 404, {"errorCode": 404}
        if venue_id:
            if venue_id not in self.venues:
                return self._invalid_id(venue_id)
            venue = self.venues[venue_id]
            if not self._can(user, "venue", "READ", venue["entity"]):
                return 403, {"errorCode": 403}
            return 200, deepcopy(venue)
        visible = set(self._visible_entities(user))
        ids = [vid for vid, venue in self.venues.items() if venue["entity"] in visible]
        if query.get("countOnly", ["false"])[0].lower() == "true":
            return 200, {"count": len(ids)}
        return 200, {"venues": [deepcopy(self.venues[vid]) for vid in ids]}

    def _location(self, method: str, query: dict[str, list[str]], user: FakeUser) -> tuple[int, Any]:
        if method != "GET":
            return 404, {"errorCode": 404}
        venue_id = query.get("locationsForVenue", [""])[0]
        if not venue_id:
            return 400, {"errorCode": 400}
        if venue_id not in self.venues:
            return 404, {"errorCode": 404}
        venue = self.venues[venue_id]
        if not self._can(user, "venue", "READ", venue["entity"]):
            return 403, {"errorCode": 403}
        return 200, {"locations": [deepcopy(loc) for loc in self.locations.values() if loc["venue"] == venue_id]}

    @staticmethod
    def _unsafe_filter(query: dict[str, list[str]]) -> bool:
        text = json.dumps(query)
        return any(marker in text.lower() for marker in ("'", "\"", " or ", "--", ";", "drop", "select"))

    @staticmethod
    def _invalid_id(value: str) -> tuple[int, dict[str, Any]]:
        if any(ch in value for ch in ("'", "\"", " ", ";")):
            return 400, {"errorCode": 400, "errorDetails": "invalid id"}
        return 404, {"errorCode": 404, "errorDetails": "not found"}


HARNESS = FakeOWProv()


def request(method: str, path: str, token: str | None = "token-a", body: Any = None, expected_json: bool = True):
    del expected_json
    return HARNESS.request(method.upper(), path, token=token, body=body)


def set_scenario(name: str) -> None:
    HARNESS.set_scenario(name)


def reset_observations() -> None:
    HARNESS.reset_observations()


def observations() -> list[dict[str, Any]]:
    return HARNESS.observations


def assert_status(status: int, expected: int | tuple[int, ...], body: Any = None) -> None:
    expected_tuple = expected if isinstance(expected, tuple) else (expected,)
    assert status in expected_tuple, f"expected {expected_tuple}, got {status}: {body}"


def extract_ids(body: dict[str, Any]) -> set[str]:
    ids: set[str] = set()
    for value in body.values():
        if isinstance(value, list):
            for item in value:
                if isinstance(item, dict) and "id" in item:
                    ids.add(str(item["id"]))
    return ids


def run_unittest(module_name: str) -> None:
    suite = unittest.defaultTestLoader.loadTestsFromName(module_name)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        sys.exit(1)


USERPORTAL_URL = os.environ.get("OWPROV_URL", "fake://owprov")
FAKE_URL = os.environ.get("FAKE_URL", "http://127.0.0.1:8080")
