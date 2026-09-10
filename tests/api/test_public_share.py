"""The public share link: the only unauthenticated endpoint that returns user data.

    GET  /api/splits/share/:token            no auth at all
    POST /api/splits/:id/share/regenerate    owner only

For every other endpoint the interesting question is what it returns. Here the
interesting question is what it must NOT return (docs/api.md, "What the public
view must NOT contain"):

    no email addresses, no user_ids, no split_id or any other internal id that
    grants access elsewhere, and the token is never echoed back. Member ids
    appear only where the UI needs them to key rows.

So the central test in this file does not check a handful of known-bad keys.
It walks the entire serialized response and asserts that:

  * no string anywhere looks like an email address,
  * no key anywhere is one of the forbidden names,
  * no *value* anywhere equals a secret the fixture knows (the owner's email, a
    member's email, the owner's user id, the split id, a bill id, the token
    itself, or a payment's private notes),
  * every uuid-shaped string anywhere is a member id of this split.

That last assertion is the one that survives the future. A migration that adds
`bills.receipt_url` and a careless `SELECT *` would slip past a list of banned
keys; a new id column would not slip past "every uuid here must be a member id".
"""
import re
import unittest

from harness import ApiTestCase, BAD_IDS, MISSING_UUID, Client, new_user

# BAD_IDS contains a space, which http.client refuses to put in a request line.
def path_id(raw):
    return raw.replace(" ", "%20")


TOKEN_RE = re.compile(r"^[0-9a-f]{32}$")
UUID_RE = re.compile(
    r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b"
)
EMAIL_RE = re.compile(r"[^\s@]+@[^\s@]+\.[^\s@]+")

# Keys that must never appear at any depth of the public payload.
FORBIDDEN_KEYS = {
    "email", "user_id", "owner_id", "split_id", "share_token", "token",
    "session", "password", "password_hash", "notes", "bill_id", "bill_item_id",
    "payer_member_id", "archived_at",
}

MONEY_KEYS = {
    "total", "owes", "fronted", "payments_made", "payments_received",
    "balance", "items", "tax", "tip", "fees", "amount",
}


def walk(node, path="$"):
    """Yields (path, key, value) for every node in a JSON document.

    `key` is None for list elements and for the root.
    """
    yield path, None, node
    if isinstance(node, dict):
        for k, v in node.items():
            yield from walk(v, f"{path}.{k}")
            yield f"{path}.{k}", k, v
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from walk(v, f"{path}[{i}]")


def all_strings(node):
    for path, _key, value in walk(node):
        if isinstance(value, str):
            yield path, value


def all_keys(node):
    for path, key, _value in walk(node):
        if key is not None:
            yield path, key


class PublicShareCase(ApiTestCase, unittest.TestCase):
    """Fixtures and assertions shared by the suites below."""

    def public(self, token, expect=200):
        """Fetches the share view with a client that has never authenticated.

        A fresh Client per call is the point: this endpoint must work with no
        cookie, no Authorization header and no account at all.
        """
        r = Client().get(f"/api/splits/share/{token}")
        self.assertStatus(r, expect)
        return r

    def build_split(self, owner, currency="USD"):
        """A split with two members, a bill with items and allocations, and a
        second bill in another currency. Returns a dict of everything the
        leak test needs to know the secrets of."""
        split = owner.make_split(currency=currency)
        members = owner.get(f"/api/splits/{split['id']}/members").json
        alice = members[0]                       # the owner's own member row
        bob = owner.make_member(split["id"], "Bob", email="bob-private@example.com")

        bill = owner.make_bill(
            split["id"], store_name="Safeway", date="2026-08-30",
            tax="3.83", payer_member_id=alice["id"],
        )
        oil = owner.make_item(bill["id"], "Olive oil", "12.50", quantity=2)
        bread = owner.make_item(bill["id"], "Bread", "36.49")
        self.assertStatus(
            owner.allocate(bill["id"], oil["id"], "ratio",
                           [{"member_id": alice["id"], "ratio": "100"}]), 200)
        self.assertStatus(
            owner.allocate(bill["id"], bread["id"], "ratio",
                           [{"member_id": bob["id"], "ratio": "100"}]), 200)

        return {
            "split": split,
            "alice": alice,
            "bob": bob,
            "bill": bill,
            "items": [oil, bread],
        }

    def add_payment(self, owner, fx, amount="20.0000", notes="private note"):
        """Records a settlement. Returns the payment, or None when the payments
        endpoint is not available (it is a separate slice); payment-specific
        assertions are skipped in that case rather than failing this suite for
        someone else's module."""
        r = owner.post(
            f"/api/splits/{fx['split']['id']}/payments",
            {"from_member": fx["bob"]["id"], "to_member": fx["alice"]["id"],
             "amount": amount, "method": "venmo", "notes": notes},
        )
        return r.json if r.status == 201 else None


class PublicViewOmissionsTest(PublicShareCase):
    """The omissions are the specification."""

    def test_public_view_leaks_nothing(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        payment = self.add_payment(owner, fx, notes="paid at the airport bar")

        token = fx["split"]["share_token"]
        r = self.public(token)
        body, doc = r.body, r.json

        member_ids = {m["id"] for m in
                      owner.get(f"/api/splits/{fx['split']['id']}/members").json}

        # ── 1. nothing that looks like an email address, anywhere ────────────
        for path, value in all_strings(doc):
            self.assertIsNone(
                EMAIL_RE.search(value),
                f"{path} contains an email address: {value!r}",
            )

        # ── 2. no forbidden key, at any depth ────────────────────────────────
        for path, key in all_keys(doc):
            self.assertNotIn(
                key, FORBIDDEN_KEYS,
                f"{path} is a field the public view must not contain",
            )

        # ── 3. no known secret value, anywhere in the raw body ───────────────
        # Checked against the raw text rather than the parsed document so a
        # secret smuggled inside a longer string is caught too.
        secrets = {
            "owner's email": owner.user["email"],
            "member's email": "bob-private@example.com",
            "owner's user id": owner.user["id"],
            "split id": fx["split"]["id"],
            "bill id": fx["bill"]["id"],
            "bill item id": fx["items"][0]["id"],
            "the share token itself": token,
        }
        if payment:
            secrets["payment id"] = payment["id"]
            secrets["payment notes"] = "paid at the airport bar"
        for label, secret in secrets.items():
            self.assertNotIn(secret, body, f"public view leaked the {label}")

        # ── 4. every uuid present is a member id of this split ───────────────
        # The catch-all: a column added by a later migration and serialized by
        # accident would show up here as an id that is not a member id, even
        # though its key name is not on any banned list.
        for path, value in all_strings(doc):
            for found in UUID_RE.findall(value):
                self.assertIn(
                    found, member_ids,
                    f"{path} contains a uuid that is not a member id: {found}",
                )

    def test_shape_is_the_documented_one(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        doc = self.public(fx["split"]["share_token"]).json

        self.assertEqual(
            set(doc),
            {"name", "description", "type", "base_currency", "mixed_currency",
             "created_at", "archived", "bills", "by_currency", "payments"},
        )
        self.assertEqual(doc["name"], fx["split"]["name"])
        self.assertEqual(doc["base_currency"], "USD")
        self.assertEqual(doc["type"], "one_time")
        self.assertIs(doc["mixed_currency"], False)
        self.assertIs(doc["archived"], False)
        # description is "" when null, never null — same rule as Split.
        self.assertEqual(doc["description"], "")

        self.assertEqual(len(doc["bills"]), 1)
        bill = doc["bills"][0]
        self.assertEqual(set(bill),
                         {"store_name", "date", "currency", "total",
                          "payer_name", "item_count"})
        self.assertEqual(bill["store_name"], "Safeway")
        self.assertEqual(bill["date"], "2026-08-30")
        self.assertEqual(bill["item_count"], 2)
        # The payer is named, not identified: no member id on a bill row.
        self.assertEqual(bill["payer_name"], "Alice")
        self.assertMoneyString(bill["total"], "bills[0].total")
        self.assertEqual(bill["total"], "65.3200")   # 25.00 + 36.49 + 3.83

    def test_every_money_value_is_a_four_decimal_string(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        doc = self.public(fx["split"]["share_token"]).json

        seen = 0
        for path, key, value in walk(doc):
            if key in MONEY_KEYS and not isinstance(value, (dict, list)):
                self.assertMoneyString(value, path)
                seen += 1
        self.assertGreater(seen, 0, "no money fields were checked")

    def test_no_authentication_of_any_kind_is_required(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        token = fx["split"]["share_token"]

        # A client that has never registered, never logged in, and holds no
        # cookie gets the same 200 the owner would.
        anonymous = Client()
        r = anonymous.get(f"/api/splits/share/{token}")
        self.assertStatus(r, 200)
        self.assertEqual(len(anonymous.jar), 0, "the public view set a cookie")

        # And a *different* user's session neither helps nor hurts.
        stranger = new_user()
        self.assertStatus(stranger.get(f"/api/splits/share/{token}"), 200)


class PublicViewResolutionTest(PublicShareCase):
    """Which tokens resolve, and what happens to the ones that do not."""

    def test_unknown_token_is_404(self):
        r = self.public("0" * 32, expect=404)
        self.assertError(r, 404)

    def test_malformed_token_is_404(self):
        # Every one of these is the wrong shape for a token. None may produce a
        # 500, and none may say anything different from the unknown-token case.
        malformed = [
            "abc",                    # too short
            "deadbeef",               # hex, but 8 characters
            "f" * 31,                 # one character short
            "f" * 33,                 # one character long
            "F" * 32,                 # uppercase: tokens are lowercase hex
            "g" * 32,                 # right length, not hex
            MISSING_UUID,             # a uuid is not a token
        ] + BAD_IDS

        unknown_message = self.public("0" * 32, expect=404).json["error"]

        for raw in malformed:
            with self.subTest(token=raw):
                r = Client().get(f"/api/splits/share/{path_id(raw)}")
                self.assertError(r, 404)
                # "never existed" and "malformed" must be indistinguishable, or
                # the endpoint becomes an oracle for which links were once real.
                self.assertEqual(r.json["error"], unknown_message)

    def test_empty_token_path_does_not_reach_the_split_detail_route(self):
        # /api/splits/share with no token is the split-detail route with an id
        # of "share"; unauthenticated that is a 401, and it must never be a 500.
        r = Client().get("/api/splits/share")
        self.assertIn(r.status, (401, 404), f"unexpected: {r}")

        # /api/splits/share/ with an empty token matches no route at all, so it
        # is the app's catch-all 404 rather than the share route's. Different
        # message, same status, and still the {"error": ...} envelope: it says
        # nothing about tokens because it never looked at one.
        self.assertError(Client().get("/api/splits/share/"), 404)

    def test_archived_split_still_resolves(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        token = fx["split"]["share_token"]

        self.assertStatus(owner.delete(f"/api/splits/{fx['split']['id']}"), 200)

        # A link shared before archiving must not break; it just says so.
        doc = self.public(token).json
        self.assertIs(doc["archived"], True)
        self.assertEqual(doc["name"], fx["split"]["name"])
        self.assertEqual(len(doc["bills"]), 1)
        # ...and archiving does not start leaking the timestamp either.
        self.assertNotIn("archived_at", doc)


class RouteCollisionTest(PublicShareCase):
    """/api/splits/share/:token and /api/splits/:id must not swallow each other."""

    def test_share_path_is_not_matched_by_the_split_detail_route(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        token = fx["split"]["share_token"]

        # If /api/splits/<string> matched the share path, this would be a 401
        # for an anonymous caller instead of the public view.
        self.assertStatus(Client().get(f"/api/splits/share/{token}"), 200)

    def test_a_token_in_the_split_id_position_is_not_a_split(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        token = fx["split"]["share_token"]

        # The token is not a uuid, so the detail route must 404 it — and it must
        # certainly not serve the split just because the string is a valid token.
        r = owner.get(f"/api/splits/{token}")
        self.assertError(r, 404)

    def test_split_detail_route_still_works(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)

        r = owner.get(f"/api/splits/{fx['split']['id']}")
        self.assertStatus(r, 200)
        self.assertEqual(r.json["id"], fx["split"]["id"])


class RegenerateTest(PublicShareCase):
    """POST /api/splits/:id/share/regenerate — owner only, immediate."""

    def regenerate(self, client, split_id):
        return client.post(f"/api/splits/{split_id}/share/regenerate")

    def test_regenerate_returns_a_new_token_and_kills_the_old_one(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        old = fx["split"]["share_token"]
        self.assertStatus(self.public(old), 200)

        r = self.regenerate(owner, fx["split"]["id"])
        self.assertStatus(r, 200)
        self.assertEqual(set(r.json), {"share_token"},
                         "regenerate must return only the new token")
        new = r.json["share_token"]
        self.assertRegex(new, TOKEN_RE, "token must be 32 lowercase hex chars")
        self.assertNotEqual(new, old)

        # Immediately, with no grace period and no second valid token.
        self.assertError(self.public(old, expect=404), 404)
        self.assertStatus(self.public(new), 200)

        # The split itself is unchanged, and reports the new token to its owner.
        self.assertEqual(
            owner.get(f"/api/splits/{fx['split']['id']}").json["share_token"], new)

    def test_regenerate_twice_invalidates_the_intermediate_token(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)

        first = self.regenerate(owner, fx["split"]["id"]).json["share_token"]
        second = self.regenerate(owner, fx["split"]["id"]).json["share_token"]
        self.assertNotEqual(first, second)
        self.assertError(self.public(first, expect=404), 404)
        self.assertStatus(self.public(second), 200)

    def test_non_owner_cannot_regenerate(self):
        owner = new_user(display_name="Alice")
        stranger = new_user(display_name="Mallory")
        fx = self.build_split(owner)
        token = fx["split"]["share_token"]

        # 404, not 403: the API does not confirm that the split exists.
        r = self.regenerate(stranger, fx["split"]["id"])
        self.assertError(r, 404)

        # And the owner's link is untouched by the attempt.
        self.assertStatus(self.public(token), 200)
        self.assertEqual(
            owner.get(f"/api/splits/{fx['split']['id']}").json["share_token"], token)

    def test_regenerate_requires_a_session(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)

        r = Client().post(f"/api/splits/{fx['split']['id']}/share/regenerate")
        self.assertError(r, 401)
        self.assertStatus(self.public(fx["split"]["share_token"]), 200)

    def test_regenerate_on_a_missing_or_malformed_split_id(self):
        owner = new_user(display_name="Alice")
        self.assertError(self.regenerate(owner, MISSING_UUID), 404)
        for raw in BAD_IDS:
            with self.subTest(split_id=raw):
                self.assertError(self.regenerate(owner, path_id(raw)), 404)

    def test_regenerate_works_on_an_archived_split(self):
        # Revoking a link that was shared before archiving is exactly when an
        # owner reaches for this.
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        old = fx["split"]["share_token"]
        self.assertStatus(owner.delete(f"/api/splits/{fx['split']['id']}"), 200)

        r = self.regenerate(owner, fx["split"]["id"])
        self.assertStatus(r, 200)
        self.assertError(self.public(old, expect=404), 404)
        self.assertIs(self.public(r.json["share_token"]).json["archived"], True)


class PublicSummaryTest(PublicShareCase):
    """by_currency follows the same rules as the split summary."""

    def test_balances_and_settlements(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        payment = self.add_payment(owner, fx, amount="20.00")
        doc = self.public(fx["split"]["share_token"]).json

        self.assertEqual(len(doc["by_currency"]), 1)
        usd = doc["by_currency"][0]
        self.assertEqual(set(usd),
                         {"currency", "bill_count", "total", "unallocated",
                          "members", "settlements"})
        self.assertEqual(usd["currency"], "USD")
        self.assertEqual(usd["bill_count"], 1)
        self.assertEqual(usd["total"], "65.3200")

        # Everything is allocated, so nothing is left unassigned.
        self.assertEqual(usd["unallocated"]["total"], "0.0000")

        by_name = {m["name"]: m for m in usd["members"]}
        self.assertEqual(set(by_name), {"Alice", "Bob"})
        self.assertEqual(set(by_name["Alice"]),
                         {"member_id", "name", "owes", "fronted",
                          "payments_made", "payments_received", "balance"})

        # Alice's items are 25.00 of a 61.49 subtotal, so her tax is
        # 25/61.49*3.83 = 1.5572; Bob carries the rest.
        self.assertEqual(by_name["Alice"]["owes"], "26.5572")
        self.assertEqual(by_name["Bob"]["owes"], "38.7628")
        self.assertEqual(by_name["Alice"]["fronted"], "65.3200")
        self.assertEqual(by_name["Bob"]["fronted"], "0.0000")

        if payment is None:
            self.skipTest("payments endpoint not available in this build")

        self.assertEqual(by_name["Bob"]["payments_made"], "20.0000")
        self.assertEqual(by_name["Alice"]["payments_received"], "20.0000")
        # balance = fronted + made - owes - received
        self.assertEqual(by_name["Alice"]["balance"], "18.7628")
        self.assertEqual(by_name["Bob"]["balance"], "-18.7628")

        self.assertEqual(len(usd["settlements"]), 1)
        s = usd["settlements"][0]
        self.assertEqual(s["from_name"], "Bob")
        self.assertEqual(s["to_name"], "Alice")
        # The transfer clears the balance exactly — no float, no lost hundredth
        # of a cent.
        self.assertEqual(s["amount"], "18.7628")
        self.assertEqual(s["from_member"], by_name["Bob"]["member_id"])
        self.assertEqual(s["to_member"], by_name["Alice"]["member_id"])

    def test_members_with_no_activity_still_appear_with_zeros(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        owner.make_member(fx["split"]["id"], "Cara")

        usd = self.public(fx["split"]["share_token"]).json["by_currency"][0]
        cara = [m for m in usd["members"] if m["name"] == "Cara"][0]
        for field in ("owes", "fronted", "payments_made",
                      "payments_received", "balance"):
            self.assertEqual(cara[field], "0.0000", field)

    def test_currencies_are_grouped_and_never_summed(self):
        owner = new_user(display_name="Alice")
        fx = self.build_split(owner)
        eur = owner.make_bill(fx["split"]["id"], store_name="Paris cafe",
                              date="2026-08-28", currency="EUR", tax="5.00")
        self.assertEqual(eur["currency"], "EUR")

        doc = self.public(fx["split"]["share_token"]).json
        self.assertIs(doc["mixed_currency"], True)
        self.assertEqual([c["currency"] for c in doc["by_currency"]],
                         ["EUR", "USD"])

        totals = {c["currency"]: c["total"] for c in doc["by_currency"]}
        self.assertEqual(totals["USD"], "65.3200")
        self.assertEqual(totals["EUR"], "5.0000")
        # 70.32 must appear nowhere: adding EUR to USD invents a number.
        self.assertNotIn("70.3200", self.public(
            fx["split"]["share_token"]).body)

    def test_zero_subtotal_puts_the_whole_of_tax_in_unallocated(self):
        # The division-by-zero that must not reach Postgres: a bill with tax and
        # no items has no proportion to divide, so every member's share is zero
        # and the whole amount is unallocated.
        owner = new_user(display_name="Alice")
        split = owner.make_split(currency="USD")
        owner.make_bill(split["id"], store_name="Corkage", date="2026-08-30",
                        tax="5.00", tip="1.00")

        block = self.public(split["share_token"]).json["by_currency"][0]
        self.assertEqual(block["unallocated"]["items"], "0.0000")
        self.assertEqual(block["unallocated"]["tax"], "5.0000")
        self.assertEqual(block["unallocated"]["tip"], "1.0000")
        self.assertEqual(block["unallocated"]["total"], "6.0000")
        for m in block["members"]:
            self.assertEqual(m["owes"], "0.0000")

    def test_split_with_no_bills_has_no_currency_blocks(self):
        owner = new_user(display_name="Alice")
        split = owner.make_split(currency="GBP")

        doc = self.public(split["share_token"]).json
        self.assertEqual(doc["bills"], [])
        # Not an error, and not a fabricated zero row for the split's currency.
        self.assertEqual(doc["by_currency"], [])
        self.assertIs(doc["mixed_currency"], False)
        self.assertEqual(doc["base_currency"], "GBP")


if __name__ == "__main__":
    unittest.main()
