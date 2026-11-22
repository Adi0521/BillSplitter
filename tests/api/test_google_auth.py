"""Sign in with Google — the account-linking rules and seat matching.

Driven through the fake identity provider (AUTH_FAKE_IDENTITY=1), which accepts
base64url(json) as the authorization code instead of calling Google. That is the
only way to walk every row of the linking table in docs/api.md offline, and the
rows are where the security of this feature lives:

  1. a known google_sub signs the same person in
  2. a VERIFIED local account links the sub and keeps its password
  3. an UNVERIFIED local account has its sub linked, its address verified and
     its password WIPED — the squatter eviction, the most important test here
  4. no match at all creates a passwordless, verified account

Plus the two things that make the flow safe rather than merely working: `state`
(no session may be created without a matching one) and the rule that a seat is
only ever matched to an account whose email Google actually verified.

Nothing here reads the database to make its assertions, with one exception noted
inline: the checks are "can this person sign in", "is the seat theirs", "does the
password still work" — the properties a user would notice.
"""
import base64
import json
import os
import subprocess
import unittest
import urllib.error
import urllib.parse
import urllib.request
import uuid

from harness import BASE_URL, ApiTestCase, Client, Response, new_user

# The base the server redirects back to; matches APP_BASE_URL in the runner.
APP_BASE = os.environ.get("BILLSPLITTER_APP_BASE_URL", "http://localhost:5173")

# A second server started with nothing configured — no GOOGLE_CLIENT_ID and no
# AUTH_FAKE_IDENTITY — so /start's 503 can be exercised. Skipped when unset.
UNCONFIGURED_URL = os.environ.get("BILLSPLITTER_UNCONFIGURED_URL")

# Direct database access, used by exactly one test: an account that is both
# email-verified AND has a password cannot be produced through the API (nothing
# verifies an address except Google, and Google sign-in leaves no password), yet
# it is precisely row 2 of the linking table. Skipped when unset.
DB_URL = os.environ.get("BILLSPLITTER_TEST_DB_URL")

PASSWORD = "hunter2hunter2"

# The one body a failed password login may ever produce, whatever the reason.
LOGIN_FAILURE_BODY = {"error": "Invalid email or password"}


# ── A client that does not follow redirects ───────────────────────────────────
# The whole flow is 302s, and urllib's default opener would chase the Location
# header to APP_BASE_URL, where nothing is listening. Cookies are tracked by
# hand rather than with http.cookiejar so that clearing one (Max-Age=0) is
# asserted here rather than delegated to the library's interpretation of it.

class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class GClient:
    def __init__(self, base_url=BASE_URL):
        self.base_url = base_url
        self.cookies = {}
        self.opener = urllib.request.build_opener(_NoRedirect)

    def request(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base_url + path, data=data, method=method)
        req.add_header("Content-Type", "application/json")
        if self.cookies:
            req.add_header("Cookie", "; ".join(f"{k}={v}" for k, v in self.cookies.items()))
        try:
            with self.opener.open(req, timeout=15) as resp:
                r = Response(resp.status, resp.read().decode(), dict(resp.headers))
                r.set_cookies = resp.headers.get_all("Set-Cookie") or []
        except urllib.error.HTTPError as e:
            r = Response(e.code, e.read().decode(), dict(e.headers))
            r.set_cookies = e.headers.get_all("Set-Cookie") or []
        self._absorb(r.set_cookies)
        return r

    def get(self, path):
        return self.request("GET", path)

    def post(self, path, body=None):
        return self.request("POST", path, body if body is not None else {})

    def _absorb(self, set_cookies):
        for raw in set_cookies:
            name, _, rest = raw.partition("=")
            name = name.strip()
            value = rest.split(";", 1)[0].strip()
            if "max-age=0" in raw.lower():
                self.cookies.pop(name, None)
            else:
                self.cookies[name] = value


def fake_code(sub, email, name="", email_verified=True):
    """The fake provider's authorization code: base64url of the identity JSON."""
    payload = json.dumps({
        "sub": sub,
        "email": email,
        "name": name,
        "email_verified": email_verified,
    }).encode()
    return base64.urlsafe_b64encode(payload).decode().rstrip("=")


def query_of(url):
    return urllib.parse.parse_qs(urllib.parse.urlparse(url).query)


def state_of(start_response):
    """The `state` the server sent to the consent screen."""
    loc = start_response.headers["Location"]
    return query_of(loc)["state"][0]


def cookie_of(response, name):
    for raw in response.set_cookies:
        if raw.startswith(name + "="):
            return raw
    return None


def fresh_email(tag="g"):
    return f"{tag}-{uuid.uuid4().hex[:12]}@example.com"


def fresh_sub():
    return "sub-" + uuid.uuid4().hex


def start(client, redirect=None):
    path = "/api/auth/google/start"
    if redirect is not None:
        path += "?redirect=" + urllib.parse.quote(redirect, safe="")
    return client.get(path)


def sign_in(client, sub, email, name="", email_verified=True, redirect=None):
    """The full flow: /start, then /callback with the matching state."""
    started = start(client, redirect)
    assert started.status == 302, f"/start did not redirect: {started}"
    code = fake_code(sub, email, name, email_verified)
    return client.get("/api/auth/google/callback"
                      f"?code={urllib.parse.quote(code)}&state={state_of(started)}")


def db_exec(sql):
    psql = "psql"
    subprocess.run([psql, DB_URL, "-v", "ON_ERROR_STOP=1", "-q", "-c", sql],
                   check=True, capture_output=True, text=True)


def setUpModule():
    probe = GClient().get("/api/auth/google/start")
    if probe.status == 404:
        raise unittest.SkipTest(
            "/api/auth/google/start is not registered "
            "(add register_google_routes(app, pool) to main.cpp)")
    if probe.status == 503:
        raise unittest.SkipTest(
            "Google sign-in is not configured on the server under test; "
            "run it with AUTH_FAKE_IDENTITY=1")


class TestStart(ApiTestCase, unittest.TestCase):

    def test_start_redirects_to_consent_and_sets_a_short_lived_state_cookie(self):
        r = start(GClient())
        self.assertStatus(r, 302)
        self.assertIn("Location", r.headers)

        cookie = cookie_of(r, "g_state")
        self.assertIsNotNone(cookie, f"no g_state cookie: {r.set_cookies}")
        self.assertIn("HttpOnly", cookie)
        self.assertIn("Path=/api/auth/google", cookie,
                      "the state cookie must not be sent to the rest of the app")
        self.assertIn("Max-Age=600", cookie)
        self.assertIn("SameSite=Lax", cookie)

    def test_state_in_the_url_is_a_32_hex_nonce_and_is_not_the_cookie_itself(self):
        r = start(GClient(), redirect="/invite/abcd")
        state = state_of(r)
        self.assertRegex(state, r"^[0-9a-f]{32}$")

        # Only the nonce travels to Google. The redirect target stays in the
        # HttpOnly cookie, so what finally lands in a Location header never made
        # a round trip through an external service.
        cookie_value = cookie_of(r, "g_state").split("=", 1)[1].split(";", 1)[0]
        self.assertNotEqual(cookie_value, state)
        self.assertNotIn("invite", r.headers["Location"],
                         "the redirect path must not be sent to Google")

    def test_start_is_safe_to_call_twice(self):
        # The frontend preflights /start with redirect:'manual' to detect the
        # 503, then navigates for real, so it is hit twice per sign-in. The
        # first call must not invalidate the second.
        c = GClient()
        first = start(c)
        second = start(c)
        self.assertStatus(first, 302)
        self.assertStatus(second, 302)
        self.assertNotEqual(state_of(first), state_of(second))

        code = fake_code(fresh_sub(), fresh_email())
        done = c.get(f"/api/auth/google/callback?code={code}&state={state_of(second)}")
        self.assertStatus(done, 302, "the most recently issued state must work: ")

    @unittest.skipUnless(UNCONFIGURED_URL,
                         "needs a server with no GOOGLE_CLIENT_ID and no AUTH_FAKE_IDENTITY")
    def test_start_is_503_when_google_is_not_configured(self):
        r = GClient(UNCONFIGURED_URL).get("/api/auth/google/start")
        self.assertError(r, 503)
        self.assertNotIn("Location", r.headers,
                         "a redirect to a consent screen that cannot work is worse "
                         "than an honest failure")


class TestRedirectTarget(ApiTestCase, unittest.TestCase):

    def final_location(self, redirect):
        r = sign_in(GClient(), fresh_sub(), fresh_email(), redirect=redirect)
        self.assertStatus(r, 302)
        return r.headers["Location"]

    def test_a_relative_path_is_honoured(self):
        loc = self.final_location("/invite/7f3a")
        self.assertTrue(loc.startswith(APP_BASE + "/invite/7f3a?"), loc)
        self.assertIn("signed_in=google", loc)

    def test_flags_are_appended_to_the_paths_own_query(self):
        loc = self.final_location("/invite/7f3a?from=email")
        self.assertIn("from=email", loc)
        self.assertIn("&signed_in=google", loc)

    def test_hostile_redirects_fall_back_to_the_app_root(self):
        for hostile in ["//evil.com", "https://evil.com", "http://evil.com/x",
                        "javascript:alert(1)", "/\\evil.com", "evil.com",
                        "/x\r\nSet-Cookie: a=b", ""]:
            with self.subTest(redirect=hostile):
                loc = self.final_location(hostile)
                self.assertNotIn("evil", loc)
                self.assertNotIn("javascript", loc)
                self.assertTrue(loc.startswith(APP_BASE + "/?signed_in=google"), loc)

    def test_the_redirect_never_carries_the_code_the_token_or_the_email(self):
        c = GClient()
        email = fresh_email()
        sub = fresh_sub()
        r = sign_in(c, sub, email)
        loc = r.headers["Location"]
        self.assertNotIn(email, loc)
        self.assertNotIn(email.split("@")[0], loc)
        self.assertNotIn(sub, loc)
        self.assertNotIn(c.cookies["session"], loc)


class TestNewGoogleUser(ApiTestCase, unittest.TestCase):

    def test_a_brand_new_google_user_gets_a_session_and_an_account(self):
        c = GClient()
        email = fresh_email()
        r = sign_in(c, fresh_sub(), email, name="Grace Google")
        self.assertStatus(r, 302)
        self.assertTrue(any(raw.startswith("session=") for raw in r.set_cookies),
                        f"no session cookie: {r.set_cookies}")

        me = c.get("/api/auth/me")
        self.assertStatus(me, 200)
        self.assertEqual(me.json["email"], email)
        self.assertEqual(me.json["display_name"], "Grace Google")

    def test_the_new_account_has_no_password(self):
        email = fresh_email()
        sign_in(GClient(), fresh_sub(), email)

        r = Client().post("/api/auth/login", {"email": email, "password": PASSWORD})
        self.assertStatus(r, 401)
        self.assertEqual(r.json, LOGIN_FAILURE_BODY)

    def test_the_state_cookie_is_cleared_after_a_successful_sign_in(self):
        c = GClient()
        r = sign_in(c, fresh_sub(), fresh_email())
        self.assertStatus(r, 302)
        self.assertNotIn("g_state", c.cookies)

    def test_signing_in_twice_reuses_the_same_account(self):
        sub, email = fresh_sub(), fresh_email()
        first = GClient()
        sign_in(first, sub, email)
        first_id = first.get("/api/auth/me").json["id"]

        second = GClient()
        sign_in(second, sub, email)
        self.assertEqual(second.get("/api/auth/me").json["id"], first_id)

    def test_the_address_is_refreshed_from_google(self):
        # Row 1: the subject identifies the person, so a changed address follows
        # them rather than creating a second account.
        sub = fresh_sub()
        c = GClient()
        sign_in(c, sub, fresh_email())
        first_id = c.get("/api/auth/me").json["id"]

        moved = fresh_email("moved")
        sign_in(c, sub, moved)
        me = c.get("/api/auth/me")
        self.assertEqual(me.json["id"], first_id)
        self.assertEqual(me.json["email"], moved)


class TestLinkingToAVerifiedAccount(ApiTestCase, unittest.TestCase):
    """Row 2 — the address was already proven, so the accounts are one person."""

    def test_a_second_google_subject_links_to_the_verified_account(self):
        email = fresh_email()
        first = GClient()
        sign_in(first, fresh_sub(), email)
        user_id = first.get("/api/auth/me").json["id"]

        # A different subject, same verified address: no new account.
        second = GClient()
        r = sign_in(second, fresh_sub(), email)
        self.assertStatus(r, 302)
        self.assertEqual(second.get("/api/auth/me").json["id"], user_id)

    @unittest.skipUnless(DB_URL, "needs BILLSPLITTER_TEST_DB_URL to build a "
                                 "verified account that still has a password")
    def test_a_verified_account_keeps_its_password(self):
        email = fresh_email("verified")
        owner = Client()
        owner.register(email=email, password=PASSWORD)
        registered_id = owner.user["id"]

        # The one state the API cannot produce: verified AND password-holding.
        db_exec("UPDATE users SET email_verified = TRUE, verified_at = now() "
                f"WHERE email = '{email}'")

        r = sign_in(GClient(), fresh_sub(), email)
        self.assertStatus(r, 302)
        self.assertNotIn("password_cleared", r.headers["Location"],
                         "a verified account's password must survive linking")

        after = Client()
        after.login(email, PASSWORD)
        self.assertEqual(after.user["id"], registered_id)


class TestSquatterEviction(ApiTestCase, unittest.TestCase):
    """Row 3 — the account's address was never proven, so Google wins.

    This is the takeover the whole feature exists to close: anyone could
    register as someone@gmail.com, because registration never checked. When
    Google proves the address, the sub is linked, the address becomes verified,
    and the password is cleared. The account keeps all its data.
    """

    def test_the_password_stops_working_and_google_signs_the_owner_in(self):
        email = fresh_email("squatted")
        squatter = Client()
        squatter.register(email=email, password=PASSWORD)
        account_id = squatter.user["id"]

        # A seat waiting for this address, to prove the eviction also verifies.
        owner = new_user(display_name="Owner")
        split = owner.make_split(name="Eviction split")
        owner.make_member(split["id"], name="Sam", email=email)

        google = GClient()
        r = sign_in(google, fresh_sub(), email, name="Sam")
        self.assertStatus(r, 302)
        loc = r.headers["Location"]
        self.assertIn("password_cleared=1", loc,
                      "the UI has to be able to say what happened")
        self.assertIn("seats=1", loc)

        # The password no longer works — and fails exactly as a wrong password
        # does, message included.
        failed = Client().post("/api/auth/login", {"email": email, "password": PASSWORD})
        self.assertStatus(failed, 401)
        self.assertEqual(failed.json, LOGIN_FAILURE_BODY)

        # Same account, not a new one, and all its data is still there.
        me = google.get("/api/auth/me")
        self.assertStatus(me, 200)
        self.assertEqual(me.json["id"], account_id)

        # Still signs in with Google afterwards.
        again = GClient()
        sign_in(again, fresh_sub(), email)
        self.assertEqual(again.get("/api/auth/me").json["id"], account_id)

        # And the seat is genuinely theirs now.
        splits = again.get("/api/splits")
        self.assertStatus(splits, 200)
        self.assertIn(split["id"], [s["id"] for s in splits.json])

    def test_password_login_against_a_google_only_account_is_the_same_401(self):
        google_only = fresh_email("googleonly")
        sign_in(GClient(), fresh_sub(), google_only)

        # A registered account, wrong password — the reference failure.
        other = Client()
        other.register(password=PASSWORD)
        wrong = Client().post("/api/auth/login",
                              {"email": other.user["email"], "password": "not-the-password"})
        # A Google-only account, any password.
        passwordless = Client().post("/api/auth/login",
                                     {"email": google_only, "password": PASSWORD})
        # An address that does not exist at all.
        unknown = Client().post("/api/auth/login",
                                {"email": fresh_email("nobody"), "password": PASSWORD})

        self.assertEqual(passwordless.status, wrong.status)
        self.assertEqual(passwordless.json, wrong.json)
        self.assertEqual(passwordless.json, LOGIN_FAILURE_BODY)
        self.assertEqual(unknown.json, LOGIN_FAILURE_BODY)
        self.assertNotIn("Set-Cookie", passwordless.headers)


class TestSeatMatching(ApiTestCase, unittest.TestCase):

    def test_a_seat_with_a_matching_email_links_on_sign_in(self):
        email = fresh_email("seat")
        owner = new_user(display_name="Owner")
        split = owner.make_split(name="Dinner")
        owner.make_member(split["id"], name="Sam", email=email)

        google = GClient()
        r = sign_in(google, fresh_sub(), email, name="Sam")
        self.assertIn("seats=1", r.headers["Location"])

        splits = google.get("/api/splits")
        self.assertStatus(splits, 200)
        self.assertIn(split["id"], [s["id"] for s in splits.json],
                      "the matched seat must make the split visible")

    def test_a_seat_with_no_email_does_not_link(self):
        owner = new_user(display_name="Owner")
        split = owner.make_split(name="Anonymous seat")
        owner.make_member(split["id"], name="Sam")  # no address

        google = GClient()
        r = sign_in(google, fresh_sub(), fresh_email("seatless"))
        self.assertIn("seats=0", r.headers["Location"])
        self.assertNotIn(split["id"], [s["id"] for s in google.get("/api/splits").json])

    def test_a_seat_matching_an_unverified_account_does_not_link(self):
        # Registration never verified anything, so a password account's address
        # is an unproven claim and must not attract seats.
        email = fresh_email("unverified")
        holder = Client()
        holder.register(email=email, password=PASSWORD)

        owner = new_user(display_name="Owner")
        split = owner.make_split(name="Unverified seat")
        owner.make_member(split["id"], name="Sam", email=email)

        holder.login(email, PASSWORD)
        splits = holder.get("/api/splits")
        self.assertStatus(splits, 200)
        self.assertNotIn(split["id"], [s["id"] for s in splits.json],
                         "an unverified address must not inherit a seat")

    def test_a_user_who_already_holds_a_seat_gets_no_second_one(self):
        email = fresh_email("twice")
        owner = new_user(display_name="Owner")
        split = owner.make_split(name="One seat each")
        owner.make_member(split["id"], name="Sam", email=email)

        sub = fresh_sub()
        google = GClient()
        first = sign_in(google, sub, email)
        self.assertIn("seats=1", first.headers["Location"])

        # A second seat carrying the same address in the same split.
        owner.make_member(split["id"], name="Sam again", email=email)
        second = sign_in(GClient(), sub, email)
        self.assertIn("seats=0", second.headers["Location"],
                      "one account holds at most one seat in a split")

    def test_case_differences_in_the_address_still_match(self):
        email = fresh_email("case")
        owner = new_user(display_name="Owner")
        split = owner.make_split(name="Shouty")
        owner.make_member(split["id"], name="Sam", email=email.upper())

        r = sign_in(GClient(), fresh_sub(), email)
        self.assertIn("seats=1", r.headers["Location"])


class TestUnverifiedGoogleAddress(ApiTestCase, unittest.TestCase):
    """email_verified: false is not a verified address, and never links a seat."""

    def test_no_seats_are_linked_for_an_unverified_google_address(self):
        email = fresh_email("unverifiedgoogle")
        owner = new_user(display_name="Owner")
        split = owner.make_split(name="Unverified google")
        owner.make_member(split["id"], name="Sam", email=email)

        google = GClient()
        r = sign_in(google, fresh_sub(), email, email_verified=False)
        self.assertStatus(r, 302)
        self.assertIn("seats=0", r.headers["Location"])

        me = google.get("/api/auth/me")
        self.assertStatus(me, 200, "the account is still created and signed in: ")
        self.assertNotIn(split["id"], [s["id"] for s in google.get("/api/splits").json])

    def test_later_verification_of_the_same_subject_links_the_seat(self):
        email = fresh_email("laterverified")
        owner = new_user(display_name="Owner")
        split = owner.make_split(name="Later")
        owner.make_member(split["id"], name="Sam", email=email)

        sub = fresh_sub()
        unverified = sign_in(GClient(), sub, email, email_verified=False)
        self.assertIn("seats=0", unverified.headers["Location"])

        verified = sign_in(GClient(), sub, email, email_verified=True)
        self.assertIn("seats=1", verified.headers["Location"])

    def test_an_unverified_address_cannot_adopt_an_existing_account(self):
        email = fresh_email("adopt")
        victim = Client()
        victim.register(email=email, password=PASSWORD)

        google = GClient()
        r = sign_in(google, fresh_sub(), email, email_verified=False)
        self.assertError(r, 409)
        self.assertNotIn("session", google.cookies,
                         "no session may be created for a refused link")

        # The victim's password still works: nothing was cleared.
        victim.login(email, PASSWORD)


class TestState(ApiTestCase, unittest.TestCase):

    def assertNoSession(self, response, client):
        self.assertNotIn("session", client.cookies)
        for raw in response.set_cookies:
            self.assertFalse(raw.startswith("session="),
                             f"a rejected callback must not set a session: {raw}")

    def test_a_missing_state_is_rejected(self):
        c = GClient()
        start(c)
        code = fake_code(fresh_sub(), fresh_email())
        r = c.get(f"/api/auth/google/callback?code={code}")
        self.assertError(r, 400)
        self.assertNoSession(r, c)

    def test_a_mismatched_state_is_rejected(self):
        c = GClient()
        start(c)
        code = fake_code(fresh_sub(), fresh_email())
        r = c.get(f"/api/auth/google/callback?code={code}&state={'0' * 32}")
        self.assertError(r, 400)
        self.assertNoSession(r, c)

    def test_a_callback_with_no_state_cookie_at_all_is_rejected(self):
        # i.e. an unsolicited callback: someone else's link, or CSRF.
        started = start(GClient())
        victim = GClient()  # never called /start, so holds no cookie
        code = fake_code(fresh_sub(), fresh_email())
        r = victim.get(f"/api/auth/google/callback?code={code}&state={state_of(started)}")
        self.assertError(r, 400)
        self.assertNoSession(r, victim)

    def test_a_state_cannot_be_reused(self):
        c = GClient()
        started = start(c)
        state = state_of(started)
        code = fake_code(fresh_sub(), fresh_email())

        first = c.get(f"/api/auth/google/callback?code={code}&state={state}")
        self.assertStatus(first, 302)

        replay = c.get(f"/api/auth/google/callback?code={code}&state={state}")
        self.assertError(replay, 400)

    def test_a_self_consistent_cookie_and_state_pair_is_accepted(self):
        c = GClient()
        c.cookies["g_state"] = base64.urlsafe_b64encode(
            json.dumps({"n": "f" * 32, "r": "/"}).encode()).decode().rstrip("=")
        code = fake_code(fresh_sub(), fresh_email())
        # The attacker knows the nonce they wrote, so this is the strongest
        # forgery available — and it works, which is exactly why the cookie is
        # HttpOnly and why matching state proves only that the two legs belong
        # together. What must NOT work is a callback with no cookie (above).
        r = c.get(f"/api/auth/google/callback?code={code}&state={'f' * 32}")
        self.assertStatus(r, 302)

    def test_a_garbage_state_cookie_is_rejected_not_a_500(self):
        c = GClient()
        c.cookies["g_state"] = "!!!not-base64url!!!"
        code = fake_code(fresh_sub(), fresh_email())
        r = c.get(f"/api/auth/google/callback?code={code}&state=abc")
        self.assertError(r, 400)
        self.assertNoSession(r, c)


class TestCallbackFailures(ApiTestCase, unittest.TestCase):

    def test_a_missing_code_is_a_400(self):
        c = GClient()
        started = start(c)
        r = c.get(f"/api/auth/google/callback?state={state_of(started)}")
        self.assertError(r, 400)

    def test_an_undecodable_code_is_a_502_not_a_500(self):
        c = GClient()
        started = start(c)
        r = c.get(f"/api/auth/google/callback?code=%2A%2A%2A&state={state_of(started)}")
        self.assertError(r, 502)

    def test_a_code_describing_no_subject_is_a_502(self):
        c = GClient()
        started = start(c)
        code = base64.urlsafe_b64encode(
            json.dumps({"email": fresh_email()}).encode()).decode().rstrip("=")
        r = c.get(f"/api/auth/google/callback?code={code}&state={state_of(started)}")
        self.assertError(r, 502)

    def test_google_reporting_an_error_does_not_sign_anyone_in(self):
        c = GClient()
        started = start(c)
        r = c.get("/api/auth/google/callback"
                  f"?error=access_denied&state={state_of(started)}")
        self.assertError(r, 401)
        self.assertNotIn("session", c.cookies)


if __name__ == "__main__":
    unittest.main()
