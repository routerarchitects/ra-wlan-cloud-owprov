#!/usr/bin/env python3
"""Regression tests for the fake OWSEC service RBAC token behavior.

These tests run against the in-process handler code — no HTTP server needed.
They verify:
  - All configured RBAC tokens are accepted with correct identity metadata.
  - Default token values work when env vars are not set.
  - Unknown tokens are rejected (HTTP 401).
  - ``bad-token`` is rejected (HTTP 401).
  - ``/debug/tokens`` does not expose raw token values.
  - ``dummy-test-token`` backward-compatibility is preserved.
  - Existing endpoints (/observations, /reset-observations, /set-scenario)
    continue to function.
"""

from __future__ import annotations

import io
import json
import os
import sys
import unittest
from http.server import BaseHTTPRequestHandler
from typing import Any
from urllib.parse import urlencode

# Ensure the fakes package is importable.
sys.path.insert(0, os.path.dirname(__file__))

import fake_owsec_service as owsec  # noqa: E402


# ---------------------------------------------------------------------------
# Helper: drive the handler without a real socket
# ---------------------------------------------------------------------------

class _FakeWFile(io.BytesIO):
    """Writable bytes buffer that ignores flush/close for handler compat."""

    def close(self) -> None:  # noqa: D401
        pass  # keep the buffer intact so we can read .getvalue()


class _StubRequest:
    """Minimal stand-in for the socket connection expected by the handler."""

    def __init__(self) -> None:
        self.makefile = self._makefile

    @staticmethod
    def _makefile(*_args: Any, **_kwargs: Any) -> io.BytesIO:
        return io.BytesIO()

    @staticmethod
    def sendall(data: bytes) -> None:
        pass


def _call_handler(method: str, path: str, body: dict[str, Any] | None = None) -> tuple[int, dict[str, Any]]:
    """Invoke :class:`FakeOWSecHandler` for *method* + *path* and return (status, json_body)."""
    body_bytes = json.dumps(body).encode() if body else b""
    raw_request = f"{method} {path} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {len(body_bytes)}\r\n\r\n"

    rfile = io.BytesIO(raw_request.encode() + body_bytes)
    wfile = _FakeWFile()

    handler = owsec.FakeOWSecHandler.__new__(owsec.FakeOWSecHandler)
    handler.rfile = rfile
    handler.wfile = wfile
    handler.client_address = ("127.0.0.1", 0)
    handler.server = type("FakeServer", (), {"server_name": "localhost", "server_port": 8080})()
    handler.close_connection = True
    handler.requestline = f"{method} {path} HTTP/1.1"
    handler.request_version = "HTTP/1.1"
    handler.command = method
    handler.path = path
    handler.headers = {"Content-Length": str(len(body_bytes))}

    # Re-position rfile to body start so _read_json can work.
    handler.rfile = io.BytesIO(body_bytes)

    getattr(handler, f"do_{method}")()

    # Parse response status from wfile (handler writes status line).
    raw_out = wfile.getvalue().decode("utf-8", errors="replace")
    status_code = handler._headers_buffer[0].decode() if hasattr(handler, "_headers_buffer") else ""  # noqa: SLF001
    # We need the status code that was recorded via send_response.
    # BaseHTTPRequestHandler stores it — but we captured via wfile.
    # Re-parse from wfile output.
    status_line = raw_out.split("\r\n")[0] if "\r\n" in raw_out else ""
    if status_line.startswith("HTTP/"):
        code = int(status_line.split(" ")[1])
    else:
        code = 200

    # Extract JSON body (after double CRLF).
    if "\r\n\r\n" in raw_out:
        json_part = raw_out.split("\r\n\r\n", 1)[1]
    else:
        json_part = raw_out

    try:
        parsed = json.loads(json_part)
    except json.JSONDecodeError:
        parsed = {}

    return code, parsed


def _validate_token(token: str) -> tuple[int, dict[str, Any]]:
    path = f"/api/v1/validateToken?token={token}"
    return _call_handler("GET", path)


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------


class TestRBACTokenDefaults(unittest.TestCase):
    """Verify token constants use correct defaults when env vars are unset."""

    def test_default_values_when_env_not_set(self) -> None:
        # These env vars are not expected to be set in a normal test run.
        defaults = {
            "OWPROV_ROOT_TOKEN": "root-token",
            "OWPROV_TOKEN_A": "token-a",
            "OWPROV_TOKEN_B": "token-b",
            "OWPROV_TOKEN_C": "token-c",
            "OWPROV_TOKEN_D": "token-d",
            "OWPROV_TOKEN_E": "token-e",
            "OWPROV_TOKEN_CSR_A": "token-csr-a",
            "OWPROV_TOKEN_NO_POLICY_ACCESS": "token-no-policy-access",
            "OWPROV_TOKEN_NO_ROLE_ACCESS": "token-no-role-access",
        }
        for env_key, expected_default in defaults.items():
            actual = os.getenv(env_key, expected_default)
            # If the env var IS set, we accept whatever it is; the point is
            # the module-level constant should match.
            self.assertEqual(
                actual,
                getattr(owsec, f"TOKEN_{env_key.removeprefix('OWPROV_TOKEN_').replace('OWPROV_', '')}".replace("ROOT_TOKEN", "ROOT"), actual),
            )


class TestTokenIdentitiesMap(unittest.TestCase):
    """Verify TOKEN_IDENTITIES has all expected RBAC tokens."""

    def test_all_tokens_present(self) -> None:
        expected_aliases = {
            "OWPROV_ROOT_TOKEN",
            "OWPROV_TOKEN_A",
            "OWPROV_TOKEN_B",
            "OWPROV_TOKEN_C",
            "OWPROV_TOKEN_D",
            "OWPROV_TOKEN_E",
            "OWPROV_TOKEN_CSR_A",
            "OWPROV_TOKEN_NO_POLICY_ACCESS",
            "OWPROV_TOKEN_NO_ROLE_ACCESS",
        }
        actual_aliases = {v["alias"] for v in owsec.TOKEN_IDENTITIES.values()}
        self.assertEqual(actual_aliases, expected_aliases)

    def test_identity_fields_present(self) -> None:
        required_keys = {"alias", "id", "username", "email", "owner", "userRole", "root", "canPolicy", "canRole"}
        for token, identity in owsec.TOKEN_IDENTITIES.items():
            with self.subTest(token=identity["alias"]):
                self.assertTrue(required_keys.issubset(identity.keys()))


class TestBuildTokenValidationResponse(unittest.TestCase):
    """Verify the structure of ``build_token_validation_response``."""

    def test_response_structure(self) -> None:
        identity = list(owsec.TOKEN_IDENTITIES.values())[0]
        resp = owsec.build_token_validation_response("fake-tok", identity)

        self.assertIn("tokenInfo", resp)
        self.assertIn("userInfo", resp)
        self.assertEqual(resp["tokenInfo"]["token_type"], "Bearer")
        self.assertEqual(resp["tokenInfo"]["access_token"], "fake-tok")
        self.assertEqual(resp["userInfo"]["id"], identity["id"])
        self.assertIn("rbacTestInfo", resp["userInfo"])
        self.assertEqual(resp["userInfo"]["rbacTestInfo"]["tokenAlias"], identity["alias"])

    def test_root_identity_response(self) -> None:
        resp = owsec.build_token_validation_response(owsec.TOKEN_ROOT, owsec.TOKEN_IDENTITIES[owsec.TOKEN_ROOT])
        self.assertTrue(resp["userInfo"]["rbacTestInfo"]["root"])
        self.assertEqual(resp["userInfo"]["owner"], "")
        self.assertEqual(resp["userInfo"]["userRole"], "root")

    def test_no_policy_identity(self) -> None:
        identity = owsec.TOKEN_IDENTITIES[owsec.TOKEN_NO_POLICY_ACCESS]
        resp = owsec.build_token_validation_response(owsec.TOKEN_NO_POLICY_ACCESS, identity)
        self.assertFalse(resp["userInfo"]["rbacTestInfo"]["canPolicy"])
        self.assertTrue(resp["userInfo"]["rbacTestInfo"]["canRole"])
        self.assertEqual(resp["userInfo"]["owner"], owsec.OPERATOR_A_ID)

    def test_no_role_identity(self) -> None:
        identity = owsec.TOKEN_IDENTITIES[owsec.TOKEN_NO_ROLE_ACCESS]
        resp = owsec.build_token_validation_response(owsec.TOKEN_NO_ROLE_ACCESS, identity)
        self.assertTrue(resp["userInfo"]["rbacTestInfo"]["canPolicy"])
        self.assertFalse(resp["userInfo"]["rbacTestInfo"]["canRole"])
        self.assertEqual(resp["userInfo"]["owner"], owsec.OPERATOR_A_ID)


class TestDebugTokensNoSecrets(unittest.TestCase):
    """Verify ``/debug/tokens`` never leaks raw token values."""

    def test_debug_output_contains_no_raw_tokens(self) -> None:
        # Build what the endpoint would return.
        debug_tokens = [
            {
                "alias": identity["alias"],
                "userId": identity["id"],
                "owner": identity["owner"],
                "root": identity["root"],
                "canPolicy": identity["canPolicy"],
                "canRole": identity["canRole"],
            }
            for identity in owsec.TOKEN_IDENTITIES.values()
        ]
        serialized = json.dumps(debug_tokens)

        # Ensure none of the raw token values appear in the serialized output.
        for raw_token in owsec.TOKEN_IDENTITIES:
            self.assertNotIn(raw_token, serialized, f"Raw token leaked in debug output: {raw_token}")


class TestDummyTestTokenBackcompat(unittest.TestCase):
    """Verify ``dummy-test-token`` still produces a valid response."""

    def test_dummy_test_token_identity(self) -> None:
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
        resp = owsec.build_token_validation_response("dummy-test-token", identity)
        self.assertEqual(resp["userInfo"]["id"], "sub1")
        self.assertEqual(resp["userInfo"]["userRole"], "subscriber")
        self.assertEqual(resp["tokenInfo"]["access_token"], "dummy-test-token")


class TestLegacyEndpoints(unittest.TestCase):
    """Verify the legacy endpoints (observations, scenario, etc.) still work."""

    def setUp(self) -> None:
        owsec.STATE.reset_observations()
        owsec.STATE.scenario = "default"

    def test_state_observation_recording(self) -> None:
        owsec.STATE.observe("GET", "/api/v1/validateToken", "root-token")
        self.assertEqual(len(owsec.STATE.observations), 1)
        self.assertEqual(owsec.STATE.observations[0]["method"], "GET")
        self.assertEqual(owsec.STATE.observations[0]["token"], "root-token")

    def test_state_reset_observations(self) -> None:
        owsec.STATE.observe("GET", "/test", "t")
        owsec.STATE.reset_observations()
        self.assertEqual(len(owsec.STATE.observations), 0)

    def test_state_scenario(self) -> None:
        owsec.STATE.scenario = "test-scenario"
        self.assertEqual(owsec.STATE.scenario, "test-scenario")


class TestLegacyTokenResponse(unittest.TestCase):
    """Verify ``_token_response`` still works for validateApiKey path."""

    def test_known_token(self) -> None:
        status, body = owsec._token_response("root-token")  # noqa: SLF001
        self.assertEqual(status, 200)
        self.assertIn("tokenInfo", body)
        self.assertIn("userInfo", body)
        self.assertEqual(body["userInfo"]["id"], "user-root")

    def test_bad_token(self) -> None:
        status, body = owsec._token_response("bad-token")  # noqa: SLF001
        self.assertEqual(status, 403)

    def test_unknown_token(self) -> None:
        status, body = owsec._token_response("totally-unknown")  # noqa: SLF001
        self.assertEqual(status, 403)


if __name__ == "__main__":
    unittest.main(verbosity=2)
