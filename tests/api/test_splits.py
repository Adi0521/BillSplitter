"""Phase 2 — Splits.

Asserted against docs/api.md. Two contract points get more attention than the
rest because they are the ones a plausible-looking implementation gets wrong:

  * `description` is `""` and never null; `archived_at` is null and never `""`.
    The column is nullable in one case and not the other, and the API deliberately
    does not expose that symmetry — a client that has to handle both null and ""
    for the same field will eventually handle only one.
  * Another user's split is a **404, not a 403**. A 403 confirms the id exists,
    which is precisely the fact the 404 is there to withhold.
"""
import unittest
import uuid
from urllib.parse import quote

from harness import BAD_IDS, MISSING_UUID, ApiTestCase, Client, new_user


def as_path_id(value):
    """BAD_IDS holds things like `', 'x` that are not legal in a URL path;
    percent-encoding them keeps the *server* as the thing under test rather
    than urllib's request builder."""
    return quote(value, safe="")


SPLIT_KEYS = {"id", "name", "description", "type", "currency", "share_token",
              "created_at", "archived_at", "member_count"}


class TestCreateSplit(ApiTestCase, unittest.TestCase):

    def test_create_returns_201_and_every_documented_field(self):
        c = new_user()
        r = c.post("/api/splits", {"name": "Tahoe trip", "type": "one_time"})
        self.assertStatus(r, 201)
        self.assertTrue(SPLIT_KEYS.issubset(r.json.keys()),
                        f"missing {SPLIT_KEYS - set(r.json.keys())}")
        self.assertEqual(r.json["name"], "Tahoe trip")
        self.assertEqual(r.json["type"], "one_time")
        self.assertTrue(r.json["share_token"], "share_token must be populated")
        self.assertTrue(r.json["created_at"])

    def test_omitted_description_is_the_empty_string_never_null(self):
        c = new_user()
        created = c.post("/api/splits", {"name": "No description", "type": "one_time"}).json
        self.assertEqual(created["description"], "")
        detail = c.get(f"/api/splits/{created['id']}").json
        self.assertEqual(detail["description"], "")
        listed = [s for s in c.get("/api/splits").json if s["id"] == created["id"]][0]
        self.assertEqual(listed["description"], "")

    def test_explicit_null_description_reads_back_as_the_empty_string(self):
        c = new_user()
        r = c.post("/api/splits",
                   {"name": "Null description", "type": "one_time", "description": None})
        self.assertStatus(r, 201)
        self.assertEqual(r.json["description"], "")

    def test_a_new_split_is_not_archived_so_archived_at_is_json_null(self):
        c = new_user()
        created = c.make_split(name="Live split")
        self.assertIsNone(created["archived_at"],
                          "archived_at must be null, not \"\", on a live split")

    def test_member_count_is_one_on_creation_and_tracks_added_members(self):
        c = new_user()
        created = c.make_split()
        self.assertEqual(created["member_count"], 1,
                         "the owner is auto-added, so a new split has one member")
        c.make_member(created["id"], name="Bob")
        c.make_member(created["id"], name="Carol")
        self.assertEqual(c.get(f"/api/splits/{created['id']}").json["member_count"], 3)
        listed = [s for s in c.get("/api/splits").json if s["id"] == created["id"]][0]
        self.assertEqual(listed["member_count"], 3,
                         "member_count is documented as present on the list too")

    def test_creating_a_split_auto_adds_the_owner_named_from_display_name(self):
        c = new_user(display_name="Alice Anderson")
        split = c.make_split()
        members = c.get(f"/api/splits/{split['id']}/members").json
        self.assertEqual(len(members), 1)
        self.assertEqual(members[0]["name"], "Alice Anderson")
        self.assertEqual(members[0]["user_id"], c.user["id"],
                         "the owner-member must be linked to the owner's user row")

    def test_the_owner_member_falls_back_to_the_email_local_part(self):
        local_part = f"fallback-{uuid.uuid4().hex[:10]}"
        c = Client()
        c.register(email=f"{local_part}@example.com", display_name="")
        split = c.make_split()
        members = c.get(f"/api/splits/{split['id']}/members").json
        self.assertEqual(len(members), 1)
        self.assertEqual(members[0]["name"], local_part,
                         "with no display_name the owner-member is named after "
                         "the part of the email before the @")

    def test_currency_defaults_to_usd_when_omitted(self):
        c = new_user()
        self.assertEqual(c.make_split()["currency"], "USD")

    def test_currency_is_normalized_to_uppercase(self):
        c = new_user()
        self.assertEqual(c.make_split(currency="eur")["currency"], "EUR")
        self.assertEqual(c.make_split(currency="gBp")["currency"], "GBP")

    def test_a_currency_that_is_not_three_ascii_letters_is_400(self):
        c = new_user()
        for bad in ("US", "USDD", "US1", "u$d", "12345", "$$$", " US D", "", "  "):
            with self.subTest(currency=bad):
                r = c.post("/api/splits",
                           {"name": "Bad currency", "type": "one_time", "currency": bad})
                if bad == "":
                    # An empty string is the "unset" a blank <select> submits;
                    # docs/api.md settles it as meaning "inherit the default".
                    self.assertStatus(r, 201)
                    self.assertEqual(r.json["currency"], "USD")
                else:
                    self.assertError(r, 400)


class TestCreateValidation(ApiTestCase, unittest.TestCase):

    def test_a_blank_or_whitespace_only_name_is_400(self):
        c = new_user()
        for name in ("", "   ", "\t\n"):
            with self.subTest(name=repr(name)):
                self.assertError(c.post("/api/splits", {"name": name, "type": "one_time"}), 400)

    def test_a_missing_name_is_400(self):
        c = new_user()
        self.assertError(c.post("/api/splits", {"type": "one_time"}), 400)

    def test_a_200_character_name_is_accepted_and_201_is_rejected(self):
        c = new_user()
        self.assertStatus(c.post("/api/splits", {"name": "n" * 200, "type": "one_time"}), 201)
        self.assertError(c.post("/api/splits", {"name": "n" * 201, "type": "one_time"}), 400)

    def test_the_name_is_stored_trimmed(self):
        c = new_user()
        r = c.post("/api/splits", {"name": "  Padded  ", "type": "one_time"})
        self.assertStatus(r, 201)
        self.assertEqual(r.json["name"], "Padded")

    def test_a_missing_or_unknown_type_is_400(self):
        c = new_user()
        self.assertError(c.post("/api/splits", {"name": "No type"}), 400)
        for bad in ("weekly", "ONE_TIME", "one time", ""):
            with self.subTest(type=bad):
                self.assertError(c.post("/api/splits", {"name": "Bad type", "type": bad}), 400)

    def test_both_documented_types_are_accepted(self):
        c = new_user()
        for good in ("one_time", "ongoing"):
            with self.subTest(type=good):
                self.assertEqual(c.make_split(type=good)["type"], good)

    def test_a_2000_character_description_is_accepted_and_2001_is_rejected(self):
        c = new_user()
        ok = c.post("/api/splits",
                    {"name": "Long description", "type": "one_time", "description": "d" * 2000})
        self.assertStatus(ok, 201)
        self.assertEqual(len(ok.json["description"]), 2000)
        self.assertError(c.post("/api/splits",
                                {"name": "Too long", "type": "one_time",
                                 "description": "d" * 2001}), 400)

    def test_a_non_string_field_is_400_not_500(self):
        c = new_user()
        for body in ({"name": 42, "type": "one_time"},
                     {"name": "Ok", "type": 7},
                     {"name": "Ok", "type": "one_time", "currency": 840},
                     {"name": "Ok", "type": "one_time", "description": []}):
            with self.subTest(body=body):
                self.assertError(c.post("/api/splits", body), 400)

    def test_subtotal_and_total_are_not_split_fields(self):
        """A Split has no money on it at all — subtotal and total belong to
        bills, derived from their items. Whatever the handler does with them,
        it must never echo them back or store them, because a client that saw
        one would be reading a number the server never computed.

        What actually happens today: the create handler reads only the fields
        it knows about, so both keys are silently ignored and the response is a
        201 carrying neither. That is accepted here — but note the asymmetry
        with bills, where docs/api.md requires a supplied `subtotal` or `total`
        to be a hard 400."""
        c = new_user()
        r = c.post("/api/splits",
                   {"name": "Money keys", "type": "one_time",
                    "subtotal": "999.0000", "total": "999.0000"})
        self.assertIn(r.status, (201, 400),
                      f"expected the extra keys to be ignored or rejected: {r}")
        if r.status == 201:
            self.assertNotIn("subtotal", r.json, "a Split must not carry subtotal")
            self.assertNotIn("total", r.json, "a Split must not carry total")
            detail = c.get(f"/api/splits/{r.json['id']}").json
            self.assertNotIn("subtotal", detail)
            self.assertNotIn("total", detail)


class TestReadSplits(ApiTestCase, unittest.TestCase):

    def test_the_list_is_a_bare_json_array_of_the_callers_own_splits(self):
        c = new_user()
        a = c.make_split(name="First")
        b = c.make_split(name="Second")
        r = c.get("/api/splits")
        self.assertStatus(r, 200)
        self.assertIsInstance(r.json, list, "list endpoints return a bare array")
        ids = [s["id"] for s in r.json]
        self.assertIn(a["id"], ids)
        self.assertIn(b["id"], ids)

    def test_the_list_orders_by_created_at_descending(self):
        c = new_user()
        first = c.make_split(name="Older")
        second = c.make_split(name="Newer")
        ids = [s["id"] for s in c.get("/api/splits").json]
        self.assertLess(ids.index(second["id"]), ids.index(first["id"]),
                        "newest split must come first")

    def test_another_users_splits_never_appear_in_the_list(self):
        owner, stranger = new_user(), new_user()
        owned = owner.make_split(name="Private")
        self.assertNotIn(owned["id"], [s["id"] for s in stranger.get("/api/splits").json])

    def test_detail_embeds_the_members_array(self):
        c = new_user(display_name="Alice")
        split = c.make_split()
        c.make_member(split["id"], name="Bob")
        r = c.get(f"/api/splits/{split['id']}")
        self.assertStatus(r, 200)
        self.assertTrue(SPLIT_KEYS.issubset(r.json.keys()))
        self.assertIsInstance(r.json["members"], list)
        self.assertEqual([m["name"] for m in r.json["members"]], ["Alice", "Bob"])

    def test_a_syntactically_valid_but_unknown_id_is_404(self):
        c = new_user()
        self.assertError(c.get(f"/api/splits/{MISSING_UUID}"), 404)


class TestUpdateSplit(ApiTestCase, unittest.TestCase):

    def test_put_is_a_partial_patch_that_leaves_omitted_fields_unchanged(self):
        c = new_user()
        split = c.post("/api/splits",
                       {"name": "Original", "type": "ongoing", "currency": "eur",
                        "description": "Keep me"}).json
        r = c.put(f"/api/splits/{split['id']}", {"name": "Renamed"})
        self.assertStatus(r, 200)
        self.assertEqual(r.json["name"], "Renamed")
        self.assertEqual(r.json["currency"], "EUR", "currency must be untouched")
        self.assertEqual(r.json["type"], "ongoing", "type must be untouched")
        self.assertEqual(r.json["description"], "Keep me", "description must be untouched")

    def test_put_can_change_each_field_on_its_own(self):
        c = new_user()
        split = c.make_split(name="Patchable", type="one_time", currency="USD")
        self.assertEqual(c.put(f"/api/splits/{split['id']}", {"type": "ongoing"}).json["type"],
                         "ongoing")
        self.assertEqual(c.put(f"/api/splits/{split['id']}", {"currency": "jpy"}).json["currency"],
                         "JPY")
        self.assertEqual(
            c.put(f"/api/splits/{split['id']}", {"description": "Now described"}).json["description"],
            "Now described")

    def test_put_clearing_the_description_yields_the_empty_string_not_null(self):
        c = new_user()
        split = c.post("/api/splits",
                       {"name": "Clearable", "type": "one_time", "description": "Text"}).json
        r = c.put(f"/api/splits/{split['id']}", {"description": ""})
        self.assertStatus(r, 200)
        self.assertEqual(r.json["description"], "")
        self.assertEqual(c.get(f"/api/splits/{split['id']}").json["description"], "")

    def test_an_empty_body_is_400_not_a_silent_no_op(self):
        c = new_user()
        split = c.make_split()
        self.assertError(c.put(f"/api/splits/{split['id']}", {}), 400)

    def test_put_applies_the_same_validation_as_create(self):
        c = new_user()
        split = c.make_split(name="Validated")
        for body in ({"name": ""}, {"name": "   "}, {"name": "n" * 201},
                     {"type": "weekly"}, {"currency": "US1"}, {"currency": "u$d"},
                     {"currency": "US"}, {"currency": "USDD"},
                     {"description": "d" * 2001}):
            with self.subTest(body=body):
                self.assertError(c.put(f"/api/splits/{split['id']}", body), 400)
        # ...and the split is unchanged by any of the rejected calls.
        self.assertEqual(c.get(f"/api/splits/{split['id']}").json["name"], "Validated")

    def test_put_does_not_change_member_count_or_share_token(self):
        c = new_user()
        split = c.make_split(name="Stable")
        r = c.put(f"/api/splits/{split['id']}", {"name": "Still stable"})
        self.assertEqual(r.json["share_token"], split["share_token"])
        self.assertEqual(r.json["member_count"], split["member_count"])


class TestArchiveSplit(ApiTestCase, unittest.TestCase):

    def test_delete_archives_rather_than_deleting(self):
        c = new_user()
        split = c.make_split(name="To archive")
        r = c.delete(f"/api/splits/{split['id']}")
        self.assertStatus(r, 200)
        self.assertEqual(r.json, {"message": "Archived"})

    def test_an_archived_split_leaves_the_default_list_and_returns_with_the_flag(self):
        c = new_user()
        split = c.make_split(name="Archivable")
        c.delete(f"/api/splits/{split['id']}")

        default_ids = [s["id"] for s in c.get("/api/splits").json]
        self.assertNotIn(split["id"], default_ids,
                         "the default list excludes archived splits")

        archived = c.get("/api/splits?archived=true").json
        by_id = {s["id"]: s for s in archived}
        self.assertIn(split["id"], by_id, "?archived=true includes archived splits")
        self.assertIsNotNone(by_id[split["id"]]["archived_at"],
                             "archived_at must be a timestamp string once archived")
        self.assertIsInstance(by_id[split["id"]]["archived_at"], str)

    def test_archiving_is_idempotent_and_keeps_the_first_timestamp(self):
        c = new_user()
        split = c.make_split(name="Twice archived")
        self.assertStatus(c.delete(f"/api/splits/{split['id']}"), 200)

        def archived_at():
            found = [s for s in c.get("/api/splits?archived=true").json
                     if s["id"] == split["id"]]
            return found[0]["archived_at"]

        first = archived_at()
        second_call = c.delete(f"/api/splits/{split['id']}")
        self.assertStatus(second_call, 200, "archiving twice is a no-op, still 200: ")
        self.assertEqual(archived_at(), first,
                         "a repeat archive must not move the archival timestamp")

    def test_an_unarchived_split_still_appears_when_archived_is_true(self):
        c = new_user()
        split = c.make_split(name="Live and listed")
        ids = [s["id"] for s in c.get("/api/splits?archived=true").json]
        self.assertIn(split["id"], ids, "?archived=true includes, not replaces")

    def test_archiving_does_not_destroy_the_rows(self):
        c = new_user()
        split = c.make_split(name="Rows survive")
        c.make_member(split["id"], name="Bob")
        c.delete(f"/api/splits/{split['id']}")
        found = [s for s in c.get("/api/splits?archived=true").json
                 if s["id"] == split["id"]]
        self.assertEqual(found[0]["member_count"], 2,
                         "archiving must not cascade to members")


class TestSplitOwnership(ApiTestCase, unittest.TestCase):
    """Another user's split is 404 — never 403 (which would confirm the id
    exists) and never 200."""

    def setUp(self):
        self.owner = new_user()
        self.stranger = new_user()
        self.split = self.owner.make_split(name="Owner's split", currency="EUR")

    def test_a_stranger_gets_404_on_get(self):
        r = self.stranger.get(f"/api/splits/{self.split['id']}")
        self.assertError(r, 404)

    def test_a_stranger_gets_404_on_put(self):
        r = self.stranger.put(f"/api/splits/{self.split['id']}", {"name": "Hijacked"})
        self.assertError(r, 404)
        self.assertEqual(self.owner.get(f"/api/splits/{self.split['id']}").json["name"],
                         "Owner's split", "a rejected PUT must not have written")

    def test_a_stranger_gets_404_on_delete_and_the_split_stays_live(self):
        r = self.stranger.delete(f"/api/splits/{self.split['id']}")
        self.assertError(r, 404)
        self.assertIsNone(self.owner.get(f"/api/splits/{self.split['id']}").json["archived_at"],
                          "a rejected DELETE must not have archived anything")

    def test_a_stranger_gets_404_on_the_members_of_an_archived_split_too(self):
        self.owner.delete(f"/api/splits/{self.split['id']}")
        self.assertError(self.stranger.get(f"/api/splits/{self.split['id']}"), 404)

    def test_unauthenticated_split_requests_are_401(self):
        anon = Client()
        self.assertError(anon.get("/api/splits"), 401)
        self.assertError(anon.post("/api/splits", {"name": "x", "type": "one_time"}), 401)
        self.assertError(anon.get(f"/api/splits/{self.split['id']}"), 401)
        self.assertError(anon.put(f"/api/splits/{self.split['id']}", {"name": "x"}), 401)
        self.assertError(anon.delete(f"/api/splits/{self.split['id']}"), 401)


class TestMalformedSplitIds(ApiTestCase, unittest.TestCase):
    """A path parameter is raw user input on its way into a `::uuid` cast. A
    malformed one must be answered, not raised: 404 or 400, never a 500."""

    def test_no_bad_id_produces_a_500_on_any_split_route(self):
        c = new_user()
        # "" is deliberately not in this list: it produces `/api/splits/`, a
        # path no route matches at all, so what comes back is the framework's
        # own 404 page rather than anything this handler decided.
        for bad in BAD_IDS + [MISSING_UUID, " ", "0" * 36, MISSING_UUID + "x"]:
            path = f"/api/splits/{as_path_id(bad)}"
            for method, body in (("GET", None), ("PUT", {"name": "Renamed"}), ("DELETE", None)):
                with self.subTest(id=bad, method=method):
                    r = c.request(method, path, body)
                    self.assertIn(r.status, (400, 404),
                                  f"{method} {path} returned {r.status}: {r.body[:200]}")
                    self.assertError(r, r.status)

    def test_a_bad_id_is_404_before_the_body_is_even_considered(self):
        c = new_user()
        r = c.put(f"/api/splits/{as_path_id('not-a-uuid')}", {})
        self.assertIn(r.status, (400, 404))


if __name__ == "__main__":
    unittest.main()
