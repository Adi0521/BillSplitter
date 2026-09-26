"""Collaboration — invite links that bind an account to a member seat.

Asserted against the "Collaboration — members with accounts" section of
docs/api.md. The points that carry the most weight:

  * The token is the secret. It is returned exactly once, by the endpoint
    that mints it, and never appears in a member listing.
  * A link works exactly once. A second claim is a 404 indistinguishable from
    a link that never existed — the endpoint must not be an oracle for which
    links used to be valid.
  * One account, one seat per split (owner included): 409, and the partial
    unique index backs the handler up under a race.
  * 403 appears only when split_role() said 'member' and the action is one of
    the four owner-only ones. NULL is always 404, even on owner-only routes,
    so a stranger cannot learn a split exists by being refused permission.
  * Leaving unlinks the account but keeps the seat: the ledger written
    against that seat must survive the person's departure.
"""
import unittest
import uuid
from urllib.parse import quote

from harness import (BAD_IDS, MISSING_UUID, ApiTestCase, Client,
                     make_collaborator, new_user)


def as_path_id(value):
    return quote(value, safe="")


PREVIEW_KEYS = {"split_name", "member_name", "invited_by", "already_member", "split_id"}

# Well-formed tokens that were never issued, plus things that are not tokens.
BAD_TOKENS = ["0" * 32, "f" * 32, "not-a-token", "", " ", "0" * 31, "0" * 33,
              "G" * 32, "', 'x", "%27%3BDROP%20TABLE%20split_members%3B--"]


class InviteCase(ApiTestCase, unittest.TestCase):

    def invite(self, client, split_id, member_id):
        return client.post(f"/api/splits/{split_id}/members/{member_id}/invite")

    def revoke(self, client, split_id, member_id):
        return client.delete(f"/api/splits/{split_id}/members/{member_id}/invite")

    def preview(self, client, token):
        return client.get(f"/api/invites/{token}")

    def claim(self, client, token):
        return client.post(f"/api/invites/{token}/claim")

    def leave(self, client, split_id):
        return client.post(f"/api/splits/{split_id}/leave")

    def member_by_id(self, client, split_id, member_id):
        listed = client.get(f"/api/splits/{split_id}/members")
        self.assertStatus(listed, 200)
        found = [m for m in listed.json if m["id"] == member_id]
        self.assertEqual(len(found), 1, f"member {member_id} not listed exactly once")
        return found[0]


class TestFullFlow(InviteCase):

    def test_make_collaborator_runs_the_whole_handshake(self):
        """harness.make_collaborator asserts every step of the contract on the
        way through; this test exists so a broken flow fails *here*, by name,
        rather than in the setUp of forty unrelated tests."""
        owner = new_user(display_name="Alice")
        split = owner.make_split(name="Tahoe trip")
        collab, seat = make_collaborator(owner, split["id"], name="Bob")

        linked = self.member_by_id(owner, split["id"], seat["id"])
        self.assertEqual(linked["user_id"], collab.user["id"],
                         "claiming binds the caller's account to the seat")
        self.assertTrue(linked["linked"])
        self.assertFalse(linked["invite_pending"], "claiming clears the token")
        self.assertFalse(linked["is_owner"])
        self.assertNotIn("invite_token", linked)

    def test_the_invite_response_has_the_token_and_the_path(self):
        owner = new_user()
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        r = self.invite(owner, split["id"], seat["id"])
        self.assertStatus(r, 200)
        self.assertEqual(set(r.json.keys()), {"invite_token", "invite_path"})
        token = r.json["invite_token"]
        self.assertRegex(token, r"^[0-9a-f]{32}$", "32 lowercase hex chars")
        self.assertEqual(r.json["invite_path"], f"/invite/{token}")

    def test_an_invited_seat_shows_invite_pending_but_never_the_token(self):
        owner = new_user()
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]

        for path in (f"/api/splits/{split['id']}/members", f"/api/splits/{split['id']}"):
            with self.subTest(path=path):
                r = owner.get(path)
                self.assertStatus(r, 200)
                self.assertNotIn(token, r.body,
                                 "the token must never appear in a listing")
                members = r.json if isinstance(r.json, list) else r.json["members"]
                pending = [m for m in members if m["id"] == seat["id"]][0]
                self.assertTrue(pending["invite_pending"])
                self.assertFalse(pending["linked"])
                self.assertNotIn("invite_token", pending)

    def test_the_split_appears_in_the_collaborators_list_with_role_member(self):
        owner = new_user()
        split = owner.make_split(name="Shared")
        collab, _ = make_collaborator(owner, split["id"])
        listed = {s["id"]: s for s in collab.get("/api/splits").json}
        self.assertIn(split["id"], listed)
        self.assertEqual(listed[split["id"]]["role"], "member")
        mine = {s["id"]: s for s in owner.get("/api/splits").json}
        self.assertEqual(mine[split["id"]]["role"], "owner")


class TestInviteReplacementAndRevocation(InviteCase):

    def test_a_new_invite_replaces_the_old_token_and_the_old_link_404s(self):
        owner = new_user()
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        first = self.invite(owner, split["id"], seat["id"]).json["invite_token"]
        second = self.invite(owner, split["id"], seat["id"]).json["invite_token"]
        self.assertNotEqual(first, second)

        invitee = new_user()
        self.assertError(self.preview(invitee, first), 404)
        self.assertError(self.claim(invitee, first), 404)
        self.assertStatus(self.preview(invitee, second), 200)

    def test_revoking_kills_the_link_and_clears_invite_pending(self):
        owner = new_user()
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]

        r = self.revoke(owner, split["id"], seat["id"])
        self.assertStatus(r, 200)
        self.assertEqual(r.json, {"message": "Invite revoked"})
        self.assertFalse(self.member_by_id(owner, split["id"], seat["id"])["invite_pending"])

        invitee = new_user()
        self.assertError(self.preview(invitee, token), 404)
        self.assertError(self.claim(invitee, token), 404)

    def test_revoking_a_seat_with_no_invite_is_a_200_no_op(self):
        owner = new_user()
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        self.assertStatus(self.revoke(owner, split["id"], seat["id"]), 200)

    def test_inviting_an_already_linked_seat_is_400(self):
        owner = new_user()
        split = owner.make_split()
        _, seat = make_collaborator(owner, split["id"])
        self.assertError(self.invite(owner, split["id"], seat["id"]), 400)

    def test_inviting_the_owners_own_seat_is_400_because_it_is_linked(self):
        owner = new_user()
        split = owner.make_split()
        owner_seat = [m for m in owner.get(f"/api/splits/{split['id']}/members").json
                      if m["is_owner"]][0]
        self.assertError(self.invite(owner, split["id"], owner_seat["id"]), 400)

    def test_inviting_a_seat_from_another_split_is_404(self):
        owner = new_user()
        a, b = owner.make_split(name="A"), owner.make_split(name="B")
        seat_in_a = owner.make_member(a["id"], name="Only in A")
        self.assertError(self.invite(owner, b["id"], seat_in_a["id"]), 404)
        self.assertError(self.invite(owner, a["id"], MISSING_UUID), 404)


class TestClaim(InviteCase):

    def test_claiming_twice_is_404_the_second_time(self):
        owner = new_user()
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]

        first, second = new_user(), new_user()
        self.assertStatus(self.claim(first, token), 200)
        r = self.claim(second, token)
        self.assertError(r, 404)
        self.assertEqual(self.member_by_id(owner, split["id"], seat["id"])["user_id"],
                         first.user["id"], "the seat stays with the first claimant")

    def test_a_consumed_link_previews_as_404_too(self):
        owner = new_user()
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]
        self.assertStatus(self.claim(new_user(), token), 200)
        self.assertError(self.preview(new_user(), token), 404)

    def test_consumed_revoked_and_unknown_tokens_share_one_error_message(self):
        """Do not tell a caller *why* their link failed."""
        owner = new_user()
        split = owner.make_split()
        consumed_seat = owner.make_member(split["id"], name="Consumed")
        revoked_seat = owner.make_member(split["id"], name="Revoked")
        consumed = self.invite(owner, split["id"], consumed_seat["id"]).json["invite_token"]
        revoked = self.invite(owner, split["id"], revoked_seat["id"]).json["invite_token"]
        self.assertStatus(self.claim(new_user(), consumed), 200)
        self.assertStatus(self.revoke(owner, split["id"], revoked_seat["id"]), 200)

        caller = new_user()
        messages = {self.claim(caller, t).error for t in (consumed, revoked, "0" * 32)}
        self.assertEqual(len(messages), 1, f"one message for all three, got {messages}")

    def test_claiming_a_seat_in_a_split_you_already_belong_to_is_409(self):
        owner = new_user()
        split = owner.make_split()
        collab, _ = make_collaborator(owner, split["id"], name="Bob")

        second_seat = owner.make_member(split["id"], name="Bob again")
        token = self.invite(owner, split["id"], second_seat["id"]).json["invite_token"]
        r = self.claim(collab, token)
        self.assertError(r, 409)
        self.assertIsNone(self.member_by_id(owner, split["id"], second_seat["id"])["user_id"],
                          "a 409 must not have written")
        # The link is still live for someone else.
        self.assertStatus(self.claim(new_user(), token), 200)

    def test_the_owner_cannot_claim_a_seat_in_their_own_split(self):
        owner = new_user()
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]
        self.assertError(self.claim(owner, token), 409)
        self.assertIsNone(self.member_by_id(owner, split["id"], seat["id"])["user_id"])

    def test_claiming_returns_the_split_id_and_grants_access(self):
        owner = new_user()
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]

        invitee = new_user()
        # Before claiming, the invitee is a stranger.
        self.assertError(invitee.get(f"/api/splits/{split['id']}"), 404)
        r = self.claim(invitee, token)
        self.assertStatus(r, 200)
        self.assertEqual(r.json, {"split_id": split["id"]})
        detail = invitee.get(f"/api/splits/{split['id']}")
        self.assertStatus(detail, 200)
        self.assertEqual(detail.json["role"], "member")

    def test_claiming_requires_a_session(self):
        owner = new_user()
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]
        anon = Client()
        self.assertError(self.claim(anon, token), 401)
        self.assertError(self.preview(anon, token), 401)


class TestPreview(InviteCase):

    def test_preview_has_exactly_the_documented_shape(self):
        owner = new_user(display_name="Alice")
        split = owner.make_split(name="Tahoe trip")
        seat = owner.make_member(split["id"], name="Bob")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]

        r = self.preview(new_user(), token)
        self.assertStatus(r, 200)
        self.assertEqual(set(r.json.keys()), PREVIEW_KEYS,
                         f"InvitePreview shape drifted: {sorted(r.json.keys())}")
        self.assertEqual(r.json["split_name"], "Tahoe trip")
        self.assertEqual(r.json["member_name"], "Bob")
        self.assertEqual(r.json["invited_by"], "Alice")
        self.assertFalse(r.json["already_member"])

    def test_preview_never_contains_the_owners_email(self):
        owner = new_user(display_name="Alice")
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]
        r = self.preview(new_user(), token)
        self.assertStatus(r, 200)
        self.assertNotIn(owner.user["email"], r.body)
        self.assertNotIn("@", r.body)

    def test_invited_by_falls_back_to_the_email_local_part_not_the_email(self):
        local_part = f"fallback-owner-{uuid.uuid4().hex[:10]}"
        owner = Client()
        owner.register(email=f"{local_part}@example.com", display_name="")
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]
        r = self.preview(new_user(), token)
        self.assertStatus(r, 200)
        self.assertEqual(r.json["invited_by"], local_part)
        self.assertNotIn("@", r.body)

    def test_already_member_is_true_for_the_owner_and_an_existing_collaborator(self):
        owner = new_user()
        split = owner.make_split()
        collab, _ = make_collaborator(owner, split["id"])
        seat = owner.make_member(split["id"], name="Someone else")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]
        self.assertTrue(self.preview(owner, token).json["already_member"])
        self.assertTrue(self.preview(collab, token).json["already_member"])
        self.assertFalse(self.preview(new_user(), token).json["already_member"])

    def test_preview_does_not_consume_the_link(self):
        owner = new_user()
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = self.invite(owner, split["id"], seat["id"]).json["invite_token"]
        invitee = new_user()
        for _ in range(3):
            self.assertStatus(self.preview(invitee, token), 200)
        self.assertStatus(self.claim(invitee, token), 200)


class TestLeave(InviteCase):

    def test_leaving_unlinks_the_account_but_the_seat_and_its_payments_survive(self):
        owner = new_user(display_name="Alice")
        split = owner.make_split()
        collab, seat = make_collaborator(owner, split["id"], name="Bob")
        owner_seat = [m for m in owner.get(f"/api/splits/{split['id']}/members").json
                      if m["is_owner"]][0]
        payment = owner.post(f"/api/splits/{split['id']}/payments",
                             {"from_member": seat["id"], "to_member": owner_seat["id"],
                              "amount": "20.00"})
        self.assertStatus(payment, 201)

        r = self.leave(collab, split["id"])
        self.assertStatus(r, 200)
        self.assertEqual(r.json, {"message": "Left"})

        after = self.member_by_id(owner, split["id"], seat["id"])
        self.assertIsNone(after["user_id"], "leaving clears user_id")
        self.assertFalse(after["linked"])
        self.assertEqual(after["name"], "Bob", "the seat itself is kept")

        payments = owner.get(f"/api/splits/{split['id']}/payments")
        self.assertStatus(payments, 200)
        self.assertIn(payment.json["id"], [p["id"] for p in payments.json],
                      "the ledger written against the seat must survive")
        self.assertEqual(
            [p for p in payments.json if p["id"] == payment.json["id"]][0]["from_member"],
            seat["id"])

    def test_after_leaving_the_split_is_gone_from_the_list_and_404s(self):
        owner = new_user()
        split = owner.make_split()
        collab, _ = make_collaborator(owner, split["id"])
        self.assertStatus(self.leave(collab, split["id"]), 200)
        self.assertNotIn(split["id"], [s["id"] for s in collab.get("/api/splits").json])
        self.assertError(collab.get(f"/api/splits/{split['id']}"), 404)
        # Having left, the caller is a stranger again.
        self.assertError(self.leave(collab, split["id"]), 404)

    def test_a_left_seat_can_be_re_invited(self):
        owner = new_user()
        split = owner.make_split()
        collab, seat = make_collaborator(owner, split["id"])
        self.assertStatus(self.leave(collab, split["id"]), 200)
        r = self.invite(owner, split["id"], seat["id"])
        self.assertStatus(r, 200, "an unlinked seat is invitable again: ")
        self.assertStatus(self.claim(collab, r.json["invite_token"]), 200)

    def test_the_owner_cannot_leave(self):
        owner = new_user()
        split = owner.make_split()
        r = self.leave(owner, split["id"])
        self.assertError(r, 400)
        self.assertIn("archive", r.error.lower(), "the message should point at archiving")
        self.assertEqual(owner.get(f"/api/splits/{split['id']}").json["role"], "owner")

    def test_a_stranger_leaving_is_404(self):
        owner, stranger = new_user(), new_user()
        split = owner.make_split()
        self.assertError(self.leave(stranger, split["id"]), 404)
        self.assertError(self.leave(stranger, MISSING_UUID), 404)


class TestInviteAuthorization(InviteCase):
    """403 only for a member on an owner-only action. NULL role is 404, even
    on those same actions, so a stranger learns nothing."""

    def setUp(self):
        self.owner = new_user()
        self.stranger = new_user()
        self.split = self.owner.make_split()
        self.collab, self.collab_seat = make_collaborator(self.owner, self.split["id"])
        self.guest = self.owner.make_member(self.split["id"], name="Guest")

    def test_a_member_creating_an_invite_is_403(self):
        r = self.invite(self.collab, self.split["id"], self.guest["id"])
        self.assertError(r, 403)
        self.assertFalse(
            self.member_by_id(self.owner, self.split["id"], self.guest["id"])["invite_pending"],
            "a 403 must not have minted a token")

    def test_a_member_revoking_an_invite_is_403_and_the_link_survives(self):
        token = self.invite(self.owner, self.split["id"], self.guest["id"]).json["invite_token"]
        self.assertError(self.revoke(self.collab, self.split["id"], self.guest["id"]), 403)
        self.assertStatus(self.preview(new_user(), token), 200)

    def test_a_stranger_creating_or_revoking_an_invite_is_404_not_403(self):
        self.assertError(self.invite(self.stranger, self.split["id"], self.guest["id"]), 404)
        self.assertError(self.revoke(self.stranger, self.split["id"], self.guest["id"]), 404)

    def test_a_stranger_cannot_reach_the_seat_through_their_own_split(self):
        theirs = self.stranger.make_split()
        self.assertError(self.invite(self.stranger, theirs["id"], self.guest["id"]), 404)

    def test_unauthenticated_invite_requests_are_401(self):
        anon = Client()
        self.assertError(self.invite(anon, self.split["id"], self.guest["id"]), 401)
        self.assertError(self.revoke(anon, self.split["id"], self.guest["id"]), 401)
        self.assertError(self.leave(anon, self.split["id"]), 401)


class TestMalformedInviteInput(InviteCase):
    """Tokens and ids are raw user input. Anything malformed is a 404 with the
    documented error envelope — never a 500."""

    def test_malformed_and_unknown_tokens_are_404_not_500(self):
        c = new_user()
        for bad in BAD_TOKENS:
            if bad == "":
                continue  # `/api/invites/` matches no route at all
            for method, path in (("GET", f"/api/invites/{as_path_id(bad)}"),
                                 ("POST", f"/api/invites/{as_path_id(bad)}/claim")):
                with self.subTest(token=bad, method=method):
                    r = c.request(method, path, {} if method == "POST" else None)
                    self.assertError(r, 404)

    def test_malformed_ids_on_invite_and_leave_routes_are_404_not_500(self):
        c = new_user()
        split = c.make_split()
        seat = c.make_member(split["id"], name="Bob")
        for bad in BAD_IDS + [MISSING_UUID, "0" * 36, MISSING_UUID + "x"]:
            b = as_path_id(bad)
            for method, path in (
                ("POST",   f"/api/splits/{b}/members/{seat['id']}/invite"),
                ("DELETE", f"/api/splits/{b}/members/{seat['id']}/invite"),
                ("POST",   f"/api/splits/{split['id']}/members/{b}/invite"),
                ("DELETE", f"/api/splits/{split['id']}/members/{b}/invite"),
                ("POST",   f"/api/splits/{b}/leave"),
            ):
                with self.subTest(id=bad, method=method, path=path):
                    r = c.request(method, path, {} if method == "POST" else None)
                    self.assertError(r, 404)


if __name__ == "__main__":
    unittest.main()


class TestInviteCarriesSplitId(ApiTestCase, unittest.TestCase):
    """The UI needs a split id to deep-link an already-member caller. Split
    names are not unique, so matching by name is not a substitute."""

    def test_preview_includes_the_split_id(self):
        owner = new_user(display_name="Alice")
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = owner.post(f"/api/splits/{split['id']}/members/{seat['id']}/invite").json["invite_token"]
        r = new_user().get(f"/api/invites/{token}")
        self.assertStatus(r, 200)
        self.assertEqual(r.json["split_id"], split["id"])

    def test_the_409_names_the_split_the_caller_is_already_in(self):
        owner = new_user(display_name="Alice")
        split = owner.make_split()
        seat = owner.make_member(split["id"], name="Bob")
        token = owner.post(f"/api/splits/{split['id']}/members/{seat['id']}/invite").json["invite_token"]
        r = owner.post(f"/api/invites/{token}/claim")     # owner already holds a seat
        self.assertError(r, 409)
        self.assertEqual(r.json["split_id"], split["id"])
