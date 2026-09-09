"""Shared harness for the BillSplitter API integration tests.

These tests drive the real HTTP API against a real PostgreSQL database, because
that is where this system's logic actually lives: the money arithmetic is in
NUMERIC expressions, the authorization rules are joins, and the validation is in
handler code. A unit test of a C++ helper would exercise none of it.

Standard library only — no pip install, so CI needs nothing but Python.

Run via scripts/run-api-tests.sh, which points these at a throwaway database.
Never run them against the development database: they create and delete freely.
"""

import http.cookiejar
import json
import os
import urllib.error
import urllib.request
import uuid

BASE_URL = os.environ.get("BILLSPLITTER_TEST_URL", "http://localhost:8099")


class Response:
    """A parsed HTTP response. `.json` is None for a body that is not JSON."""

    def __init__(self, status, body, headers):
        self.status = status
        self.body = body
        self.headers = headers
        try:
            self.json = json.loads(body) if body else None
        except json.JSONDecodeError:
            self.json = None

    @property
    def error(self):
        """The `error` field the API returns on every failure, or None."""
        return self.json.get("error") if isinstance(self.json, dict) else None

    def __repr__(self):
        return f"<Response {self.status} {self.body[:120]!r}>"


class Client:
    """One API client with its own cookie jar, i.e. its own session.

    Tests that need two users create two Clients; that is how the ownership
    tests prove one user cannot reach another's data.
    """

    def __init__(self, base_url=BASE_URL):
        self.base_url = base_url
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar)
        )
        self.user = None

    def request(self, method, path, body=None):
        url = self.base_url + path
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Content-Type", "application/json")
        try:
            # A hung request is a real failure mode here (require_auth once held
            # connections open forever), so never wait indefinitely.
            with self.opener.open(req, timeout=15) as resp:
                return Response(resp.status, resp.read().decode(), dict(resp.headers))
        except urllib.error.HTTPError as e:
            return Response(e.code, e.read().decode(), dict(e.headers))

    def get(self, path):
        return self.request("GET", path)

    def post(self, path, body=None):
        return self.request("POST", path, body if body is not None else {})

    def put(self, path, body=None):
        return self.request("PUT", path, body if body is not None else {})

    def delete(self, path):
        return self.request("DELETE", path)

    # ── auth ────────────────────────────────────────────────────────────────

    def register(self, email=None, password="hunter2hunter2", display_name=None):
        email = email or f"t-{uuid.uuid4().hex[:12]}@example.com"
        r = self.post(
            "/api/auth/register",
            {"email": email, "password": password, "display_name": display_name or ""},
        )
        assert r.status == 201, f"register failed: {r}"
        self.user = r.json
        self.user["email"] = email
        self.user["password"] = password
        return self.user

    def login(self, email, password="hunter2hunter2"):
        r = self.post("/api/auth/login", {"email": email, "password": password})
        assert r.status == 200, f"login failed: {r}"
        self.user = r.json
        return self.user

    # ── fixture builders ────────────────────────────────────────────────────
    # Each returns the created object and asserts the create succeeded, so a
    # test that fails does so on its own assertion rather than on a KeyError
    # three lines later.

    def make_split(self, name=None, type="one_time", currency=None):
        body = {"name": name or f"Split {uuid.uuid4().hex[:6]}", "type": type}
        if currency:
            body["currency"] = currency
        r = self.post("/api/splits", body)
        assert r.status == 201, f"make_split failed: {r}"
        return r.json

    def make_member(self, split_id, name="Member", email=None):
        body = {"name": name}
        if email:
            body["email"] = email
        r = self.post(f"/api/splits/{split_id}/members", body)
        assert r.status == 201, f"make_member failed: {r}"
        return r.json

    def make_bill(self, split_id, store_name="Store", date="2026-01-15", **kw):
        body = {"store_name": store_name, "date": date}
        body.update(kw)
        r = self.post(f"/api/splits/{split_id}/bills", body)
        assert r.status == 201, f"make_bill failed: {r}"
        return r.json

    def make_item(self, bill_id, name="Item", price="10.00", quantity=None, **kw):
        body = {"name": name, "price": price}
        if quantity is not None:
            body["quantity"] = quantity
        body.update(kw)
        r = self.post(f"/api/bills/{bill_id}/items", body)
        assert r.status == 201, f"make_item failed: {r}"
        return r.json

    def allocate(self, bill_id, item_id, mode, allocations):
        return self.put(
            f"/api/bills/{bill_id}/items/{item_id}/allocations",
            {"mode": mode, "allocations": allocations},
        )


def new_user(display_name=None):
    """A freshly registered, logged-in client. Each test gets its own users so
    tests never contend for the same rows and can run in any order."""
    c = Client()
    c.register(display_name=display_name)
    return c


# A syntactically valid uuid that will never exist, for not-found assertions.
MISSING_UUID = "00000000-0000-0000-0000-000000000000"

# Strings that must never produce a 500 when used as a path parameter.
BAD_IDS = ["not-a-uuid", "', 'x", "%27%3BDROP%20TABLE%20splits%3B--", "123"]


class ApiTestCase:
    """Assertions shared across the suites. Mix in alongside unittest.TestCase."""

    def assertStatus(self, resp, expected, msg=""):
        self.assertEqual(
            resp.status, expected,
            f"{msg}expected {expected}, got {resp.status}: {resp.body[:200]}",
        )

    def assertError(self, resp, expected_status):
        """A failure must carry the documented {"error": "..."} envelope —
        never an empty body, an HTML page, or a bare string."""
        self.assertStatus(resp, expected_status)
        self.assertIsInstance(
            resp.json, dict, f"body was not a JSON object: {resp.body[:200]}"
        )
        self.assertIn("error", resp.json, f"no error field: {resp.body[:200]}")
        self.assertTrue(resp.json["error"], "error message was empty")

    def assertMoneyString(self, value, field=""):
        """Money crosses the wire as a string, never a JSON number. A float here
        is the bug the whole representation exists to prevent, so assert the
        type rather than the value."""
        self.assertIsInstance(
            value, str, f"{field} must be a JSON string, got {type(value).__name__}: {value!r}"
        )
        self.assertRegex(value, r"^-?\d+\.\d{4}$", f"{field} must have 4 decimal places")
