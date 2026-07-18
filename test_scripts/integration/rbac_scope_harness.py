#!/usr/bin/env python3
"""HTTP RBAC API harness for real OWProv plus fake OWSEC token validation."""

from __future__ import annotations

import json
import os
import ssl
import sys
import unittest
import uuid
from copy import deepcopy
from dataclasses import dataclass, field
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
from urllib.parse import parse_qs, urlparse

def load_seeded_env():
    import os
    try:
        base_dir = os.path.dirname(os.path.abspath(__file__))
        for path in [
            os.path.join(base_dir, "seeded_env.sh"),
            os.path.join(base_dir, "..", "seeded_env.sh"),
            os.path.join(base_dir, "..", "..", "seeded_env.sh"),
            "seeded_env.sh"
        ]:
            if os.path.exists(path):
                with open(path, "r") as f:
                    for line in f:
                        line = line.strip()
                        if line.startswith("export "):
                            parts = line[7:].split("=", 1)
                            if len(parts) == 2:
                                key, val = parts[0].strip(), parts[1].strip()
                                if val.startswith('"') and val.endswith('"'):
                                    val = val[1:-1]
                                elif val.startswith("'") and val.endswith("'"):
                                    val = val[1:-1]
                                os.environ[key] = val
    except Exception as e:
        pass

load_seeded_env()

ROOT_ENTITY = os.getenv("OWPROV_ROOT_ENTITY", "0000-0000-0000")
ENTITY_A = os.getenv("OWPROV_ENTITY_A", "09189968-6d82-47e0-a8cc-ecacf9890463")
ENTITY_B = os.getenv("OWPROV_ENTITY_B", "1f92df91-2adc-4efd-a8a8-3c014610eca7")
ENTITY_C = os.getenv("OWPROV_ENTITY_C", "dc4977fd-3a32-4bfb-866d-618054960724")
ENTITY_D = os.getenv("OWPROV_ENTITY_D", "3f891d10-982e-40f7-9c35-acf2205b69d7")
ENTITY_E = os.getenv("OWPROV_ENTITY_E", "entity-e")
ENTITY_J = "entity-j"
ENTITY_A_NORMAL = "entity-a-normal"
ENTITY_C_NORMAL = "entity-c-normal"
ENTITY_D_NORMAL = "entity-d-normal"
ENTITY_E_NORMAL = "entity-e-normal"
ENTITY_J_NORMAL = "entity-j-normal"
ENTITY_PLAIN_GRANDCHILD = "entity-plain-grandchild"

OPERATOR_DEFAULT = os.getenv("OWPROV_OPERATOR_DEFAULT", "babdfb0b-1c5d-4755-83d8-fa428c317a45")
OPERATOR_A = os.getenv("OWPROV_OPERATOR_A", "c72c9186-7f16-4213-920f-68f40ceb5252")
OPERATOR_B = os.getenv("OWPROV_OPERATOR_B", "3a68fcc9-2601-4c9b-b96f-51685cf7a5f7")
OPERATOR_C = os.getenv("OWPROV_OPERATOR_C", "42bc1890-ee3d-491d-8b48-ace0b9813665")
OPERATOR_D = os.getenv("OWPROV_OPERATOR_D", "1b0740fa-ebfe-4cae-af8e-521914ab258e")
OPERATOR_E = os.getenv("OWPROV_OPERATOR_E", "operator-e")
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

POLICY_A = os.getenv("OWPROV_POLICY_A", "c9cd496a-4a21-43ec-b568-6498b9b9b8ae")
POLICY_B = os.getenv("OWPROV_POLICY_B", "116baff9-cbd1-4a9f-a610-247c281bf3a5")
POLICY_C = os.getenv("OWPROV_POLICY_C", "38076268-8fcf-4ce6-8dd5-a8832711130c")
POLICY_D = os.getenv("OWPROV_POLICY_D", "72385f1c-eec4-4ac8-be25-b834b2b26441")
POLICY_E = os.getenv("OWPROV_POLICY_E", "policy-e")
POLICY_CSR_A = os.getenv("OWPROV_POLICY_CSR_A", "policy-csr-a")

ROLE_A = os.getenv("OWPROV_ROLE_A", "5afb0153-c891-466a-acec-91ef35595414")
ROLE_B = os.getenv("OWPROV_ROLE_B", "6ff27d80-22d3-4a31-8dc3-1b15eda3f18c")
ROLE_C = os.getenv("OWPROV_ROLE_C", "5975495d-a9c8-4a44-86fc-8deeac5803fa")
ROLE_D = os.getenv("OWPROV_ROLE_D", "22b6c368-d085-4127-8532-b9666b1d0655")
ROLE_E = os.getenv("OWPROV_ROLE_E", "role-e")
ROLE_CSR_A = os.getenv("OWPROV_ROLE_CSR_A", "role-csr-a")

USER_ID_A = os.getenv("OWPROV_USER_ID_A", "19232181-669f-42b1-bc5f-d505c04237ba")
USER_ID_B = os.getenv("OWPROV_USER_ID_B", "99b59972-2f76-44d3-ad05-aa93ebab6017")
USER_ID_C = os.getenv("OWPROV_USER_ID_C", "c66fdb8c-6894-4fe9-aae5-86e8f0f2ff75")
USER_ID_D = os.getenv("OWPROV_USER_ID_D", "138087ea-54f3-4972-bf1f-53463fba40e4")
USER_ID_E = os.getenv("OWPROV_USER_ID_E", "user-e")
USER_ID_CSR_A = "user-csr-a"
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
    can_mutate_policy: bool = True
    can_mutate_role: bool = True


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
            "token-csr-a": FakeUser(
                USER_ID_CSR_A,
                "token-csr-a",
                OPERATOR_A,
                can_mutate_policy=False,
                can_mutate_role=False,
            ),
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
        if resource == "managementPolicy" and action in {"CREATE", "POST", "UPDATE", "PUT", "MODIFY", "DELETE"}:
            return user.can_mutate_policy and self._authorized_entity(user, entity_id)
        if resource == "managementRole" and not user.can_role:
            return False
        if resource == "managementRole" and action in {"CREATE", "POST", "UPDATE", "PUT", "MODIFY", "DELETE"}:
            return user.can_mutate_role and self._authorized_entity(user, entity_id)
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
            if self._role_scope_exists(target_entity, body.get("venue", ""), body.get("users", []), new_id):
                return 400, {"errorCode": 400, "errorDetails": "duplicate role scope"}
            self.roles[new_id] = self._role(new_id, target_entity, policy_id, body.get("users", []))
            self.roles[new_id]["venue"] = body.get("venue", "")
            return 200, deepcopy(self.roles[new_id])
        if role_id not in self.roles:
            return self._invalid_id(role_id)
        if method == "PUT":
            final_users = body.get("users", self.roles[role_id].get("users", []))
            final_venue = body.get("venue", self.roles[role_id].get("venue", ""))
            if self._role_scope_exists(target_entity, final_venue, final_users, role_id):
                return 400, {"errorCode": 400, "errorDetails": "duplicate role scope"}
            self.roles[role_id]["name"] = body.get("name", self.roles[role_id]["name"])
            self.roles[role_id]["entity"] = target_entity
            self.roles[role_id]["venue"] = final_venue
            self.roles[role_id]["managementPolicy"] = policy_id
            self.roles[role_id]["users"] = final_users
            return 200, deepcopy(self.roles[role_id])
        if method == "DELETE":
            self.roles.pop(role_id, None)
            return 200, {"ok": True}
        return 404, {"errorCode": 404}

    def _role_scope_exists(self, entity_id: str, venue_id: str, users: list[str], skip_id: str) -> bool:
        for rid, role in self.roles.items():
            if rid == skip_id:
                continue
            if role.get("entity") != entity_id or role.get("venue", "") != venue_id:
                continue
            if set(role.get("users", [])) & set(users):
                return True
        return False

    def _validate_venue_policy(self, user: FakeUser, policy_id: str, entity_id: str, venue_id: str) -> tuple[bool, int]:
        if not policy_id:
            return True, 200
        if policy_id not in self.policies:
            return False, 400
        policy = self.policies[policy_id]
        if not self._can(user, "managementPolicy", "READ", policy["entity"]):
            return False, 403
        if policy["entity"] != entity_id:
            return False, 400
        if policy.get("venue") and policy["venue"] != venue_id:
            return False, 400
        return True, 200

    def _venue(self, method: str, venue_id: str, query: dict[str, list[str]], user: FakeUser) -> tuple[int, Any]:
        if method == "GET" and venue_id:
            if venue_id not in self.venues:
                return self._invalid_id(venue_id)
            venue = self.venues[venue_id]
            if not self._can(user, "venue", "READ", venue["entity"]):
                return 403, {"errorCode": 403}
            return 200, deepcopy(venue)
        if method == "GET":
            visible = set(self._visible_entities(user))
            ids = [vid for vid, venue in self.venues.items() if venue["entity"] in visible]
            if query.get("countOnly", ["false"])[0].lower() == "true":
                return 200, {"count": len(ids)}
            return 200, {"venues": [deepcopy(self.venues[vid]) for vid in ids]}

        if method == "POST":
            new_id = venue_id or body.get("id") or f"venue-created-{len(self.venues) + 1}"
            entity_id = body.get("entity", "")
            parent_id = body.get("parent", "")
            if parent_id:
                if parent_id not in self.venues:
                    return 400, {"errorCode": 400}
                entity_id = self.venues[parent_id]["entity"]
            if not entity_id or entity_id not in self.entities:
                return 400, {"errorCode": 400}
            if not self._can(user, "venue", "CREATE", entity_id):
                return 403, {"errorCode": 403}
            ok, failure_status = self._validate_venue_policy(user, body.get("managementPolicy", ""), entity_id, new_id)
            if not ok:
                return failure_status, {"errorCode": failure_status}
            self.venues[new_id] = {
                "id": new_id,
                "name": body.get("name", new_id),
                "entity": entity_id,
                "parent": parent_id,
                "managementPolicy": body.get("managementPolicy", ""),
            }
            return 200, deepcopy(self.venues[new_id])

        if venue_id not in self.venues:
            return self._invalid_id(venue_id)
        existing = self.venues[venue_id]
        final_entity = body.get("entity", existing["entity"])
        if body.get("parent"):
            parent_id = body["parent"]
            if parent_id not in self.venues:
                return 400, {"errorCode": 400}
            final_entity = self.venues[parent_id]["entity"]
        if final_entity not in self.entities:
            return 400, {"errorCode": 400}
        action = {"PUT": "UPDATE", "DELETE": "DELETE"}.get(method, method)
        if not self._can(user, "venue", action, final_entity):
            return 403, {"errorCode": 403}
        if method == "PUT":
            policy_id = body.get("managementPolicy", existing.get("managementPolicy", ""))
            if "managementPolicy" in body:
                ok, failure_status = self._validate_venue_policy(user, policy_id, final_entity, venue_id)
                if not ok:
                    return failure_status, {"errorCode": failure_status}
            existing.update({k: v for k, v in body.items() if k in {"name", "entity", "parent", "managementPolicy"}})
            existing["entity"] = final_entity
            return 200, deepcopy(existing)
        if method == "DELETE":
            self.venues.pop(venue_id, None)
            return 200, {"ok": True}
        return 404, {"errorCode": 404}

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


def _env_bool(name: str, default: bool) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() not in {"0", "false", "no", "off"}


def _normalize_base_url(value: str) -> str:
    return value.rstrip("/")


def _owprov_base_url() -> str:
    return _normalize_base_url(
        os.environ.get(
            "OWPROV_BASE_URL",
            os.environ.get("OWPROV_URL", "https://openwifi.wlan.local:16005/api/v1"),
        )
    )


def _fake_owsec_url() -> str:
    return _normalize_base_url(
        os.environ.get(
            "FAKE_OWSEC_URL",
            os.environ.get("FAKE_URL", "http://127.0.0.1:8080"),
        )
    )


def _ssl_context() -> ssl.SSLContext | None:
    parsed = urlparse(_owprov_base_url())
    if parsed.scheme != "https":
        return None
    if not _env_bool("OWPROV_TLS_VERIFY", True):
        return ssl._create_unverified_context()
    root_ca = (
        os.environ.get("OW_RBAC_TLS_ROOT_CA")
        or os.environ.get("OWPROV_TLS_ROOT_CA")
        or "/home/uttam/openwifi_workspace/deployment/wlan-cloud-ucentral-deploy/docker-compose/certs/restapi-ca.pem"
    )
    if root_ca:
        return ssl.create_default_context(cafile=root_ca)
    return ssl.create_default_context()


def _owprov_url(path: str) -> str:
    base = _owprov_base_url()
    if path.startswith("/api/v1/") and base.endswith("/api/v1"):
        return base + path[len("/api/v1") :]
    return base + (path if path.startswith("/") else f"/{path}")


def _decode_json(raw: bytes) -> Any:
    if not raw:
        return {}
    text = raw.decode("utf-8", errors="replace")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {"raw": text}


def _fake_owsec_get(path: str) -> Any:
    req = Request(_fake_owsec_url() + path, headers={"Accept": "application/json"}, method="GET")
    try:
        with urlopen(req, timeout=float(os.environ.get("OWPROV_TEST_TIMEOUT", "10"))) as resp:
            return _decode_json(resp.read())
    except (HTTPError, URLError, TimeoutError, OSError) as exc:
        raise RuntimeError(
            f"fake OWSEC is required for token validation controls at {_fake_owsec_url()}: {exc}"
        ) from exc


def request(method: str, path: str, token: str | None = "token-a", body: Any = None, expected_json: bool = True):
    payload = None
    headers = {"Accept": "application/json"}
    if body is not None:
        payload = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = f"Bearer {token}"

    req = Request(_owprov_url(path), data=payload, headers=headers, method=method.upper())
    try:
        with urlopen(req, timeout=float(os.environ.get("OWPROV_TEST_TIMEOUT", "10")), context=_ssl_context()) as resp:
            raw = resp.read()
            return resp.status, _decode_json(raw) if expected_json else raw
    except HTTPError as exc:
        raw = exc.read()
        return exc.code, _decode_json(raw) if expected_json else raw
    except (URLError, TimeoutError, OSError) as exc:
        return 0, {
            "error": str(exc),
            "errorDetails": f"could not call real OWProv at {_owprov_base_url()}",
        }


def set_scenario(name: str) -> None:
    del name


def reset_observations() -> None:
    return None


def observations() -> list[dict[str, Any]]:
    body = _fake_owsec_get("/observations")
    return body.get("observations", []) if isinstance(body, dict) else []


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


def new_uuid() -> str:
    return str(uuid.uuid4())


def run_unittest(module_name: str) -> None:
    suite = unittest.defaultTestLoader.loadTestsFromName(module_name)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        sys.exit(1)


USERPORTAL_URL = _owprov_base_url()
FAKE_URL = _fake_owsec_url()
