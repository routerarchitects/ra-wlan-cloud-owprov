#!/usr/bin/env python3
"""Deterministic fake OWSEC service for OWProv RBAC contract tests."""

from __future__ import annotations

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlparse


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


USERS: dict[str, dict[str, Any]] = {
    "root-token": {
        "id": "user-root",
        "email": "root@example.test",
        "name": "rootUser",
        "owner": "",
        "userRole": "root",
    },
    "token-a": {
        "id": "user-a",
        "email": "user-a@example.test",
        "name": "userA",
        "owner": OPERATOR_A,
        "userRole": "admin",
    },
    "token-b": {
        "id": "user-b",
        "email": "user-b@example.test",
        "name": "userB",
        "owner": OPERATOR_B,
        "userRole": "admin",
    },
    "token-c": {
        "id": "user-c",
        "email": "user-c@example.test",
        "name": "userC",
        "owner": OPERATOR_C,
        "userRole": "admin",
    },
    "token-d": {
        "id": "user-d",
        "email": "user-d@example.test",
        "name": "userD",
        "owner": OPERATOR_D,
        "userRole": "admin",
    },
    "token-e": {
        "id": "user-e",
        "email": "user-e@example.test",
        "name": "userE",
        "owner": OPERATOR_E,
        "userRole": "admin",
    },
    "token-no-policy-access": {
        "id": "user-no-policy-access",
        "email": "user-no-policy-access@example.test",
        "name": "userNoPolicyAccess",
        "owner": OPERATOR_A,
        "userRole": "admin",
    },
    "token-no-role-access": {
        "id": "user-no-role-access",
        "email": "user-no-role-access@example.test",
        "name": "userNoRoleAccess",
        "owner": OPERATOR_A,
        "userRole": "admin",
    },
}


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

        if parsed.path in ("/api/v1/validateToken", "/api/v1/validateSubToken"):
            token = query.get("token", [""])[0]
            STATE.observe("GET", parsed.path, token)
            status, body = _token_response(token)
            self._send(status, body)
            return

        if parsed.path == "/api/v1/validateApiKey":
            token = query.get("apikey", query.get("apiKey", [""]))[0]
            STATE.observe("GET", parsed.path, token)
            status, body = _token_response(token)
            self._send(status, body)
            return

        if parsed.path == "/observations":
            self._send(200, {"scenario": STATE.scenario, "observations": STATE.observations})
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
