"""The summary and the public share view must report the same money.

`docs/api.md` says PublicSplit.by_currency is "same shape as
SplitSummary.by_currency". They are currently computed by two independent SQL
implementations in two files, so "same shape" is not enough — they must agree
value for value, or the same split shows one set of balances to its owner and a
different set to everyone they shared the link with.

This test exists to fail the moment those two derivations drift.
"""
import unittest
from harness import Client, new_user, ApiTestCase


def build_split_with_everything(owner):
    """A split exercising every path the balance arithmetic has: two payers,
    two currencies, a partially allocated bill, a zero-subtotal bill, a member
    with no activity, and a recorded payment."""
    split = owner.make_split(name="Agreement fixture", currency="USD")
    alice = owner.get(f"/api/splits/{split['id']}/members").json[0]
    bob = owner.make_member(split["id"], name="Bob")
    # Carol is allocated items but never fronts a bill, which is what produces a
    # negative balance. Without her every member is a net creditor, because the
    # unallocated remainder means nobody is on the hook for the rest.
    carol = owner.make_member(split["id"], name="Carol")
    owner.make_member(split["id"], name="Dave")  # no activity anywhere

    # Bill 1 — USD, Alice fronted, fully allocated between Alice and Bob
    b1 = owner.make_bill(split["id"], store_name="Dinner", tax="10.00",
                         payer_member_id=alice["id"])
    i1 = owner.make_item(b1["id"], name="Mains", price="100.00")
    owner.allocate(b1["id"], i1["id"], "ratio", [
        {"member_id": alice["id"], "ratio": "50"},
        {"member_id": bob["id"], "ratio": "30"},
        {"member_id": carol["id"], "ratio": "20"},
    ])

    # Bill 2 — USD, Bob fronted, only partly allocated (drives `unallocated`)
    b2 = owner.make_bill(split["id"], store_name="Taxi", tax="5.00",
                         payer_member_id=bob["id"])
    i2 = owner.make_item(b2["id"], name="Fare", price="50.00")
    owner.allocate(b2["id"], i2["id"], "amount",
                   [{"member_id": alice["id"], "amount": "20.00"}])

    # Bill 3 — EUR, so a second currency block exists and must stay separate
    b3 = owner.make_bill(split["id"], store_name="Museum", currency="EUR",
                         payer_member_id=alice["id"])
    i3 = owner.make_item(b3["id"], name="Tickets", price="33.00")
    owner.allocate(b3["id"], i3["id"], "ratio",
                   [{"member_id": bob["id"], "ratio": "100"}])

    # Bill 4 — nothing but tax: the zero-subtotal guard
    owner.make_bill(split["id"], store_name="Fee only", fees="8.00",
                    payer_member_id=alice["id"])

    # A settlement, so payments_made / payments_received are non-zero
    owner.post(f"/api/splits/{split['id']}/payments", {
        "from_member": bob["id"], "to_member": alice["id"], "amount": "4.00",
    })
    return split


class TestSummaryAndPublicAgree(ApiTestCase, unittest.TestCase):
    def setUp(self):
        self.owner = new_user(display_name="Alice")
        self.split = build_split_with_everything(self.owner)
        self.token = self.owner.get(f"/api/splits/{self.split['id']}").json["share_token"]

    def by_currency(self):
        s = self.owner.get(f"/api/splits/{self.split['id']}/summary")
        self.assertStatus(s, 200)
        p = Client().get(f"/api/splits/share/{self.token}")   # deliberately no session
        self.assertStatus(p, 200)
        return s.json["by_currency"], p.json["by_currency"]

    def test_the_two_views_report_identical_currency_blocks(self):
        summary, public = self.by_currency()
        self.assertEqual(
            summary, public,
            "the owner's summary and the public share view disagree about the "
            "money for the same split — two derivations have drifted",
        )

    def test_both_report_the_same_currencies_in_the_same_order(self):
        summary, public = self.by_currency()
        self.assertEqual([b["currency"] for b in summary],
                         [b["currency"] for b in public])

    def test_every_member_balance_matches_across_the_two_views(self):
        summary, public = self.by_currency()
        for s_block, p_block in zip(summary, public):
            s_bal = {m["member_id"]: m["balance"] for m in s_block["members"]}
            p_bal = {m["member_id"]: m["balance"] for m in p_block["members"]}
            self.assertEqual(s_bal, p_bal, f"balances differ for {s_block['currency']}")

    def test_settlements_match_across_the_two_views(self):
        summary, public = self.by_currency()
        for s_block, p_block in zip(summary, public):
            self.assertEqual(s_block["settlements"], p_block["settlements"],
                             f"settlements differ for {s_block['currency']}")

    def test_the_fixture_actually_exercises_the_interesting_paths(self):
        """Guards the test itself: if the fixture stops producing two
        currencies, an unallocated remainder and balances of both signs, the
        agreement above becomes vacuous."""
        summary, _ = self.by_currency()
        self.assertGreaterEqual(len(summary), 2, "fixture lost its second currency")
        usd = next(b for b in summary if b["currency"] == "USD")
        self.assertNotEqual(usd["unallocated"]["total"], "0.0000",
                            "fixture lost its unallocated remainder")
        signs = {m["balance"].startswith("-") for m in usd["members"]
                 if m["balance"] != "0.0000"}
        self.assertEqual(signs, {True, False}, "fixture lost balances of both signs")
