"""Tests for the operational hardening: login throttling and session cleanup.

The runner sets LOGIN_MAX_FAILURES low so the limit is reachable without
hammering the server; see scripts/run-api-tests.sh.
"""
import os
import time
import unittest
from harness import Client, new_user, ApiTestCase

MAX_FAILURES = int(os.environ.get("LOGIN_MAX_FAILURES", "10"))


class TestLoginThrottle(ApiTestCase, unittest.TestCase):
    def test_repeated_failures_eventually_return_429(self):
        user = new_user().user
        c = Client()
        seen_429 = False
        for _ in range(MAX_FAILURES + 2):
            r = c.post("/api/auth/login", {"email": user["email"], "password": "wrongwrongwrong"})
            self.assertIn(r.status, (401, 429), f"unexpected status: {r}")
            if r.status == 429:
                seen_429 = True
                self.assertError(r, 429)
                break
        self.assertTrue(seen_429, f"never throttled after {MAX_FAILURES + 2} failures")

    def test_a_successful_login_clears_the_failure_history(self):
        """Only failures count. A user who mistypes, succeeds, then mistypes
        again must not be closer to a lockout than someone starting fresh."""
        user = new_user().user
        c = Client()
        for _ in range(MAX_FAILURES - 1):
            c.post("/api/auth/login", {"email": user["email"], "password": "nope-nope-nope"})
        ok = c.post("/api/auth/login", {"email": user["email"], "password": user["password"]})
        self.assertStatus(ok, 200, "a correct password should still work below the limit: ")
        # History cleared, so another near-limit run of failures still 401s.
        for i in range(MAX_FAILURES - 1):
            r = c.post("/api/auth/login", {"email": user["email"], "password": "nope-nope-nope"})
            self.assertStatus(r, 401, f"failure {i} after a success was throttled: ")

    def test_throttling_one_account_does_not_block_another(self):
        victim, bystander = new_user().user, new_user().user
        c = Client()
        for _ in range(MAX_FAILURES + 2):
            c.post("/api/auth/login", {"email": victim["email"], "password": "wrongwrongwrong"})
        r = Client().post("/api/auth/login",
                          {"email": bystander["email"], "password": bystander["password"]})
        self.assertStatus(r, 200, "an unrelated account was collateral damage: ")

    def test_ordinary_repeated_logins_are_never_throttled(self):
        """The suite itself logs in constantly; if success counted toward the
        limit this would fail, and so would every real user."""
        user = new_user().user
        for i in range(MAX_FAILURES + 5):
            r = Client().post("/api/auth/login",
                              {"email": user["email"], "password": user["password"]})
            self.assertStatus(r, 200, f"successful login {i} was throttled: ")


class TestErrorEnvelope(ApiTestCase, unittest.TestCase):
    """Every failure carries {"error": ...}, including ones the framework
    generates rather than a handler."""

    def test_unrouted_path_returns_json(self):
        for path in ["/api/nope", "/api/splits/", "/nope", "/"]:
            r = Client().get(path)
            self.assertError(r, 404)

    def test_wrong_method_returns_json(self):
        r = Client().delete("/api/health")
        self.assertStatus(r, 405)
        self.assertIsInstance(r.json, dict, f"not JSON: {r.body[:120]}")
        self.assertIn("error", r.json)


class TestSessionLifecycle(ApiTestCase, unittest.TestCase):
    def test_logout_invalidates_only_that_session(self):
        user = new_user().user
        a, b = Client(), Client()
        a.login(user["email"], user["password"])
        b.login(user["email"], user["password"])
        self.assertStatus(a.post("/api/auth/logout"), 200)
        self.assertStatus(a.get("/api/auth/me"), 401, "logged-out session still works: ")
        self.assertStatus(b.get("/api/auth/me"), 200, "logout killed an unrelated session: ")
