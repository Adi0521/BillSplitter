"""Phase 1 — Auth.

Everything here is asserted against docs/api.md, not against what the handlers
happen to do. The two things worth stating up front, because they are the ones
that quietly become security bugs:

  * A failed login must not reveal whether the email exists. "No such user" and
    "wrong password" have to be indistinguishable, message included.
  * A 401 must be *returned*. `require_auth` once called `res.end()`, which made
    Crow skip the write path entirely and hold the connection open until it
    timed out; a handful of unauthenticated requests could then occupy every
    worker thread. A test that only checks the status code passes happily while
    that bug is live, so the elapsed time is asserted too.
"""
import json
import time
import unittest
import urllib.error
import urllib.request
import uuid

from harness import BASE_URL, ApiTestCase, Client, Response, new_user


def raw_request(method, path, headers=None, body=None):
    """A request with arbitrary headers and *no* cookie jar.

    The harness Client always carries its own cookies, which is exactly wrong
    for testing a garbage cookie or a bearer token, so those tests build the
    request themselves.
    """
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE_URL + path, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return Response(resp.status, resp.read().decode(), dict(resp.headers))
    except urllib.error.HTTPError as e:
        return Response(e.code, e.read().decode(), dict(e.headers))


def session_token(resp):
    """The session token out of a Set-Cookie header."""
    cookie = resp.headers.get("Set-Cookie", "")
    assert "session=" in cookie, f"no session cookie in {cookie!r}"
    return cookie.split("session=", 1)[1].split(";", 1)[0]


def fresh_email():
    return f"auth-{uuid.uuid4().hex[:12]}@example.com"


PASSWORD = "hunter2hunter2"

# Anything that would mean the hash, the salt, or the plaintext escaped into a
# response body. Checked as a substring of the raw body so a nested or renamed
# field cannot slip past a key-by-key check.
SECRET_MARKERS = ["password", "hash", "salt", PASSWORD]


class TestRegister(ApiTestCase, unittest.TestCase):

    def test_register_returns_201_with_the_user_and_a_session_cookie(self):
        email = fresh_email()
        r = Client().post("/api/auth/register",
                          {"email": email, "password": PASSWORD, "display_name": "Alice"})
        self.assertStatus(r, 201)
        self.assertEqual(r.json["email"], email)
        self.assertEqual(r.json["display_name"], "Alice")
        self.assertTrue(r.json["id"], "register must return the new user's id")
        cookie = r.headers.get("Set-Cookie", "")
        self.assertIn("session=", cookie, "register must set a session cookie")
        self.assertIn("HttpOnly", cookie, "the session cookie must be HttpOnly")

    def test_register_body_contains_no_password_hash_salt_or_plaintext(self):
        r = Client().post("/api/auth/register",
                          {"email": fresh_email(), "password": PASSWORD, "display_name": "Alice"})
        self.assertStatus(r, 201)
        for marker in SECRET_MARKERS:
            self.assertNotIn(marker, r.body.lower(),
                             f"register body leaked {marker!r}: {r.body}")

    def test_register_and_login_return_the_documented_user_shape(self):
        """docs/api.md: `User` is {id, email, display_name, created_at} and both
        register and login are documented as returning a `User`."""
        email = fresh_email()
        reg = Client().post("/api/auth/register",
                            {"email": email, "password": PASSWORD, "display_name": "Alice"})
        self.assertStatus(reg, 201)
        self.assertEqual(sorted(reg.json.keys()),
                         ["created_at", "display_name", "email", "id"],
                         f"register returned {sorted(reg.json.keys())}")

        log = Client().post("/api/auth/login", {"email": email, "password": PASSWORD})
        self.assertStatus(log, 200)
        self.assertEqual(sorted(log.json.keys()),
                         ["created_at", "display_name", "email", "id"],
                         f"login returned {sorted(log.json.keys())}")

    def test_registering_the_same_email_twice_is_409(self):
        email = fresh_email()
        first = Client().post("/api/auth/register", {"email": email, "password": PASSWORD})
        self.assertStatus(first, 201)
        second = Client().post("/api/auth/register", {"email": email, "password": PASSWORD})
        self.assertError(second, 409)

    def test_a_password_shorter_than_the_minimum_is_400(self):
        r = Client().post("/api/auth/register", {"email": fresh_email(), "password": "short"})
        self.assertError(r, 400)

    def test_an_email_with_no_at_sign_is_400(self):
        r = Client().post("/api/auth/register",
                          {"email": "not-an-email", "password": PASSWORD})
        self.assertError(r, 400)

    def test_missing_email_missing_password_and_empty_body_are_all_400(self):
        for body in ({}, {"email": fresh_email()}, {"password": PASSWORD},
                     {"email": "", "password": ""}):
            with self.subTest(body=body):
                self.assertError(Client().post("/api/auth/register", body), 400)

    def test_a_failed_register_creates_no_session(self):
        c = Client()
        self.assertError(c.post("/api/auth/register",
                                {"email": "no-at-sign", "password": PASSWORD}), 400)
        self.assertError(c.get("/api/auth/me"), 401)


class TestLogin(ApiTestCase, unittest.TestCase):

    def test_login_with_the_correct_password_returns_200_and_a_session(self):
        c = new_user(display_name="Bob")
        fresh = Client()
        r = fresh.post("/api/auth/login",
                       {"email": c.user["email"], "password": c.user["password"]})
        self.assertStatus(r, 200)
        self.assertEqual(r.json["id"], c.user["id"])
        self.assertIn("session=", r.headers.get("Set-Cookie", ""))
        # The cookie the login handed back is a working session.
        self.assertStatus(fresh.get("/api/auth/me"), 200)

    def test_login_with_the_wrong_password_is_401(self):
        c = new_user()
        r = Client().post("/api/auth/login",
                          {"email": c.user["email"], "password": "wrongwrongwrong"})
        self.assertError(r, 401)

    def test_login_for_an_unregistered_email_is_401(self):
        r = Client().post("/api/auth/login",
                          {"email": fresh_email(), "password": PASSWORD})
        self.assertError(r, 401)

    def test_wrong_password_and_unknown_email_are_indistinguishable(self):
        """An error that tells the two apart is a user-enumeration oracle: an
        attacker learns which addresses have accounts without guessing one
        password. Status *and* message must match."""
        c = new_user()
        wrong_password = Client().post(
            "/api/auth/login", {"email": c.user["email"], "password": "wrongwrongwrong"})
        unknown_email = Client().post(
            "/api/auth/login", {"email": fresh_email(), "password": PASSWORD})

        self.assertError(wrong_password, 401)
        self.assertError(unknown_email, 401)
        self.assertEqual(wrong_password.error, unknown_email.error,
                         "login errors must not distinguish a bad password from "
                         "an unknown email")

    def test_a_failed_login_creates_no_session(self):
        c = new_user()
        attacker = Client()
        self.assertError(attacker.post("/api/auth/login",
                                       {"email": c.user["email"], "password": "nope-nope-nope"}),
                         401)
        self.assertError(attacker.get("/api/auth/me"), 401)

    def test_login_with_missing_fields_is_400(self):
        for body in ({}, {"email": fresh_email()}, {"password": PASSWORD}):
            with self.subTest(body=body):
                self.assertError(Client().post("/api/auth/login", body), 400)

    def test_login_body_contains_no_password_hash_salt_or_plaintext(self):
        c = new_user()
        r = Client().post("/api/auth/login",
                          {"email": c.user["email"], "password": c.user["password"]})
        self.assertStatus(r, 200)
        for marker in SECRET_MARKERS:
            self.assertNotIn(marker, r.body.lower(),
                             f"login body leaked {marker!r}: {r.body}")


class TestMe(ApiTestCase, unittest.TestCase):

    def test_me_with_a_session_cookie_returns_the_current_user(self):
        c = new_user(display_name="Carol")
        r = c.get("/api/auth/me")
        self.assertStatus(r, 200)
        self.assertEqual(r.json["id"], c.user["id"])
        self.assertEqual(r.json["email"], c.user["email"])
        self.assertEqual(r.json["display_name"], "Carol")
        self.assertTrue(r.json.get("created_at"), "User must carry created_at")

    def test_me_with_no_cookie_at_all_is_401(self):
        self.assertError(raw_request("GET", "/api/auth/me"), 401)

    def test_me_with_a_garbage_cookie_is_401(self):
        for value in ("session=garbage", "session=", "session=" + "0" * 64,
                      "session=%27%3B--"):
            with self.subTest(cookie=value):
                self.assertError(
                    raw_request("GET", "/api/auth/me", {"Cookie": value}), 401)

    def test_me_returns_no_password_hash_salt_or_plaintext(self):
        c = new_user()
        r = c.get("/api/auth/me")
        for marker in SECRET_MARKERS:
            self.assertNotIn(marker, r.body.lower(),
                             f"/api/auth/me leaked {marker!r}: {r.body}")


class TestBearerToken(ApiTestCase, unittest.TestCase):

    def test_a_bearer_token_authenticates_just_like_the_cookie(self):
        email = fresh_email()
        reg = Client().post("/api/auth/register",
                            {"email": email, "password": PASSWORD, "display_name": "Dave"})
        self.assertStatus(reg, 201)
        token = session_token(reg)

        r = raw_request("GET", "/api/auth/me", {"Authorization": f"Bearer {token}"})
        self.assertStatus(r, 200)
        self.assertEqual(r.json["email"], email)

    def test_a_bearer_token_authorizes_a_split_route_too(self):
        c = new_user()
        login = Client().post("/api/auth/login",
                              {"email": c.user["email"], "password": c.user["password"]})
        split = c.make_split(name="Bearer split")
        r = raw_request("GET", f"/api/splits/{split['id']}",
                        {"Authorization": f"Bearer {session_token(login)}"})
        self.assertStatus(r, 200)
        self.assertEqual(r.json["id"], split["id"])

    def test_a_garbage_or_malformed_bearer_token_is_401(self):
        for header in ("Bearer garbage", "Bearer ", "Bearer " + "f" * 64,
                       "Basic hunter2", "garbage"):
            with self.subTest(authorization=header):
                self.assertError(
                    raw_request("GET", "/api/auth/me", {"Authorization": header}), 401)


class TestLogout(ApiTestCase, unittest.TestCase):

    def test_logout_returns_the_documented_message(self):
        c = new_user()
        r = c.post("/api/auth/logout")
        self.assertStatus(r, 200)
        self.assertEqual(r.json, {"message": "Logged out"})

    def test_logout_invalidates_the_session_so_me_is_401_afterwards(self):
        c = new_user()
        self.assertStatus(c.get("/api/auth/me"), 200)
        self.assertStatus(c.post("/api/auth/logout"), 200)
        self.assertError(c.get("/api/auth/me"), 401)

    def test_logout_invalidates_the_bearer_token_too(self):
        """The cookie and the header name the same server-side session, so
        killing the session must kill both — a token that still works after
        logout is a session that was never really destroyed."""
        email = fresh_email()
        reg = Client().post("/api/auth/register", {"email": email, "password": PASSWORD})
        token = session_token(reg)
        auth = {"Authorization": f"Bearer {token}"}
        self.assertStatus(raw_request("GET", "/api/auth/me", auth), 200)
        self.assertStatus(raw_request("POST", "/api/auth/logout", auth, {}), 200)
        self.assertError(raw_request("GET", "/api/auth/me", auth), 401)

    def test_logout_without_a_session_is_still_200(self):
        self.assertStatus(raw_request("POST", "/api/auth/logout", None, {}), 200)

    def test_one_users_logout_does_not_touch_another_users_session(self):
        a, b = new_user(), new_user()
        self.assertStatus(a.post("/api/auth/logout"), 200)
        self.assertStatus(b.get("/api/auth/me"), 200)


class TestUnauthenticatedRequestsAreAnswered(ApiTestCase, unittest.TestCase):
    """A 401 must be written and flushed, not merely decided on.

    The regression this guards against did not change any status code: the
    handler filled in a 401 and then called res.end(), Crow saw the response as
    already completed, never wrote it, and left the socket open until it timed
    out. The client's symptom is a hang, so the assertion has to be about time.
    """

    # The harness allows 15s. Anything healthy answers in milliseconds; a
    # second is already generous enough that this cannot flake on a slow
    # laptop, while still failing loudly on a held-open connection.
    BUDGET_SECONDS = 2.0

    def _assert_prompt_401(self, method, path):
        start = time.monotonic()
        r = Client().request(method, path, {} if method in ("POST", "PUT") else None)
        elapsed = time.monotonic() - start
        self.assertError(r, 401)
        self.assertLess(elapsed, self.BUDGET_SECONDS,
                        f"{method} {path} took {elapsed:.2f}s to return its 401; "
                        "an unauthenticated request must be answered immediately, "
                        "not held open until the connection times out")

    def test_an_unauthenticated_get_on_a_require_auth_route_returns_promptly(self):
        self._assert_prompt_401("GET", "/api/splits")

    def test_an_unauthenticated_post_put_and_delete_return_promptly(self):
        self._assert_prompt_401("POST", "/api/splits")
        self._assert_prompt_401("PUT", "/api/splits/00000000-0000-0000-0000-000000000000")
        self._assert_prompt_401("DELETE", "/api/splits/00000000-0000-0000-0000-000000000000")

    def test_repeated_unauthenticated_requests_do_not_exhaust_the_server(self):
        """Twenty in a row on one connection-per-request client. If each 401
        pinned a worker thread this stops answering long before the last one."""
        start = time.monotonic()
        for _ in range(20):
            self.assertError(Client().get("/api/splits"), 401)
        elapsed = time.monotonic() - start
        self.assertLess(elapsed, 10.0,
                        f"20 unauthenticated requests took {elapsed:.1f}s")

    def test_the_server_still_serves_authenticated_traffic_afterwards(self):
        for _ in range(5):
            Client().get("/api/splits")
        c = new_user()
        self.assertStatus(c.get("/api/auth/me"), 200)


class TestNoPasswordMaterialAnywhere(ApiTestCase, unittest.TestCase):

    def test_no_auth_split_or_member_response_body_carries_password_material(self):
        c = new_user(display_name="Erin")
        split = c.make_split(name="Leak check")
        c.make_member(split["id"], name="Frank", email="frank@example.com")

        bodies = {
            "GET /api/auth/me": c.get("/api/auth/me"),
            "GET /api/splits": c.get("/api/splits"),
            "GET /api/splits/:id": c.get(f"/api/splits/{split['id']}"),
            "GET /api/splits/:id/members": c.get(f"/api/splits/{split['id']}/members"),
            "POST /api/auth/logout": c.post("/api/auth/logout"),
        }
        for label, resp in bodies.items():
            for marker in SECRET_MARKERS:
                with self.subTest(endpoint=label, marker=marker):
                    self.assertNotIn(marker, resp.body.lower(),
                                     f"{label} leaked {marker!r}: {resp.body[:300]}")


if __name__ == "__main__":
    unittest.main()
