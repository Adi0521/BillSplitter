"""Per-bill shares: the proportional split of tax, tip and fees.

The rule under test (docs/api.md, "Proportional tax, tip and fees"):

    A member's share of each is `their_items / subtotal * amount`, rounded to
    4 decimals in NUMERIC. The denominator is the bill's **full** subtotal, not
    the allocated portion.

Getting that denominator wrong is the expensive bug in this whole system: it
would make two people quietly cover the tax on a third person's unassigned
lunch, and it would never look wrong on screen. The first test in this file
computes both candidate answers with decimal.Decimal and asserts the response
matches the full-subtotal one and differs from the allocated-subtotal one.

Every expected value here is a Decimal or a literal 4-decimal string. A float
is never used to derive an expectation.
"""
import unittest
from decimal import Decimal, ROUND_HALF_UP, localcontext

from harness import ApiTestCase, BAD_IDS, MISSING_UUID, new_user

# BAD_IDS contains a space, which http.client refuses to put in a request line.
def path_id(raw):
    return raw.replace(" ", "%20")


FOUR_PLACES = Decimal("0.0001")


def proportional(items, subtotal, amount):
    """`items / subtotal * amount` rounded to 4 places, in exact decimal.

    Written in the same order Postgres evaluates it (divide, then multiply,
    then round once) so the expectation is the contract's arithmetic and not a
    convenient re-association of it. Postgres NUMERIC rounds half away from
    zero, which for these non-negative values is ROUND_HALF_UP.
    """
    items, subtotal, amount = (Decimal(items), Decimal(subtotal), Decimal(amount))
    if subtotal == 0:
        return Decimal("0.0000")
    with localcontext() as ctx:
        ctx.prec = 30
        value = (items / subtotal) * amount
    return value.quantize(FOUR_PLACES, rounding=ROUND_HALF_UP)


BILL_MONEY = ("subtotal", "tax", "tip", "fees", "total",
              "allocated_subtotal", "unallocated_subtotal")
MEMBER_MONEY = ("items", "tax", "tip", "fees", "total", "owes_payer")
UNALLOCATED_MONEY = ("items", "tax", "tip", "fees", "total")


class ShareCase(ApiTestCase, unittest.TestCase):
    def shares(self, client, bill):
        r = client.get(f"/api/splits/{bill['split_id']}/bills/{bill['id']}/shares")
        self.assertStatus(r, 200)
        self.assertSharesMoney(r.json)
        return r.json

    def assertSharesMoney(self, s):
        """Every monetary field in the response is a 4-decimal JSON string."""
        for field in BILL_MONEY:
            self.assertMoneyString(s[field], field)
        for m in s["members"]:
            for field in MEMBER_MONEY:
                self.assertMoneyString(m[field], f"members[].{field}")
        for field in UNALLOCATED_MONEY:
            self.assertMoneyString(s["unallocated"][field], f"unallocated.{field}")

    def by_member(self, s):
        return {m["member_id"]: m for m in s["members"]}


class TestTheDenominator(ShareCase):
    """The single most important assertion in the suite."""

    def test_tax_is_divided_by_the_full_subtotal_not_the_allocated_part(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bob = c.make_member(split["id"], "Bob")
        bill = c.make_bill(split["id"], tax="10.00")

        # $40 assigned to Alice, $60 assigned to nobody. Subtotal $100.
        assigned = c.make_item(bill["id"], name="Alice's dinner", price="40.00")
        c.make_item(bill["id"], name="Nobody's lunch", price="60.00")
        r = c.allocate(bill["id"], assigned["id"], "ratio",
                       [{"member_id": alice["id"], "ratio": "100"}])
        self.assertStatus(r, 200)

        s = self.shares(c, bill)
        self.assertEqual(s["subtotal"], "100.0000")
        self.assertEqual(s["allocated_subtotal"], "40.0000")
        self.assertEqual(s["unallocated_subtotal"], "60.0000")

        # The two candidate answers, both computed in exact decimal.
        correct = proportional("40.00", "100.00", "10.00")     # full subtotal
        wrong = proportional("40.00", "40.00", "10.00")        # allocated only
        self.assertEqual(correct, Decimal("1.0000") * 4)       # 4.0000
        self.assertEqual(wrong, Decimal("10.0000"))
        self.assertNotEqual(correct, wrong, "the fixture must distinguish them")

        alice_share = self.by_member(s)[alice["id"]]
        self.assertEqual(Decimal(alice_share["tax"]), correct,
                         "tax must be divided by the bill's FULL subtotal")
        self.assertNotEqual(
            Decimal(alice_share["tax"]), wrong,
            "tax was divided by the allocated subtotal: an unassigned item's "
            "tax is being pushed onto the people who are present")
        self.assertEqual(alice_share["tax"], "4.0000")
        self.assertEqual(alice_share["items"], "40.0000")
        self.assertEqual(Decimal(alice_share["total"]),
                         Decimal("40.0000") + correct)

        # Bob is on the bill's split but on none of its items.
        self.assertEqual(self.by_member(s)[bob["id"]]["tax"], "0.0000")

    def test_unassigned_items_keep_their_slice_of_tax_tip_and_fees(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bill = c.make_bill(split["id"], tax="10.00", tip="5.00", fees="2.50")
        assigned = c.make_item(bill["id"], price="40.00")
        c.make_item(bill["id"], price="60.00")
        c.allocate(bill["id"], assigned["id"], "ratio",
                   [{"member_id": alice["id"], "ratio": "100"}])

        s = self.shares(c, bill)
        un = s["unallocated"]
        self.assertEqual(un["items"], "60.0000")
        for field, amount in [("tax", "10.00"), ("tip", "5.00"), ("fees", "2.50")]:
            with self.subTest(field=field):
                self.assertEqual(Decimal(un[field]),
                                 proportional("60.00", "100.00", amount))
        self.assertEqual(un["tax"], "6.0000")
        self.assertEqual(un["tip"], "3.0000")
        self.assertEqual(un["fees"], "1.5000")
        self.assertEqual(
            Decimal(un["total"]),
            Decimal(un["items"]) + Decimal(un["tax"]) + Decimal(un["tip"])
            + Decimal(un["fees"]))
        self.assertEqual(un["total"], "70.5000")

        # The unallocated slice is the proportional share of the unassigned
        # items, computed the same way a member's is — not the leftover
        # tax - SUM(members.tax), which would make the columns tie by
        # construction and hide the rounding residual.
        alice_share = self.by_member(s)[alice["id"]]
        self.assertEqual(
            Decimal(alice_share["tax"]) + Decimal(un["tax"]), Decimal("10.0000"),
            "these happen to tie here because the arithmetic is exact")

    def test_a_proportional_share_that_does_not_divide_evenly(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bob = c.make_member(split["id"], "Bob")
        bill = c.make_bill(split["id"], tax="3.83")
        first = c.make_item(bill["id"], price="25.00")
        second = c.make_item(bill["id"], price="36.49")
        c.allocate(bill["id"], first["id"], "ratio",
                   [{"member_id": alice["id"], "ratio": "100"}])
        c.allocate(bill["id"], second["id"], "ratio",
                   [{"member_id": bob["id"], "ratio": "100"}])

        s = self.shares(c, bill)
        self.assertEqual(s["subtotal"], "61.4900")
        members = self.by_member(s)
        self.assertEqual(Decimal(members[alice["id"]]["tax"]),
                         proportional("25.00", "61.49", "3.83"))
        self.assertEqual(Decimal(members[bob["id"]]["tax"]),
                         proportional("36.49", "61.49", "3.83"))
        # Exact values, so a change in rounding direction is visible.
        self.assertEqual(members[alice["id"]]["tax"], "1.5572")
        self.assertEqual(members[bob["id"]]["tax"], "2.2728")


class TestZeroSubtotal(ShareCase):
    """The division-by-zero that must never reach Postgres."""

    def test_a_bill_with_no_items_is_a_200_with_everything_unallocated(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bill = c.make_bill(split["id"], tax="5.00", tip="2.00", fees="1.00")

        r = c.get(f"/api/splits/{split['id']}/bills/{bill['id']}/shares")
        self.assertStatus(r, 200, "a zero subtotal must never be a 500: ")
        s = r.json
        self.assertSharesMoney(s)

        self.assertEqual(s["subtotal"], "0.0000")
        self.assertEqual(s["total"], "8.0000")
        self.assertEqual(s["allocated_subtotal"], "0.0000")
        self.assertEqual(s["unallocated_subtotal"], "0.0000")

        # Every member's proportional share is zero...
        self.assertTrue(s["members"], "the split's members must still be listed")
        for m in s["members"]:
            with self.subTest(member=m["name"]):
                for field in MEMBER_MONEY:
                    self.assertEqual(m[field], "0.0000",
                                     f"{field} of a zero-subtotal bill")
        self.assertIn(alice["id"], self.by_member(s))

        # ... and the WHOLE of tax, tip and fees lands in unallocated, rather
        # than sitting in no bucket at all.
        self.assertEqual(s["unallocated"]["items"], "0.0000")
        self.assertEqual(s["unallocated"]["tax"], "5.0000")
        self.assertEqual(s["unallocated"]["tip"], "2.0000")
        self.assertEqual(s["unallocated"]["fees"], "1.0000")
        self.assertEqual(s["unallocated"]["total"], "8.0000")

    def test_a_bill_of_zero_priced_items_is_also_a_200(self):
        c = new_user()
        split = c.make_split()
        member = c.make_member(split["id"], "Alice")
        bill = c.make_bill(split["id"], tax="4.00")
        item = c.make_item(bill["id"], price="0", quantity=3)
        # A zero-priced line cannot carry a nonzero allocation, so nothing is
        # assigned; the subtotal is still zero and the guard still applies.
        s = self.shares(c, bill)
        self.assertEqual(s["subtotal"], "0.0000")
        self.assertEqual(s["unallocated"]["tax"], "4.0000")
        self.assertEqual(self.by_member(s)[member["id"]]["tax"], "0.0000")
        self.assertEqual(item["line_total"], "0.0000")

    def test_a_bill_with_no_tax_tip_or_fees_is_all_zeros(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bill = c.make_bill(split["id"])
        item = c.make_item(bill["id"], price="10.00")
        c.allocate(bill["id"], item["id"], "ratio",
                   [{"member_id": alice["id"], "ratio": "100"}])
        s = self.shares(c, bill)
        share = self.by_member(s)[alice["id"]]
        self.assertEqual(share["items"], "10.0000")
        self.assertEqual(share["tax"], "0.0000")
        self.assertEqual(share["total"], "10.0000")
        self.assertEqual(s["unallocated"]["total"], "0.0000")


class TestMemberRows(ShareCase):
    def test_a_member_with_no_allocations_still_appears_with_zeros(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bystander = c.make_member(split["id"], "Bystander")
        bill = c.make_bill(split["id"], tax="10.00")
        item = c.make_item(bill["id"], price="50.00")
        c.allocate(bill["id"], item["id"], "ratio",
                   [{"member_id": alice["id"], "ratio": "100"}])

        s = self.shares(c, bill)
        members = self.by_member(s)
        self.assertIn(bystander["id"], members,
                      "a member with nothing on this bill must still be listed")
        row = members[bystander["id"]]
        self.assertEqual(row["name"], "Bystander")
        for field in ("items", "tax", "tip", "fees", "total"):
            self.assertEqual(row[field], "0.0000", field)

    def test_a_members_items_sum_across_the_bill(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bob = c.make_member(split["id"], "Bob")
        bill = c.make_bill(split["id"])
        first = c.make_item(bill["id"], price="10.00")
        second = c.make_item(bill["id"], price="20.00", quantity=2)
        c.allocate(bill["id"], first["id"], "ratio",
                   [{"member_id": alice["id"], "ratio": "50"},
                    {"member_id": bob["id"], "ratio": "50"}])
        c.allocate(bill["id"], second["id"], "amount",
                   [{"member_id": alice["id"], "amount": "30.00"}])

        s = self.shares(c, bill)
        members = self.by_member(s)
        self.assertEqual(Decimal(members[alice["id"]]["items"]),
                         Decimal("5.00") + Decimal("30.00"))
        self.assertEqual(members[alice["id"]]["items"], "35.0000")
        self.assertEqual(members[bob["id"]]["items"], "5.0000")
        self.assertEqual(s["subtotal"], "50.0000")
        self.assertEqual(s["allocated_subtotal"], "40.0000")
        self.assertEqual(s["unallocated_subtotal"], "10.0000")
        self.assertEqual(
            sum((Decimal(m["items"]) for m in s["members"]), Decimal("0")),
            Decimal(s["allocated_subtotal"]))

    def test_currency_is_the_bills_own(self):
        c = new_user()
        split = c.make_split(currency="EUR")
        bill = c.make_bill(split["id"])
        self.assertEqual(self.shares(c, bill)["currency"], "EUR")


class TestOwesPayer(ShareCase):
    def build(self, payer=None):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bob = c.make_member(split["id"], "Bob")
        bill = c.make_bill(split["id"], tax="10.00")
        item = c.make_item(bill["id"], price="100.00")
        c.allocate(bill["id"], item["id"], "ratio",
                   [{"member_id": alice["id"], "ratio": "60"},
                    {"member_id": bob["id"], "ratio": "40"}])
        if payer is not None:
            r = c.put(f"/api/splits/{split['id']}/bills/{bill['id']}",
                      {"payer_member_id": {"alice": alice, "bob": bob}[payer]["id"]})
            self.assertStatus(r, 200)
        return c, split, alice, bob, bill

    def test_the_payer_owes_nothing_and_everyone_else_owes_their_total(self):
        c, split, alice, bob, bill = self.build(payer="alice")
        s = self.shares(c, bill)
        self.assertEqual(s["payer_member_id"], alice["id"])
        members = self.by_member(s)

        self.assertEqual(members[alice["id"]]["owes_payer"], "0.0000",
                         "the payer does not owe himself")
        for member_id, row in members.items():
            if member_id == alice["id"]:
                continue
            with self.subTest(member=row["name"]):
                self.assertEqual(row["owes_payer"], row["total"])
        self.assertEqual(members[bob["id"]]["items"], "40.0000")
        self.assertEqual(Decimal(members[bob["id"]]["total"]),
                         Decimal("40.0000") + proportional("40", "100", "10"))
        self.assertEqual(members[bob["id"]]["owes_payer"], "44.0000")

    def test_with_no_payer_nobody_owes_anybody(self):
        c, split, alice, bob, bill = self.build(payer=None)
        s = self.shares(c, bill)
        self.assertIsNone(s["payer_member_id"])
        for row in s["members"]:
            with self.subTest(member=row["name"]):
                self.assertEqual(row["owes_payer"], "0.0000")
        # The totals themselves are unaffected by there being no payer.
        self.assertEqual(self.by_member(s)[alice["id"]]["total"], "66.0000")


class TestRoundingResidual(ShareCase):
    def test_the_columns_tie_only_up_to_the_documented_residual(self):
        """The contract says SUM(members) + unallocated equals total only up to
        the per-allocation rounding residual, and that the response reports what
        is true rather than forcing the columns to tie. So this asserts the gap
        is a rounding-sized sliver — not that it is zero, which would be a
        demand that somebody quietly absorb the missing hundredth of a cent."""
        c = new_user()
        split = c.make_split()
        people = [c.make_member(split["id"], n) for n in ("Alice", "Bob", "Carol")]
        bill = c.make_bill(split["id"], tax="1.00")
        item = c.make_item(bill["id"], price="10.00")
        c.allocate(bill["id"], item["id"], "ratio",
                   [{"member_id": p["id"], "ratio": "33.3333"} for p in people])

        s = self.shares(c, bill)
        self.assertEqual(s["subtotal"], "10.0000")
        self.assertEqual(s["total"], "11.0000")
        self.assertEqual(s["allocated_subtotal"], "9.9999")
        self.assertEqual(s["unallocated_subtotal"], "0.0001")

        # Each member gets 3.3333 of the line and the tax on that, and no
        # member's share was inflated to make the arithmetic come out even.
        for p in people:
            row = self.by_member(s)[p["id"]]
            with self.subTest(member=row["name"]):
                self.assertEqual(row["items"], "3.3333")
                self.assertEqual(Decimal(row["tax"]),
                                 proportional("3.3333", "10.00", "1.00"))
                self.assertEqual(row["tax"], "0.3333")

        summed = sum((Decimal(m["total"]) for m in s["members"]), Decimal("0"))
        residual = Decimal(s["total"]) - (summed + Decimal(s["unallocated"]["total"]))
        # A cent's tolerance: enough room for per-row rounding across a handful
        # of allocations, far too little to hide a real accounting error.
        self.assertLessEqual(abs(residual), Decimal("0.01"),
                             f"residual {residual} is larger than rounding explains")


class TestSharesOwnership(ShareCase):
    def test_another_user_gets_404(self):
        owner, other = new_user(), new_user()
        split = owner.make_split()
        bill = owner.make_bill(split["id"])
        self.assertError(
            other.get(f"/api/splits/{split['id']}/bills/{bill['id']}/shares"), 404)

    def test_a_bill_from_another_split_is_404(self):
        c = new_user()
        a, b = c.make_split(), c.make_split()
        bill = c.make_bill(a["id"])
        self.assertError(
            c.get(f"/api/splits/{b['id']}/bills/{bill['id']}/shares"), 404)

    def test_a_missing_bill_is_404(self):
        c = new_user()
        split = c.make_split()
        self.assertError(
            c.get(f"/api/splits/{split['id']}/bills/{MISSING_UUID}/shares"), 404)

    def test_unauthenticated_access_is_401(self):
        from harness import Client
        owner = new_user()
        split = owner.make_split()
        bill = owner.make_bill(split["id"])
        self.assertError(
            Client().get(f"/api/splits/{split['id']}/bills/{bill['id']}/shares"), 401)


class TestSharesBadIds(ShareCase):
    def test_bad_ids_never_500(self):
        c = new_user()
        split = c.make_split()
        bill = c.make_bill(split["id"])
        for bad in BAD_IDS:
            with self.subTest(bad=bad):
                for path in (
                    f"/api/splits/{path_id(bad)}/bills/{bill['id']}/shares",
                    f"/api/splits/{split['id']}/bills/{path_id(bad)}/shares",
                ):
                    r = c.get(path)
                    self.assertLess(r.status, 500, f"{r}")
                    self.assertIn(r.status, (400, 404), f"{r}")


if __name__ == "__main__":
    unittest.main()
