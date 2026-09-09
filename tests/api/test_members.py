"""Phase 2 — Members.

Asserted against docs/api.md. The points that carry the most weight:

  * `user_id` and `email` are nullable columns and must serialize as JSON
    `null`, never as `""`. `""` and `null` mean different things ("a guest with
    a blank address" vs "a guest with no address") and a client that has to
    treat both as absent will one day treat only one of them as absent.
  * Duplicate names in a split are *allowed*. Two people really can both be
    Alex; the id is the identity, and a uniqueness check here would be a bug
    dressed up as validation.
  * A member id from a different split is a 404 on DELETE even when the caller
    owns both splits — the member simply is not in that collection.
"""
import unittest
from urllib.parse import quote

from harness import BAD_IDS, MISSING_UUID, ApiTestCase, Client, new_user


def as_path_id(value):
    """BAD_IDS holds things like `', 'x` that are not legal in a URL path;
    percent-encoding them keeps the server as the thing under test."""
    return quote(value, safe="")


MEMBER_KEYS = {"id", "split_id", "user_id", "name", "email", "joined_at"}


class TestAddMember(ApiTestCase, unittest.TestCase):

    def test_add_returns_201_and_every_documented_field(self):
        c = new_user()
        split = c.make_split()
        r = c.post(f"/api/splits/{split['id']}/members", {"name": "Bob"})
        self.assertStatus(r, 201)
        self.assertEqual(set(r.json.keys()), MEMBER_KEYS,
                         f"Member shape drifted: {sorted(r.json.keys())}")
        self.assertEqual(r.json["name"], "Bob")
        self.assertEqual(r.json["split_id"], split["id"])
        self.assertTrue(r.json["joined_at"])

    def test_a_guest_members_user_id_and_email_are_json_null_not_empty_string(self):
        c = new_user()
        split = c.make_split()
        member = c.make_member(split["id"], name="Guest")
        self.assertIsNone(member["user_id"], "user_id must be null for a guest")
        self.assertIsNone(member["email"], "email must be null when not provided")
        listed = c.get(f"/api/splits/{split['id']}/members").json
        fetched = [m for m in listed if m["id"] == member["id"]][0]
        self.assertIsNone(fetched["user_id"])
        self.assertIsNone(fetched["email"])

    def test_an_explicitly_blank_or_null_email_stores_null_not_empty_string(self):
        c = new_user()
        split = c.make_split()
        for value in ("", "   ", None):
            with self.subTest(email=repr(value)):
                r = c.post(f"/api/splits/{split['id']}/members",
                           {"name": "Blank email", "email": value})
                self.assertStatus(r, 201)
                self.assertIsNone(r.json["email"],
                                  'a blank email is "no email", i.e. null')

    def test_a_provided_email_is_stored_and_returned(self):
        c = new_user()
        split = c.make_split()
        member = c.make_member(split["id"], name="Bob", email="bob@example.com")
        self.assertEqual(member["email"], "bob@example.com")
        self.assertEqual(
            c.get(f"/api/splits/{split['id']}/members").json[-1]["email"],
            "bob@example.com")

    def test_the_owner_member_carries_a_user_id(self):
        """The auto-added owner is a registered user, so unlike a guest their
        user_id is populated — that is what distinguishes the two cases."""
        c = new_user(display_name="Alice")
        split = c.make_split()
        members = c.get(f"/api/splits/{split['id']}/members").json
        self.assertEqual(members[0]["user_id"], c.user["id"])

    def test_duplicate_names_within_one_split_are_allowed(self):
        c = new_user()
        split = c.make_split()
        first = c.make_member(split["id"], name="Alex")
        second = c.make_member(split["id"], name="Alex")
        self.assertNotEqual(first["id"], second["id"], "the id is the identity")
        names = [m["name"] for m in c.get(f"/api/splits/{split['id']}/members").json]
        self.assertEqual(names.count("Alex"), 2)

    def test_the_name_is_stored_trimmed(self):
        c = new_user()
        split = c.make_split()
        r = c.post(f"/api/splits/{split['id']}/members", {"name": "  Padded  "})
        self.assertStatus(r, 201)
        self.assertEqual(r.json["name"], "Padded")


class TestAddMemberValidation(ApiTestCase, unittest.TestCase):

    def setUp(self):
        self.c = new_user()
        self.split = self.c.make_split()

    def _post(self, body):
        return self.c.post(f"/api/splits/{self.split['id']}/members", body)

    def test_a_blank_or_whitespace_only_name_is_400(self):
        for name in ("", "   ", "\t\n"):
            with self.subTest(name=repr(name)):
                self.assertError(self._post({"name": name}), 400)

    def test_a_missing_or_non_string_name_is_400(self):
        for body in ({}, {"email": "x@example.com"}, {"name": None}, {"name": 42},
                     {"name": ["Bob"]}):
            with self.subTest(body=body):
                self.assertError(self._post(body), 400)

    def test_a_100_character_name_is_accepted_and_101_is_rejected(self):
        ok = self._post({"name": "n" * 100})
        self.assertStatus(ok, 201)
        self.assertEqual(len(ok.json["name"]), 100)
        self.assertError(self._post({"name": "n" * 101}), 400)

    def test_an_email_without_an_at_sign_is_400(self):
        for bad in ("bob", "bob.example.com", "  bob  "):
            with self.subTest(email=bad):
                self.assertError(self._post({"name": "Bob", "email": bad}), 400)

    def test_an_email_longer_than_320_characters_is_400(self):
        long_local = "b" * 310
        self.assertError(self._post({"name": "Bob", "email": f"{long_local}@example.com"}), 400)

    def test_a_non_string_email_is_400_not_500(self):
        self.assertError(self._post({"name": "Bob", "email": 42}), 400)

    def test_a_rejected_add_leaves_the_member_list_untouched(self):
        before = len(self.c.get(f"/api/splits/{self.split['id']}/members").json)
        self.assertError(self._post({"name": ""}), 400)
        self.assertError(self._post({"name": "Bob", "email": "no-at-sign"}), 400)
        after = len(self.c.get(f"/api/splits/{self.split['id']}/members").json)
        self.assertEqual(after, before, "a 400 must not have inserted anything")


class TestListMembers(ApiTestCase, unittest.TestCase):

    def test_the_list_is_a_bare_array_ordered_by_joined_at_ascending(self):
        c = new_user(display_name="Alice")
        split = c.make_split()
        added = [c.make_member(split["id"], name=n) for n in ("Bob", "Carol", "Dave")]

        r = c.get(f"/api/splits/{split['id']}/members")
        self.assertStatus(r, 200)
        self.assertIsInstance(r.json, list, "list endpoints return a bare array")
        self.assertEqual([m["name"] for m in r.json], ["Alice", "Bob", "Carol", "Dave"],
                         "members come back in the order they joined")
        self.assertEqual([m["id"] for m in r.json][1:], [m["id"] for m in added])

        timestamps = [m["joined_at"] for m in r.json]
        self.assertEqual(timestamps, sorted(timestamps),
                         f"joined_at must be non-decreasing: {timestamps}")

    def test_every_listed_member_has_the_documented_shape(self):
        c = new_user()
        split = c.make_split()
        c.make_member(split["id"], name="Bob", email="bob@example.com")
        for m in c.get(f"/api/splits/{split['id']}/members").json:
            self.assertEqual(set(m.keys()), MEMBER_KEYS)
            self.assertEqual(m["split_id"], split["id"])

    def test_a_split_with_no_members_lists_an_empty_array_not_a_404(self):
        c = new_user()
        split = c.make_split()
        owner_member = c.get(f"/api/splits/{split['id']}/members").json[0]
        self.assertStatus(
            c.delete(f"/api/splits/{split['id']}/members/{owner_member['id']}"), 200)
        r = c.get(f"/api/splits/{split['id']}/members")
        self.assertStatus(r, 200)
        self.assertEqual(r.json, [],
                         "a split with no members is a valid, if useless, state")

    def test_members_of_an_unknown_split_are_404(self):
        c = new_user()
        self.assertError(c.get(f"/api/splits/{MISSING_UUID}/members"), 404)

    def test_one_splits_members_never_leak_into_another(self):
        c = new_user()
        a, b = c.make_split(name="A"), c.make_split(name="B")
        c.make_member(a["id"], name="Only in A")
        names = [m["name"] for m in c.get(f"/api/splits/{b['id']}/members").json]
        self.assertNotIn("Only in A", names)


class TestRemoveMember(ApiTestCase, unittest.TestCase):

    def test_delete_returns_the_documented_message_and_drops_the_member(self):
        c = new_user()
        split = c.make_split()
        member = c.make_member(split["id"], name="Bob")
        r = c.delete(f"/api/splits/{split['id']}/members/{member['id']}")
        self.assertStatus(r, 200)
        self.assertEqual(r.json, {"message": "Removed"})
        ids = [m["id"] for m in c.get(f"/api/splits/{split['id']}/members").json]
        self.assertNotIn(member["id"], ids)

    def test_deleting_the_same_member_twice_is_404_the_second_time(self):
        c = new_user()
        split = c.make_split()
        member = c.make_member(split["id"], name="Bob")
        self.assertStatus(c.delete(f"/api/splits/{split['id']}/members/{member['id']}"), 200)
        self.assertError(c.delete(f"/api/splits/{split['id']}/members/{member['id']}"), 404)

    def test_removing_a_member_decrements_the_splits_member_count(self):
        c = new_user()
        split = c.make_split()
        member = c.make_member(split["id"], name="Bob")
        self.assertEqual(c.get(f"/api/splits/{split['id']}").json["member_count"], 2)
        c.delete(f"/api/splits/{split['id']}/members/{member['id']}")
        self.assertEqual(c.get(f"/api/splits/{split['id']}").json["member_count"], 1)

    def test_removing_the_last_remaining_member_is_allowed(self):
        c = new_user()
        split = c.make_split()
        for m in c.get(f"/api/splits/{split['id']}/members").json:
            self.assertStatus(c.delete(f"/api/splits/{split['id']}/members/{m['id']}"), 200)
        self.assertEqual(c.get(f"/api/splits/{split['id']}/members").json, [])
        self.assertEqual(c.get(f"/api/splits/{split['id']}").json["member_count"], 0)

    def test_an_unknown_member_id_is_404(self):
        c = new_user()
        split = c.make_split()
        self.assertError(c.delete(f"/api/splits/{split['id']}/members/{MISSING_UUID}"), 404)

    def test_a_member_of_another_split_is_404_even_when_the_caller_owns_both(self):
        """The member exists and the split exists and both belong to the
        caller — it is the *pairing* that does not, so the answer is still 404
        and the member must survive."""
        c = new_user()
        a, b = c.make_split(name="A"), c.make_split(name="B")
        member_of_a = c.make_member(a["id"], name="Only in A")

        r = c.delete(f"/api/splits/{b['id']}/members/{member_of_a['id']}")
        self.assertError(r, 404)

        ids = [m["id"] for m in c.get(f"/api/splits/{a['id']}/members").json]
        self.assertIn(member_of_a["id"], ids,
                      "the rejected delete must not have removed the member")


class TestMemberOwnership(ApiTestCase, unittest.TestCase):
    """A split that is not the caller's is a 404 everywhere, so its members are
    neither listable, addable, nor deletable by a stranger."""

    def setUp(self):
        self.owner = new_user()
        self.stranger = new_user()
        self.split = self.owner.make_split(name="Owner's split")
        self.member = self.owner.make_member(self.split["id"], name="Bob")

    def test_a_stranger_gets_404_listing_another_users_members(self):
        self.assertError(self.stranger.get(f"/api/splits/{self.split['id']}/members"), 404)

    def test_a_stranger_gets_404_adding_to_another_users_split(self):
        r = self.stranger.post(f"/api/splits/{self.split['id']}/members", {"name": "Intruder"})
        self.assertError(r, 404)
        names = [m["name"] for m in
                 self.owner.get(f"/api/splits/{self.split['id']}/members").json]
        self.assertNotIn("Intruder", names, "the rejected add must not have inserted")

    def test_a_stranger_gets_404_deleting_another_users_member(self):
        r = self.stranger.delete(
            f"/api/splits/{self.split['id']}/members/{self.member['id']}")
        self.assertError(r, 404)
        ids = [m["id"] for m in
               self.owner.get(f"/api/splits/{self.split['id']}/members").json]
        self.assertIn(self.member["id"], ids,
                      "the rejected delete must not have removed the member")

    def test_a_stranger_cannot_reach_the_member_through_their_own_split(self):
        stranger_split = self.stranger.make_split(name="Stranger's split")
        r = self.stranger.delete(
            f"/api/splits/{stranger_split['id']}/members/{self.member['id']}")
        self.assertError(r, 404)

    def test_unauthenticated_member_requests_are_401(self):
        anon = Client()
        self.assertError(anon.get(f"/api/splits/{self.split['id']}/members"), 401)
        self.assertError(anon.post(f"/api/splits/{self.split['id']}/members",
                                   {"name": "Anon"}), 401)
        self.assertError(anon.delete(
            f"/api/splits/{self.split['id']}/members/{self.member['id']}"), 401)


class TestMalformedMemberIds(ApiTestCase, unittest.TestCase):
    """Both path parameters are raw user input on the way to a `::uuid` cast:
    404 or 400, never a 500."""

    def test_no_bad_split_id_produces_a_500_on_any_member_route(self):
        c = new_user()
        for bad in BAD_IDS + [MISSING_UUID, "0" * 36, MISSING_UUID + "x"]:
            base = f"/api/splits/{as_path_id(bad)}/members"
            for method, path, body in (
                ("GET", base, None),
                ("POST", base, {"name": "Bob"}),
                ("DELETE", f"{base}/{MISSING_UUID}", None),
            ):
                with self.subTest(split_id=bad, method=method):
                    r = c.request(method, path, body)
                    self.assertIn(r.status, (400, 404),
                                  f"{method} {path} returned {r.status}: {r.body[:200]}")
                    self.assertError(r, r.status)

    def test_no_bad_member_id_produces_a_500_on_delete(self):
        c = new_user()
        split = c.make_split()
        for bad in BAD_IDS + [MISSING_UUID, "0" * 36, MISSING_UUID + "x"]:
            with self.subTest(member_id=bad):
                r = c.delete(f"/api/splits/{split['id']}/members/{as_path_id(bad)}")
                self.assertIn(r.status, (400, 404),
                              f"DELETE with member id {bad!r} returned {r.status}: "
                              f"{r.body[:200]}")
                self.assertError(r, r.status)

    def test_an_injection_shaped_name_is_stored_as_text_not_executed(self):
        """The name is a parameter, not string-concatenated SQL. The proof is
        that it comes back verbatim and the split is still there."""
        c = new_user()
        split = c.make_split()
        nasty = "Bob'); DROP TABLE split_members; --"
        member = c.make_member(split["id"], name=nasty)
        self.assertEqual(member["name"], nasty)
        listed = c.get(f"/api/splits/{split['id']}/members").json
        self.assertIn(nasty, [m["name"] for m in listed])


if __name__ == "__main__":
    unittest.main()
