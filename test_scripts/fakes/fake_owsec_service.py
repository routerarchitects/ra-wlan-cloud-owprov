#!/usr/bin/env python3
"""Deterministic fake OWSEC service for OWProv RBAC contract tests."""

from __future__ import annotations

import argparse
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlparse


# ---------------------------------------------------------------------------
# Deterministic RBAC token values (configurable via environment variables)
# ---------------------------------------------------------------------------

TOKEN_ROOT = os.getenv("OWPROV_ROOT_TOKEN", "root-token")
TOKEN_A = os.getenv("OWPROV_TOKEN_A", "token-a")
TOKEN_B = os.getenv("OWPROV_TOKEN_B", "token-b")
TOKEN_C = os.getenv("OWPROV_TOKEN_C", "token-c")
TOKEN_D = os.getenv("OWPROV_TOKEN_D", "token-d")
TOKEN_E = os.getenv("OWPROV_TOKEN_E", "token-e")
TOKEN_NO_POLICY_ACCESS = os.getenv("OWPROV_TOKEN_NO_POLICY_ACCESS", "token-no-policy-access")
TOKEN_NO_ROLE_ACCESS = os.getenv("OWPROV_TOKEN_NO_ROLE_ACCESS", "token-no-role-access")

USER_ID_A = os.getenv("OWPROV_USER_ID_A", "19232181-669f-42b1-bc5f-d505c04237ba")
USER_ID_B = os.getenv("OWPROV_USER_ID_B", "99b59972-2f76-44d3-ad05-aa93ebab6017")
USER_ID_C = os.getenv("OWPROV_USER_ID_C", "c66fdb8c-6894-4fe9-aae5-86e8f0f2ff75")
USER_ID_D = os.getenv("OWPROV_USER_ID_D", "138087ea-54f3-4972-bf1f-53463fba40e4")
USER_ID_READ_ONLY_A = os.getenv("OWPROV_USER_ID_READ_ONLY_A", "4054c847-ed42-4bcd-a9a3-aa88c94136b6")
USER_ID_NO_ACCESS_A = os.getenv("OWPROV_USER_ID_NO_ACCESS_A", "00000000-0000-0000-0000-0000000000aa")

OPERATOR_A_ID = os.getenv("OWPROV_OPERATOR_A", "c72c9186-7f16-4213-920f-68f40ceb5252")
OPERATOR_B_ID = os.getenv("OWPROV_OPERATOR_B", "3a68fcc9-2601-4c9b-b96f-51685cf7a5f7")
OPERATOR_C_ID = os.getenv("OWPROV_OPERATOR_C", "42bc1890-ee3d-491d-8b48-ace0b9813665")
OPERATOR_D_ID = os.getenv("OWPROV_OPERATOR_D", "1b0740fa-ebfe-4cae-af8e-521914ab258e")


# ---------------------------------------------------------------------------
# Deterministic identity map — maps token value -> identity metadata.
# Token values themselves MUST NOT be exposed by debug endpoints or logs.
# ---------------------------------------------------------------------------

TOKEN_IDENTITIES: dict[str, dict[str, Any]] = {
    TOKEN_ROOT: {
        "alias": "OWPROV_ROOT_TOKEN",
        "id": "user-root",
        "username": "root@test.com",
        "email": "root@test.com",
        "owner": "",
        "userRole": "root",
        "root": True,
        "canPolicy": True,
        "canRole": True,
    },
    TOKEN_A: {
        "alias": "OWPROV_TOKEN_A",
        "id": USER_ID_A,
        "username": "operator-a@test.com",
        "email": "operator-a@test.com",
        "owner": OPERATOR_A_ID,
        "userRole": "operator",
        "root": False,
        "canPolicy": True,
        "canRole": True,
    },
    TOKEN_B: {
        "alias": "OWPROV_TOKEN_B",
        "id": USER_ID_B,
        "username": "operator-b@test.com",
        "email": "operator-b@test.com",
        "owner": OPERATOR_B_ID,
        "userRole": "operator",
        "root": False,
        "canPolicy": True,
        "canRole": True,
    },
    TOKEN_C: {
        "alias": "OWPROV_TOKEN_C",
        "id": USER_ID_C,
        "username": "operator-c@test.com",
        "email": "operator-c@test.com",
        "owner": OPERATOR_C_ID,
        "userRole": "operator",
        "root": False,
        "canPolicy": True,
        "canRole": True,
    },
    TOKEN_D: {
        "alias": "OWPROV_TOKEN_D",
        "id": USER_ID_D,
        "username": "operator-d@test.com",
        "email": "operator-d@test.com",
        "owner": OPERATOR_D_ID,
        "userRole": "operator",
        "root": False,
        "canPolicy": True,
        "canRole": True,
    },
    TOKEN_E: {
        "alias": "OWPROV_TOKEN_E",
        "id": "user-e",
        "username": "operator-e@test.com",
        "email": "operator-e@test.com",
        "owner": "operator-e",
        "userRole": "operator",
        "root": False,
        "canPolicy": True,
        "canRole": True,
    },
    TOKEN_NO_POLICY_ACCESS: {
        "alias": "OWPROV_TOKEN_NO_POLICY_ACCESS",
        "id": USER_ID_NO_ACCESS_A,
        "username": "no-policy@test.com",
        "email": "no-policy@test.com",
        "owner": OPERATOR_A_ID,
        "userRole": "operator",
        "root": False,
        "canPolicy": False,
        "canRole": True,
    },
    TOKEN_NO_ROLE_ACCESS: {
        "alias": "OWPROV_TOKEN_NO_ROLE_ACCESS",
        "id": USER_ID_NO_ACCESS_A,
        "username": "no-role@test.com",
        "email": "no-role@test.com",
        "owner": OPERATOR_A_ID,
        "userRole": "operator",
        "root": False,
        "canPolicy": True,
        "canRole": False,
    },
}


# ---------------------------------------------------------------------------
# Preserved entity / operator constants (unchanged from original)
# ---------------------------------------------------------------------------

ROOT_ENTITY = "entity-root"
ENTITY_A = "entity-a"
ENTITY_B = "entity-b"
ENTITY_C = "entity-c"
ENTITY_D = "entity-d"
ENTITY_E = "entity-e"

OPERATOR_A = "operator-a"
OPERATOR_B = "operator-b"
OPERATOR_C = "operator-c"
OPERATOR_D = "operator-d"
OPERATOR_E = "operator-e"


# ---------------------------------------------------------------------------
# Legacy USERS map — kept for backward-compatible _token_response path
# ---------------------------------------------------------------------------

USERS: dict[str, dict[str, Any]] = {
    TOKEN_ROOT: {
        "id": "user-root",
        "email": "root@example.test",
        "name": "rootUser",
        "owner": "",
        "userRole": "root",
    },
    TOKEN_A: {
        "id": "user-a",
        "email": "user-a@example.test",
        "name": "userA",
        "owner": OPERATOR_A,
        "userRole": "admin",
    },
    TOKEN_B: {
        "id": "user-b",
        "email": "user-b@example.test",
        "name": "userB",
        "owner": OPERATOR_B,
        "userRole": "admin",
    },
    TOKEN_C: {
        "id": "user-c",
        "email": "user-c@example.test",
        "name": "userC",
        "owner": OPERATOR_C,
        "userRole": "admin",
    },
    TOKEN_D: {
        "id": "user-d",
        "email": "user-d@example.test",
        "name": "userD",
        "owner": OPERATOR_D,
        "userRole": "admin",
    },
    TOKEN_E: {
        "id": "user-e",
        "email": "user-e@example.test",
        "name": "userE",
        "owner": OPERATOR_E,
        "userRole": "admin",
    },
    TOKEN_NO_POLICY_ACCESS: {
        "id": "user-no-policy-access",
        "email": "user-no-policy-access@example.test",
        "name": "userNoPolicyAccess",
        "owner": OPERATOR_A,
        "userRole": "admin",
    },
    TOKEN_NO_ROLE_ACCESS: {
        "id": "user-no-role-access",
        "email": "user-no-role-access@example.test",
        "name": "userNoRoleAccess",
        "owner": OPERATOR_A,
        "userRole": "admin",
    },
}


# ---------------------------------------------------------------------------
# Reusable response builder for RBAC token validation
# ---------------------------------------------------------------------------

def build_token_validation_response(token: str, identity: dict[str, Any]) -> dict[str, Any]:
    """Build a deterministic OWSEC-style token-validation response.

    The response includes both tokenInfo and a rich userInfo with an
    ``rbacTestInfo`` block that integration tests can inspect.
    """
    return {
        "tokenInfo": {
            "access_token": token,
            "token_type": "Bearer",
            "expires_in": 3600,
            "created": 2000000000,
            "username": identity["username"],
        },
        "userInfo": {
            "id": identity["id"],
            "name": identity["username"],
            "description": "",
            "avatar": "",
            "email": identity["email"],
            "validated": True,
            "validationEmail": "",
            "validationDate": 0,
            "creationDate": 0,
            "validationURI": "",
            "changePassword": False,
            "lastLogin": 0,
            "currentLoginURI": "",
            "lastPasswordChange": 0,
            "lastEmailCheck": 0,
            "waitingForEmailCheck": False,
            "locale": "",
            "notes": [],
            "location": "",
            "owner": identity["owner"],
            "suspended": False,
            "blackListed": False,
            "userRole": identity["userRole"],
            "userTypeProprietaryInfo": {
                "mobiles": [],
                "mfa": {
                    "enabled": False,
                    "method": "",
                },
                "authenticatorSecret": "",
            },
            "securityPolicy": "",
            "securityPolicyChange": 0,
            "currentPassword": "",
            "lastPasswords": [],
            "oauthType": "",
            "oauthUserInfo": "",
            "modified": 0,
            "signingUp": "",
            "rbacTestInfo": {
                "root": identity["root"],
                "canPolicy": identity["canPolicy"],
                "canRole": identity["canRole"],
                "tokenAlias": identity["alias"],
            },
        },
    }


# ---------------------------------------------------------------------------
# Server state
# ---------------------------------------------------------------------------

class FakeOWSecState:
    def __init__(self) -> None:
        self.scenario = "default"
        self.observations: list[dict[str, Any]] = []

    def reset_observations(self) -> None:
        self.observations.clear()

    def observe(self, method: str, path: str, token: str) -> None:
        self.observations.append(
            {
                "method": method,
                "path": path,
                "token": token,
                "scenario": self.scenario,
                "at": int(time.time()),
            }
        )


STATE = FakeOWSecState()


# ---------------------------------------------------------------------------
# Legacy _token_response (kept for validateApiKey and any non-RBAC callers)
# ---------------------------------------------------------------------------

def _token_response(token: str) -> tuple[int, dict[str, Any]]:
    user = USERS.get(token)
    if token == "bad-token" or user is None:
        return 403, {"errorCode": 403, "errorDetails": "invalid token"}

    now = int(time.time())
    return 200, {
        "tokenInfo": {
            "access_token_": token,
            "token_type_": "Bearer",
            "expires_in_": 3600,
            "created_": now,
        },
        "userInfo": user,
        "expiresOn": now + 3600,
    }


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class FakeOWSecHandler(BaseHTTPRequestHandler):
    server_version = "FakeOWSec/1.0"

    def log_message(self, fmt: str, *args: Any) -> None:
        return

    def _read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))

    def _send(self, status: int, body: dict[str, Any]) -> None:
        payload = json.dumps(body, sort_keys=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)

        # ---- RBAC-aware token validation (validateToken / validateSubToken) ----
        if parsed.path in ("/api/v1/validateToken", "/api/v1/validateSubToken"):
            token = query.get("token", [""])[0]
            STATE.observe("GET", parsed.path, token)

            # Missing or explicitly bad token
            if not token or token == "bad-token":
                self.send_response(401)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"error": "invalid_token"}).encode())
                return

            # Backward-compatible dummy-test-token support
            if token == "dummy-test-token":
                identity = {
                    "alias": "DUMMY_TEST_TOKEN",
                    "id": "sub1",
                    "username": "test@test.com",
                    "email": "test@test.com",
                    "owner": "operator1",
                    "userRole": "subscriber",
                    "root": False,
                    "canPolicy": True,
                    "canRole": True,
                }
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps(build_token_validation_response(token, identity)).encode())
                return

            # Configured RBAC tokens
            identity = TOKEN_IDENTITIES.get(token)
            if identity is None:
                self.send_response(401)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"error": "invalid_token"}).encode())
                return

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(build_token_validation_response(token, identity)).encode())
            return

        # ---- validateApiKey (unchanged) ----
        if parsed.path == "/api/v1/validateApiKey":
            token = query.get("apikey", query.get("apiKey", [""]))[0]
            STATE.observe("GET", parsed.path, token)
            status, body = _token_response(token)
            self._send(status, body)
            return

        # ---- Observations (unchanged) ----
        if parsed.path == "/observations":
            self._send(200, {"scenario": STATE.scenario, "observations": STATE.observations})
            return

        # ---- Safe debug endpoint — exposes aliases/metadata only, never raw tokens ----
        if parsed.path == "/debug/tokens":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({
                "tokens": [
                    {
                        "alias": identity["alias"],
                        "userId": identity["id"],
                        "owner": identity["owner"],
                        "root": identity["root"],
                        "canPolicy": identity["canPolicy"],
                        "canRole": identity["canRole"],
                    }
                    for identity in TOKEN_IDENTITIES.values()
                ]
            }).encode())
            return

        self._send(404, {"errorCode": 404, "errorDetails": "not found"})

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        body = self._read_json()

        if parsed.path == "/reset-observations":
            STATE.reset_observations()
            self._send(200, {"ok": True})
            return

        if parsed.path == "/set-scenario":
            STATE.scenario = str(body.get("name", "default"))
            STATE.reset_observations()
            self._send(200, {"ok": True, "scenario": STATE.scenario})
            return

        self._send(404, {"errorCode": 404, "errorDetails": "not found"})


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), FakeOWSecHandler)
    print(f"fake OWSEC listening on http://{args.host}:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
