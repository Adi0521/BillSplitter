"""Split summary: balances, currency grouping and suggested settlements.

The rules under test (docs/api.md, "Phase 6 — Split summary"):

    balance = fronted + payments_made - owes - payments_received

    `owes` is the Phase 4 per-bill arithmetic added up: a member's items plus
    their proportional tax/tip/fees, where the denominator is each bill's FULL
    subtotal. `fronted` is the whole total of every bill they paid for.

Three things in that sentence are worth a test each, because each one is a bug
that looks plausible on screen:

  * "everyone owes the payer" is not the model. When Alice fronts dinner and
    Bob fronts the taxi, both of them are owed something and both of them owe
    something, and only the net has a sign. TestTwoPayers builds exactly that
    and asserts the balances come out with both signs.
  * currencies are never summed. A EUR bill and a USD bill produce two
    independent sets of balances; adding them would invent a number, and the
    invented number is the kind that nobody notices. TestMixedCurrency asserts
    the USD group is identical to what the USD bills produce on their own.
  * the balances need not sum to zero. Rounding residue and the unallocated
    remainder are real, and settlements must stop rather than invent a transfer
    to absorb them.

Every expected value here is a Decimal or a literal 4-decimal string. A float is
never used to derive an expectation.

NOTE: `make_split` adds the owner as a member automatically, so each split has
one more member than the test explicitly creates. Assertions are keyed by member
id and never by the length of `members`, except where that extra member is the
point.
"""
import unittest
from decimal import Decimal, ROUND_HALF_UP, localcontext

from harness import ApiTestCase, BAD_IDS, MISSING_UUID, Client, new_user

# BAD_IDS contains a space, which http.client refuses to put in a request line.
def path_id(raw):
    return raw.replace(" ", "%20")


FOUR_PLACES = Decimal("0.0001")
ZERO = Decimal("0.0000")


def proportional(items, subtotal, amount):
    """`items / subtotal * amount` rounded to 4 places, in exact decimal.

    Written in the order Postgres evaluates it (divide, multiply, round once),
    so the expectation is the contract's arithmetic rather than a convenient
    re-association of it. A zero subtotal contributes nothing to a member.
    """
    items, subtotal, amount = Decimal(items), Decimal(subtotal), Decimal(amount)
    if subtotal == 0:
        return ZERO
    with localcontext() as ctx:
        ctx.prec = 30
        value = (items / subtotal) * amount
    return value.quantize(FOUR_PLACES, rounding=ROUND_HALF_UP)


MEMBER_MONEY = ("owes", "fronted", "payments_made", "payments_received", "balance")
UNALLOCATED_MONEY = ("items", "tax", "tip", "fees", "total")


class SummaryCase(ApiTestCase, unittest.TestCase):
    def summary(self, client, split_id):
        r = client.get(f"/api/splits/{split_id}/summary")
        self.assertStatus(r, 200)
        self.assertSummaryShape(r.json)
        return r.json

    def assertSummaryShape(self, s):
        """The envelope, and every monetary field as a 4-decimal JSON string."""
        for field in ("split_id", "name", "base_currency"):
            self.assertIsInstance(s[field], str, field)
        self.assertIsInstance(s["mixed_currency"], bool)
        self.assertIsInstance(s["by_currency"], list)
        self.assertEqual(s["mixed_currency"], len(s["by_currency"]) > 1,
                         "mixed_currency must mean 'more than one currency entry'")
        for g in s["by_currency"]:
            self.assertIsInstance(g["currency"], str)
            # A count is a count: it is the one number here that is not money.
            self.assertIsInstance(g["bill_count"], int)
            self.assertMoneyString(g["total"], "by_currency[].total")
            for field in UNALLOCATED_MONEY:
                self.assertMoneyString(g["unallocated"][field], f"unallocated.{field}")
            for m in g["members"]:
                self.assertIsInstance(m["member_id"], str)
                self.assertIsInstance(m["name"], str)
                for field in MEMBER_MONEY:
                    self.assertMoneyString(m[field], f"members[].{field}")
            for t in g["settlements"]:
                self.assertMoneyString(t["amount"], "settlements[].amount")
                for field in ("from_member", "from_name", "to_member", "to_name"):
                    self.assertIsInstance(t[field], str, field)

    def group(self, s, currency):
        matches = [g for g in s["by_currency"] if g["currency"] == currency]
        self.assertEqual(len(matches), 1,
                         f"expected exactly one {currency} entry, got "
                         f"{[g['currency'] for g in s['by_currency']]}")
        return matches[0]

    def by_member(self, group):
        return {m["member_id"]: m for m in group["members"]}

    def assertBalanceIdentity(self, member):
        """balance = fronted + payments_made - owes - payments_received, exactly."""
        expected = (Decimal(member["fronted"]) + Decimal(member["payments_made"])
                    - Decimal(member["owes"]) - Decimal(member["payments_received"]))
        self.assertEqual(Decimal(member["balance"]), expected,
                         f"balance identity broken for {member['name']}")


class TestTwoPayers(SummaryCase):
    """Alice fronts one bill, Bob fronts another: the case plan.md's
    "everyone owes the payer" framing cannot express."""

    def build(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bob = c.make_member(split["id"], "Bob")

        # Dinner: $100 of items + $10 tax, Alice fronted, 60/40 Alice/Bob.
        dinner = c.make_bill(split["id"], store_name="Dinner", tax="10.00",
                             payer_member_id=alice["id"])
        feast = c.make_item(dinner["id"], name="Feast", price="100.00")
        r = c.allocate(dinner["id"], feast["id"], "ratio",
                       [{"member_id": alice["id"], "ratio": "60"},
                        {"member_id": bob["id"], "ratio": "40"}])
        self.assertStatus(r, 200)

        # Taxi: $50 of items, no tax, Bob fronted, 50/50.
        taxi = c.make_bill(split["id"], store_name="Taxi",
                           payer_member_id=bob["id"])
        ride = c.make_item(taxi["id"], name="Ride", price="50.00")
        r = c.allocate(taxi["id"], ride["id"], "ratio",
                       [{"member_id": alice["id"], "ratio": "50"},
                        {"member_id": bob["id"], "ratio": "50"}])
        self.assertStatus(r, 200)
        return c, split, alice, bob

    def test_balances_come_out_with_both_signs(self):
        c, split, alice, bob = self.build()
        g = self.group(self.summary(c, split["id"]), "USD")
        members = self.by_member(g)

        alice_owes = (Decimal("60.0000") + proportional("60", "100", "10")
                      + Decimal("25.0000"))
        bob_owes = (Decimal("40.0000") + proportional("40", "100", "10")
                    + Decimal("25.0000"))
        self.assertEqual(alice_owes, Decimal("91.0000"))
        self.assertEqual(bob_owes, Decimal("69.0000"))

        self.assertEqual(Decimal(members[alice["id"]]["owes"]), alice_owes)
        self.assertEqual(Decimal(members[bob["id"]]["owes"]), bob_owes)
        # fronted is the whole total of the bill they paid for, tax included.
        self.assertEqual(members[alice["id"]]["fronted"], "110.0000")
        self.assertEqual(members[bob["id"]]["fronted"], "50.0000")

        self.assertEqual(Decimal(members[alice["id"]]["balance"]),
                         Decimal("110.0000") - alice_owes)
        self.assertEqual(members[alice["id"]]["balance"], "19.0000")
        self.assertEqual(members[bob["id"]]["balance"], "-19.0000")

        signs = {m["balance"][0] == "-" for m in g["members"]
                 if Decimal(m["balance"]) != ZERO}
        self.assertEqual(signs, {True, False},
                         "a two-payer split must produce balances of both signs")
        for m in g["members"]:
            self.assertBalanceIdentity(m)

    def test_the_answer_is_not_who_owes_the_payer(self):
        """Bob's bill-by-bill debt to Alice is 44.0000; his balance is -19.0000
        because Alice owes him 25.0000 for the taxi. Asserting the two differ is
        what pins the balance model down."""
        c, split, alice, bob = self.build()
        g = self.group(self.summary(c, split["id"]), "USD")
        bob_row = self.by_member(g)[bob["id"]]

        owed_to_alice_on_dinner = Decimal("40.0000") + proportional("40", "100", "10")
        self.assertEqual(owed_to_alice_on_dinner, Decimal("44.0000"))
        self.assertNotEqual(Decimal(bob_row["balance"]), -owed_to_alice_on_dinner)
        self.assertEqual(Decimal(bob_row["balance"]),
                         -owed_to_alice_on_dinner + Decimal("25.0000"))

    def test_the_settlement_clears_the_debt_exactly(self):
        c, split, alice, bob = self.build()
        g = self.group(self.summary(c, split["id"]), "USD")
        self.assertEqual(len(g["settlements"]), 1)
        transfer = g["settlements"][0]
        self.assertEqual(transfer["from_member"], bob["id"])
        self.assertEqual(transfer["from_name"], "Bob")
        self.assertEqual(transfer["to_member"], alice["id"])
        self.assertEqual(transfer["to_name"], "Alice")
        # Exact: a settlement a hundredth of a cent off from the balance it
        # clears is a bug, so this is == and not a tolerance.
        self.assertEqual(Decimal(transfer["amount"]), Decimal("19.0000"))

    def test_the_split_total_is_every_bill_in_that_currency(self):
        c, split, alice, bob = self.build()
        g = self.group(self.summary(c, split["id"]), "USD")
        self.assertEqual(g["bill_count"], 2)
        self.assertEqual(Decimal(g["total"]), Decimal("110.0000") + Decimal("50.0000"))
        self.assertEqual(g["total"], "160.0000")
        self.assertEqual(g["unallocated"]["total"], "0.0000")

    def test_a_payment_moves_the_balance_without_changing_owes(self):
        c, split, alice, bob = self.build()
        r = c.post(f"/api/splits/{split['id']}/payments",
                   {"from_member": bob["id"], "to_member": alice["id"],
                    "amount": "4.00", "method": "venmo"})
        if r.status == 404:
            self.skipTest("payments endpoint not registered yet (separate slice)")
        self.assertStatus(r, 201)

        g = self.group(self.summary(c, split["id"]), "USD")
        members = self.by_member(g)
        # What each of them owes is untouched: a payment settles a debt, it does
        # not change what the debt was.
        self.assertEqual(members[alice["id"]]["owes"], "91.0000")
        self.assertEqual(members[bob["id"]]["owes"], "69.0000")
        self.assertEqual(members[bob["id"]]["payments_made"], "4.0000")
        self.assertEqual(members[alice["id"]]["payments_received"], "4.0000")
        self.assertEqual(members[alice["id"]]["balance"], "15.0000")
        self.assertEqual(members[bob["id"]]["balance"], "-15.0000")
        for m in g["members"]:
            self.assertBalanceIdentity(m)
        self.assertEqual(Decimal(g["settlements"][0]["amount"]), Decimal("15.0000"))


class TestMixedCurrency(SummaryCase):
    """Two currencies are two independent answers, never one combined one."""

    def usd_bill(self, c, split, payer, other):
        """$100 of groceries + $8 tax, fronted by `payer`, eaten by `other`."""
        bill = c.make_bill(split["id"], store_name="Safeway", currency="USD",
                           tax="8.00", payer_member_id=payer["id"])
        item = c.make_item(bill["id"], name="Groceries", price="100.00")
        self.assertStatus(c.allocate(bill["id"], item["id"], "ratio",
                                     [{"member_id": other["id"], "ratio": "100"}]), 200)
        return bill

    def eur_bill(self, c, split, payer, other):
        """EUR 30 of coffee + EUR 3 tax, fronted by `payer`, drunk by `other`."""
        bill = c.make_bill(split["id"], store_name="Cafe", currency="EUR",
                           tax="3.00", payer_member_id=payer["id"])
        item = c.make_item(bill["id"], name="Coffee", price="30.00")
        self.assertStatus(c.allocate(bill["id"], item["id"], "ratio",
                                     [{"member_id": other["id"], "ratio": "100"}]), 200)
        return bill

    def build(self, with_eur=True):
        c = new_user()
        split = c.make_split(currency="USD")
        alice = c.make_member(split["id"], "Alice")
        bob = c.make_member(split["id"], "Bob")
        self.usd_bill(c, split, payer=alice, other=bob)
        if with_eur:
            self.eur_bill(c, split, payer=bob, other=alice)
        return c, split, alice, bob

    def test_two_currencies_give_two_entries_and_nothing_is_summed(self):
        c, split, alice, bob = self.build()
        s = self.summary(c, split["id"])
        self.assertTrue(s["mixed_currency"])
        self.assertEqual(len(s["by_currency"]), 2)
        self.assertEqual({g["currency"] for g in s["by_currency"]}, {"USD", "EUR"})
        self.assertEqual(s["base_currency"], "USD",
                         "base_currency stays the split's own currency")

        usd, eur = self.group(s, "USD"), self.group(s, "EUR")
        self.assertEqual(usd["total"], "108.0000")
        self.assertEqual(eur["total"], "33.0000")
        self.assertEqual(usd["bill_count"], 1)
        self.assertEqual(eur["bill_count"], 1)

        # The number a combining implementation would produce appears nowhere.
        combined = Decimal("108.0000") + Decimal("33.0000")
        every_amount = [Decimal(g["total"]) for g in s["by_currency"]]
        for g in s["by_currency"]:
            every_amount += [Decimal(m[f]) for m in g["members"] for f in MEMBER_MONEY]
            every_amount += [Decimal(t["amount"]) for t in g["settlements"]]
        self.assertNotIn(combined, [abs(v) for v in every_amount],
                         "141.0000 can only come from adding USD to EUR")

    def test_each_entry_is_what_that_currency_alone_would_produce(self):
        """The strongest form of "independent": the USD numbers in a mixed split
        are identical to the USD numbers when the EUR bill does not exist."""
        mixed_c, mixed_split, m_alice, m_bob = self.build(with_eur=True)
        solo_c, solo_split, s_alice, s_bob = self.build(with_eur=False)

        # Keyed by name, and restricted to the members the test created: the
        # owner member is auto-named from the registering email, which differs
        # between the two splits and is not what this test is about.
        def by_name(group):
            return {m["name"]: {f: m[f] for f in MEMBER_MONEY}
                    for m in group["members"] if m["name"] in ("Alice", "Bob")}

        mixed_usd = self.group(self.summary(mixed_c, mixed_split["id"]), "USD")
        solo_usd = self.group(self.summary(solo_c, solo_split["id"]), "USD")

        self.assertEqual(by_name(mixed_usd), by_name(solo_usd))
        self.assertEqual(mixed_usd["total"], solo_usd["total"])
        self.assertEqual(mixed_usd["unallocated"], solo_usd["unallocated"])
        self.assertEqual([(t["from_name"], t["to_name"], t["amount"])
                          for t in mixed_usd["settlements"]],
                         [(t["from_name"], t["to_name"], t["amount"])
                          for t in solo_usd["settlements"]])

    def test_balances_point_opposite_ways_in_the_two_currencies(self):
        c, split, alice, bob = self.build()
        s = self.summary(c, split["id"])
        usd = self.by_member(self.group(s, "USD"))
        eur = self.by_member(self.group(s, "EUR"))

        # Alice fronted the USD bill and consumed the EUR one; Bob the reverse.
        self.assertEqual(usd[alice["id"]]["balance"], "108.0000")
        self.assertEqual(usd[bob["id"]]["balance"], "-108.0000")
        self.assertEqual(eur[alice["id"]]["balance"], "-33.0000")
        self.assertEqual(eur[bob["id"]]["balance"], "33.0000")

        # And the two settlements run in opposite directions, uncancelled.
        self.assertEqual(
            [(t["from_name"], t["to_name"], t["amount"])
             for t in self.group(s, "USD")["settlements"]],
            [("Bob", "Alice", "108.0000")])
        self.assertEqual(
            [(t["from_name"], t["to_name"], t["amount"])
             for t in self.group(s, "EUR")["settlements"]],
            [("Alice", "Bob", "33.0000")])

    def test_a_single_currency_split_has_exactly_one_entry(self):
        c, split, alice, bob = self.build(with_eur=False)
        s = self.summary(c, split["id"])
        self.assertEqual(len(s["by_currency"]), 1)
        self.assertFalse(s["mixed_currency"])


class TestNoBills(SummaryCase):
    def test_a_split_with_no_bills_has_an_empty_by_currency(self):
        c = new_user()
        split = c.make_split(currency="GBP")
        c.make_member(split["id"], "Solo")

        s = self.summary(c, split["id"])
        self.assertEqual(s["by_currency"], [],
                         "no bills means no currency has been used: not a "
                         "fabricated zero row for the split's own currency")
        self.assertFalse(s["mixed_currency"])
        self.assertEqual(s["base_currency"], "GBP")
        self.assertEqual(s["split_id"], split["id"])
        self.assertEqual(s["name"], split["name"])

    def test_a_bill_with_no_items_still_makes_an_entry(self):
        """Distinct from the case above: the bill exists, so its currency has
        been used, even though nobody owes anything for it."""
        c = new_user()
        split = c.make_split()
        c.make_bill(split["id"])
        s = self.summary(c, split["id"])
        self.assertEqual(len(s["by_currency"]), 1)
        self.assertEqual(s["by_currency"][0]["total"], "0.0000")


class TestZeroSubtotal(SummaryCase):
    """The division that must never reach Postgres."""

    def test_a_bill_of_nothing_but_tax_is_a_200_with_everything_unallocated(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        c.make_bill(split["id"], tax="5.00", tip="2.00", fees="1.00",
                    payer_member_id=alice["id"])

        s = self.summary(c, split["id"])
        g = self.group(s, "USD")
        self.assertEqual(g["total"], "8.0000")

        # Nobody's share can be derived from a zero subtotal, so nobody owes.
        for m in g["members"]:
            with self.subTest(member=m["name"]):
                self.assertEqual(m["owes"], "0.0000")
        # ... and the whole of tax, tip and fees lands in unallocated rather
        # than sitting in no bucket at all.
        self.assertEqual(g["unallocated"]["items"], "0.0000")
        self.assertEqual(g["unallocated"]["tax"], "5.0000")
        self.assertEqual(g["unallocated"]["tip"], "2.0000")
        self.assertEqual(g["unallocated"]["fees"], "1.0000")
        self.assertEqual(g["unallocated"]["total"], "8.0000")

        # Alice fronted it, so she is owed all of it, and there is nobody to
        # collect from: no debtor, therefore no settlement.
        self.assertEqual(self.by_member(g)[alice["id"]]["fronted"], "8.0000")
        self.assertEqual(self.by_member(g)[alice["id"]]["balance"], "8.0000")
        self.assertEqual(g["settlements"], [],
                         "with no debtor there is no transfer to suggest")

    def test_a_zero_subtotal_bill_does_not_disturb_the_other_bills(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        real = c.make_bill(split["id"], payer_member_id=alice["id"])
        item = c.make_item(real["id"], price="20.00")
        self.assertStatus(c.allocate(real["id"], item["id"], "ratio",
                                     [{"member_id": alice["id"], "ratio": "100"}]), 200)
        c.make_bill(split["id"], tax="4.00")

        g = self.group(self.summary(c, split["id"]), "USD")
        self.assertEqual(g["bill_count"], 2)
        self.assertEqual(Decimal(g["total"]), Decimal("24.0000"))
        self.assertEqual(self.by_member(g)[alice["id"]]["owes"], "20.0000")
        self.assertEqual(g["unallocated"]["tax"], "4.0000")


class TestTheDenominator(SummaryCase):
    """Same rule as the per-bill shares endpoint: the denominator is the bill's
    FULL subtotal, so an unassigned item carries its own tax instead of pushing
    it onto the people who are present."""

    def build(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bill = c.make_bill(split["id"], tax="10.00", payer_member_id=alice["id"])
        assigned = c.make_item(bill["id"], name="Alice's dinner", price="40.00")
        c.make_item(bill["id"], name="Nobody's lunch", price="60.00")
        self.assertStatus(c.allocate(bill["id"], assigned["id"], "ratio",
                                     [{"member_id": alice["id"], "ratio": "100"}]), 200)
        return c, split, alice

    def test_owes_uses_the_full_subtotal(self):
        c, split, alice = self.build()
        g = self.group(self.summary(c, split["id"]), "USD")

        correct = Decimal("40.0000") + proportional("40", "100", "10")  # 44.0000
        wrong = Decimal("40.0000") + proportional("40", "40", "10")     # 50.0000
        self.assertNotEqual(correct, wrong, "the fixture must distinguish them")

        owes = Decimal(self.by_member(g)[alice["id"]]["owes"])
        self.assertEqual(owes, correct)
        self.assertNotEqual(owes, wrong,
                            "tax was divided by the allocated subtotal: an "
                            "unassigned item's tax is being pushed onto the "
                            "people who are present")

    def test_the_unallocated_remainder_keeps_its_own_tax(self):
        c, split, alice = self.build()
        g = self.group(self.summary(c, split["id"]), "USD")
        self.assertEqual(g["unallocated"]["items"], "60.0000")
        self.assertEqual(Decimal(g["unallocated"]["tax"]),
                         proportional("60", "100", "10"))
        self.assertEqual(g["unallocated"]["tax"], "6.0000")
        self.assertEqual(g["unallocated"]["total"], "66.0000")

    def test_balances_are_not_forced_to_sum_to_zero(self):
        """Alice fronted 110.00 and owes 44.00, and nobody owes the other 66.00
        because nobody was assigned to it. The column must not tie, and no
        transfer may be invented to make it tie."""
        c, split, alice = self.build()
        g = self.group(self.summary(c, split["id"]), "USD")
        self.assertEqual(self.by_member(g)[alice["id"]]["balance"], "66.0000")
        total_balance = sum((Decimal(m["balance"]) for m in g["members"]), ZERO)
        self.assertEqual(total_balance, Decimal("66.0000"))
        self.assertEqual(total_balance, Decimal(g["unallocated"]["total"]),
                         "the gap is exactly the part nobody is on the hook for")
        self.assertEqual(g["settlements"], [],
                         "there is no debtor, so there is nothing to settle")


class TestMembersWithNoActivity(SummaryCase):
    def test_a_member_on_no_bill_still_appears_with_zeros(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bystander = c.make_member(split["id"], "Bystander")
        bill = c.make_bill(split["id"], payer_member_id=alice["id"])
        item = c.make_item(bill["id"], price="30.00")
        self.assertStatus(c.allocate(bill["id"], item["id"], "ratio",
                                     [{"member_id": alice["id"], "ratio": "100"}]), 200)

        g = self.group(self.summary(c, split["id"]), "USD")
        members = self.by_member(g)
        self.assertIn(bystander["id"], members,
                      "a member with no activity must still be listed")
        row = members[bystander["id"]]
        self.assertEqual(row["name"], "Bystander")
        for field in MEMBER_MONEY:
            self.assertEqual(row[field], "0.0000", field)
        self.assertNotIn(bystander["id"],
                         [t["from_member"] for t in g["settlements"]])

    def test_every_member_appears_in_every_currency_entry(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        for currency in ("USD", "EUR"):
            bill = c.make_bill(split["id"], currency=currency,
                               payer_member_id=alice["id"])
            c.make_item(bill["id"], price="10.00")

        s = self.summary(c, split["id"])
        ids = [set(self.by_member(g)) for g in s["by_currency"]]
        self.assertEqual(len(ids), 2)
        self.assertEqual(ids[0], ids[1],
                         "the member list is the split's, not the currency's")
        self.assertIn(alice["id"], ids[0])


class TestSettlements(SummaryCase):
    def test_the_largest_debtor_pays_the_largest_creditor(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bob = c.make_member(split["id"], "Bob")
        carol = c.make_member(split["id"], "Carol")

        # Alice fronts $90 of items consumed 1/3 by each of Bob and Carol and
        # 1/3 by herself: 30 each.
        bill = c.make_bill(split["id"], payer_member_id=alice["id"])
        item = c.make_item(bill["id"], price="90.00")
        self.assertStatus(c.allocate(bill["id"], item["id"], "amount",
                                     [{"member_id": alice["id"], "amount": "30.00"},
                                      {"member_id": bob["id"], "amount": "30.00"},
                                      {"member_id": carol["id"], "amount": "30.00"}]),
                          200)

        g = self.group(self.summary(c, split["id"]), "USD")
        members = self.by_member(g)
        self.assertEqual(members[alice["id"]]["balance"], "60.0000")
        self.assertEqual(members[bob["id"]]["balance"], "-30.0000")
        self.assertEqual(members[carol["id"]]["balance"], "-30.0000")

        self.assertEqual(len(g["settlements"]), 2)
        for t in g["settlements"]:
            with self.subTest(transfer=t):
                self.assertEqual(t["to_member"], alice["id"])
                self.assertEqual(t["amount"], "30.0000")
                self.assertNotEqual(t["from_member"], t["to_member"],
                                    "nobody settles with themselves")
                self.assertGreater(Decimal(t["amount"]), ZERO)
        self.assertEqual({t["from_member"] for t in g["settlements"]},
                         {bob["id"], carol["id"]})

    def test_no_settlement_exceeds_the_balance_it_clears(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bob = c.make_member(split["id"], "Bob")
        carol = c.make_member(split["id"], "Carol")
        # Two payers and an unassigned remainder, so the balances do not tie.
        first = c.make_bill(split["id"], tax="5.00", payer_member_id=alice["id"])
        item = c.make_item(first["id"], price="60.00")
        c.make_item(first["id"], price="40.00")  # nobody's
        self.assertStatus(c.allocate(first["id"], item["id"], "ratio",
                                     [{"member_id": bob["id"], "ratio": "50"},
                                      {"member_id": carol["id"], "ratio": "50"}]), 200)
        second = c.make_bill(split["id"], payer_member_id=bob["id"])
        ride = c.make_item(second["id"], price="20.00")
        self.assertStatus(c.allocate(second["id"], ride["id"], "ratio",
                                     [{"member_id": carol["id"], "ratio": "100"}]), 200)

        g = self.group(self.summary(c, split["id"]), "USD")
        members = self.by_member(g)
        for m in g["members"]:
            self.assertBalanceIdentity(m)

        sent = {}
        received = {}
        for t in g["settlements"]:
            sent[t["from_member"]] = sent.get(t["from_member"], ZERO) + Decimal(t["amount"])
            received[t["to_member"]] = received.get(t["to_member"], ZERO) + Decimal(t["amount"])
        for member_id, amount in sent.items():
            with self.subTest(member=members[member_id]["name"]):
                self.assertLessEqual(amount, -Decimal(members[member_id]["balance"]),
                                     "a debtor was asked to pay more than they owe")
        for member_id, amount in received.items():
            with self.subTest(member=members[member_id]["name"]):
                self.assertLessEqual(amount, Decimal(members[member_id]["balance"]),
                                     "a creditor was paid more than they are owed")

        # Nothing is invented to absorb the unallocated 40.00 + its tax: the
        # transfers total exactly what the debtors owe, not what the creditors
        # are owed.
        total_debt = sum((-Decimal(m["balance"]) for m in g["members"]
                          if Decimal(m["balance"]) < 0), ZERO)
        self.assertEqual(sum(sent.values(), ZERO), total_debt)
        self.assertLess(sum(sent.values(), ZERO),
                        sum((Decimal(m["balance"]) for m in g["members"]
                             if Decimal(m["balance"]) > 0), ZERO))

    def test_settlement_amounts_survive_a_hundredth_of_a_cent(self):
        """Three-way 33.3333% shares leave 0.3333-sized fractions. The suggested
        transfer must equal the balance exactly, not a rounded version of it."""
        c = new_user()
        split = c.make_split()
        people = [c.make_member(split["id"], n) for n in ("Alice", "Bob", "Carol")]
        bill = c.make_bill(split["id"], tax="1.00", payer_member_id=people[0]["id"])
        item = c.make_item(bill["id"], price="10.00")
        self.assertStatus(
            c.allocate(bill["id"], item["id"], "ratio",
                       [{"member_id": p["id"], "ratio": "33.3333"} for p in people]),
            200)

        g = self.group(self.summary(c, split["id"]), "USD")
        members = self.by_member(g)
        share = Decimal("3.3333") + proportional("3.3333", "10.00", "1.00")
        self.assertEqual(share, Decimal("3.6666"))
        for p in people[1:]:
            self.assertEqual(Decimal(members[p["id"]]["owes"]), share)
            self.assertEqual(Decimal(members[p["id"]]["balance"]), -share)

        by_from = {t["from_member"]: t for t in g["settlements"]}
        for p in people[1:]:
            with self.subTest(member=p["name"]):
                self.assertEqual(Decimal(by_from[p["id"]]["amount"]), share)
                self.assertEqual(by_from[p["id"]]["amount"], "3.6666")

    def test_nobody_owing_anybody_settles_nothing(self):
        c = new_user()
        split = c.make_split()
        alice = c.make_member(split["id"], "Alice")
        bill = c.make_bill(split["id"], payer_member_id=alice["id"])
        item = c.make_item(bill["id"], price="10.00")
        self.assertStatus(c.allocate(bill["id"], item["id"], "ratio",
                                     [{"member_id": alice["id"], "ratio": "100"}]), 200)

        g = self.group(self.summary(c, split["id"]), "USD")
        self.assertEqual(self.by_member(g)[alice["id"]]["balance"], "0.0000",
                         "the payer who ate the whole bill is square")
        self.assertEqual(g["settlements"], [])


class TestSummaryOwnership(SummaryCase):
    def test_another_users_split_is_404(self):
        owner, other = new_user(), new_user()
        split = owner.make_split()
        owner.make_bill(split["id"])
        self.assertError(other.get(f"/api/splits/{split['id']}/summary"), 404)
        # ... and the owner still sees it, so the 404 is about ownership and
        # not about the split being broken.
        self.assertStatus(owner.get(f"/api/splits/{split['id']}/summary"), 200)

    def test_a_missing_split_is_404(self):
        c = new_user()
        self.assertError(c.get(f"/api/splits/{MISSING_UUID}/summary"), 404)

    def test_unauthenticated_access_is_401(self):
        owner = new_user()
        split = owner.make_split()
        self.assertError(Client().get(f"/api/splits/{split['id']}/summary"), 401)

    def test_an_archived_split_still_summarizes(self):
        """A finished trip is exactly when someone wants the summary."""
        c = new_user()
        split = c.make_split()
        c.make_bill(split["id"])
        self.assertStatus(c.delete(f"/api/splits/{split['id']}"), 200)
        s = self.summary(c, split["id"])
        self.assertEqual(s["split_id"], split["id"])


class TestSummaryBadIds(SummaryCase):
    def test_bad_ids_never_500(self):
        c = new_user()
        for bad in BAD_IDS:
            with self.subTest(bad=bad):
                r = c.get(f"/api/splits/{path_id(bad)}/summary")
                self.assertLess(r.status, 500, f"{r}")
                self.assertIn(r.status, (400, 404), f"{r}")
                self.assertError(r, r.status)


if __name__ == "__main__":
    unittest.main()


class TestPaymentOnlyCurrency(ApiTestCase, unittest.TestCase):
    """A payment in a currency no bill uses must still appear.

    `currency` on a payment defaults to the *split's* currency, so a USD split
    whose bills are all in EUR produces exactly this: a recorded settlement
    belonging to no bill currency. Deriving the currency list from bills alone
    made that payment vanish from every balance — money the user entered,
    silently ignored.
    """

    def setUp(self):
        self.owner = new_user(display_name="Alice")
        self.split = self.owner.make_split(name="Payment currency", currency="USD")
        self.alice = self.owner.get(f"/api/splits/{self.split['id']}/members").json[0]
        self.bob = self.owner.make_member(self.split["id"], name="Bob")
        # Every bill is in EUR, so USD has no bills at all.
        bill = self.owner.make_bill(self.split["id"], currency="EUR",
                                    payer_member_id=self.alice["id"])
        item = self.owner.make_item(bill["id"], name="Tickets", price="30.00")
        self.owner.allocate(bill["id"], item["id"], "ratio",
                            [{"member_id": self.bob["id"], "ratio": "100"}])
        # ...and this payment defaults to the split's USD.
        r = self.owner.post(f"/api/splits/{self.split['id']}/payments", {
            "from_member": self.bob["id"], "to_member": self.alice["id"],
            "amount": "20.00",
        })
        self.assertStatus(r, 201)
        self.assertEqual(r.json["currency"], "USD", "fixture assumption broke: ")

    def summary(self):
        r = self.owner.get(f"/api/splits/{self.split['id']}/summary")
        self.assertStatus(r, 200)
        return r.json

    def test_the_payment_currency_gets_its_own_block(self):
        blocks = {b["currency"]: b for b in self.summary()["by_currency"]}
        self.assertIn("USD", blocks,
                      "a recorded USD payment produced no USD block — it is invisible")
        self.assertIn("EUR", blocks)

    def test_the_payment_only_block_reports_no_bills(self):
        usd = next(b for b in self.summary()["by_currency"] if b["currency"] == "USD")
        self.assertEqual(usd["bill_count"], 0)
        self.assertEqual(usd["total"], "0.0000")
        self.assertEqual(usd["unallocated"]["total"], "0.0000")

    def test_the_payment_moves_the_balances_in_that_block(self):
        usd = next(b for b in self.summary()["by_currency"] if b["currency"] == "USD")
        by_id = {m["member_id"]: m for m in usd["members"]}
        bob, alice = by_id[self.bob["id"]], by_id[self.alice["id"]]
        # Bob paid 20 and owes nothing in USD, so the split owes him 20.
        self.assertEqual(bob["payments_made"], "20.0000")
        self.assertEqual(bob["balance"], "20.0000")
        # Alice received it, and fronted nothing in USD.
        self.assertEqual(alice["payments_received"], "20.0000")
        self.assertEqual(alice["balance"], "-20.0000")

    def test_the_eur_block_is_untouched_by_the_usd_payment(self):
        eur = next(b for b in self.summary()["by_currency"] if b["currency"] == "EUR")
        for m in eur["members"]:
            self.assertEqual(m["payments_made"], "0.0000")
            self.assertEqual(m["payments_received"], "0.0000")
        self.assertTrue(self.summary()["mixed_currency"])
