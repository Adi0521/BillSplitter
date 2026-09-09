"""Allocations: the arithmetic, exactly.

This is where a careless rounding decision becomes a few cents nobody can
account for, so every expectation below is either a literal 4-decimal string or
a decimal.Decimal computation. A Python float is never used to derive an
expected amount: doing so would reintroduce in the test the very bug the string
money representation exists to prevent.

The rules being tested (docs/api.md, Phase 4):
  * a share is ROUND(line_total * ratio / 100, 4), computed in NUMERIC;
  * rounded shares need not sum to line_total, and the remainder is *reported*
    as `unallocated` rather than being handed to whoever sorts first;
  * over-allocation is a 400, under-allocation is fine;
  * PUT is a full replace, in one transaction.
"""
import unittest
from decimal import Decimal

from harness import ApiTestCase, BAD_IDS, MISSING_UUID, new_user

# BAD_IDS contains a space, which http.client refuses to put in a request line.
def path_id(raw):
    return raw.replace(" ", "%20")


class AllocationCase(ApiTestCase, unittest.TestCase):
    def scenario(self, price="10.00", quantity=None, members=3):
        """A split with `members` named members, one bill, one item."""
        c = new_user()
        split = c.make_split()
        people = [c.make_member(split["id"], name)
                  for name in ["Alice", "Bob", "Carol", "Dave", "Erin"][:members]]
        bill = c.make_bill(split["id"])
        item = c.make_item(bill["id"], price=price, quantity=quantity)
        return c, split, people, bill, item

    def assertSetMoney(self, s):
        for field in ("line_total", "allocated", "unallocated"):
            self.assertMoneyString(s[field], field)
        for a in s["allocations"]:
            self.assertMoneyString(a["share"], "share")
            if a["allocation_mode"] == "ratio":
                self.assertMoneyString(a["ratio"], "ratio")
                self.assertIsNone(a["amount"])
            else:
                self.assertMoneyString(a["amount"], "amount")
                self.assertIsNone(a["ratio"])
            self.assertTrue(a["member_name"], "member_name must be joined in")

    def shares_by_member(self, s):
        return {a["member_id"]: a["share"] for a in s["allocations"]}


class TestTheCanonicalRemainder(AllocationCase):
    """Three people, ten dollars, one hundredth of a cent left over."""

    def test_ten_dollars_three_ways_reports_the_remainder(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=3)
        r = c.allocate(bill["id"], item["id"], "ratio",
                       [{"member_id": p["id"], "ratio": "33.3333"} for p in people])
        self.assertStatus(r, 200)
        s = r.json
        self.assertSetMoney(s)

        self.assertEqual(s["bill_item_id"], item["id"])
        self.assertEqual(s["line_total"], "10.0000")
        self.assertEqual(s["mode"], "ratio")
        self.assertEqual(len(s["allocations"]), 3)

        shares = [a["share"] for a in s["allocations"]]
        self.assertEqual(shares, ["3.3333", "3.3333", "3.3333"])

        # The remainder is reported, not absorbed.
        self.assertEqual(s["allocated"], "9.9999")
        self.assertEqual(s["unallocated"], "0.0001")

        # ... and nobody was inflated to make the columns tie.
        self.assertEqual(set(shares), {"3.3333"},
                         "a share was adjusted to absorb the rounding remainder")
        self.assertEqual(sum((Decimal(x) for x in shares), Decimal("0")),
                         Decimal(s["allocated"]))
        self.assertLess(Decimal(s["allocated"]), Decimal(s["line_total"]))
        self.assertEqual(Decimal(s["line_total"]) - Decimal(s["allocated"]),
                         Decimal(s["unallocated"]))

    def test_the_remainder_survives_a_reread(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=3)
        c.allocate(bill["id"], item["id"], "ratio",
                   [{"member_id": p["id"], "ratio": "33.3333"} for p in people])
        s = c.get(f"/api/bills/{bill['id']}/items/{item['id']}/allocations").json
        self.assertSetMoney(s)
        self.assertEqual(s["allocated"], "9.9999")
        self.assertEqual(s["unallocated"], "0.0001")


class TestEvenSplit(AllocationCase):
    def test_ten_dollars_across_three_floors_to_cents(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=3)
        r = c.post(f"/api/bills/{bill['id']}/items/{item['id']}/even-split",
                   {"member_ids": [p["id"] for p in people]})
        self.assertStatus(r, 200)
        s = r.json
        self.assertSetMoney(s)

        self.assertEqual(s["mode"], "amount",
                         "an even split produces amount mode, not ratio")
        self.assertEqual([a["amount"] for a in s["allocations"]],
                         ["3.3300", "3.3300", "3.3300"])
        self.assertEqual([a["share"] for a in s["allocations"]],
                         ["3.3300", "3.3300", "3.3300"])
        # It floors to cents. 3.34 would over-allocate the line by a cent.
        for a in s["allocations"]:
            self.assertNotEqual(a["share"], "3.3400",
                                "an even split must floor, never round up")
        self.assertEqual(s["allocated"], "9.9900")
        self.assertEqual(s["unallocated"], "0.0100")
        self.assertEqual(Decimal(s["unallocated"]),
                         Decimal("10.00") - Decimal("3.33") * 3)

    def test_a_share_that_floors_to_zero_is_400(self):
        """One cent across three members: writing 0.00 rows would contradict
        the rule that a zero allocation is rejected."""
        c, split, people, bill, item = self.scenario(price="0.01", members=3)
        r = c.post(f"/api/bills/{bill['id']}/items/{item['id']}/even-split",
                   {"member_ids": [p["id"] for p in people]})
        self.assertError(r, 400)
        after = c.get(f"/api/bills/{bill['id']}/items/{item['id']}/allocations").json
        self.assertEqual(after["allocations"], [])

    def test_even_split_replaces_an_existing_set(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=3)
        c.allocate(bill["id"], item["id"], "ratio",
                   [{"member_id": people[0]["id"], "ratio": "100"}])
        r = c.post(f"/api/bills/{bill['id']}/items/{item['id']}/even-split",
                   {"member_ids": [p["id"] for p in people[:2]]})
        self.assertStatus(r, 200)
        self.assertEqual(len(r.json["allocations"]), 2)
        self.assertEqual([a["share"] for a in r.json["allocations"]],
                         ["5.0000", "5.0000"])

    def test_even_split_rejections(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=2)
        outsider = new_user()
        other_split = outsider.make_split()
        foreign = outsider.make_member(other_split["id"], "Foreign")
        url = f"/api/bills/{bill['id']}/items/{item['id']}/even-split"
        for body in [{"member_ids": []},
                     {"member_ids": [MISSING_UUID]},
                     {"member_ids": [foreign["id"]]},
                     {"member_ids": [people[0]["id"], people[0]["id"]]},
                     {"member_ids": ["not-a-uuid"]},
                     {"member_ids": "abc"},
                     {}]:
            with self.subTest(body=body):
                self.assertError(c.post(url, body), 400)


class TestRatioBounds(AllocationCase):
    def test_exactly_one_hundred_is_accepted(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=3)
        r = c.allocate(bill["id"], item["id"], "ratio",
                       [{"member_id": people[0]["id"], "ratio": "33.3334"},
                        {"member_id": people[1]["id"], "ratio": "33.3333"},
                        {"member_id": people[2]["id"], "ratio": "33.3333"}])
        self.assertStatus(r, 200)
        self.assertSetMoney(r.json)
        self.assertEqual(
            sum((Decimal(a["ratio"]) for a in r.json["allocations"]), Decimal("0")),
            Decimal("100.0000"))

    def test_one_ten_thousandth_over_one_hundred_is_400(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=2)
        r = c.allocate(bill["id"], item["id"], "ratio",
                       [{"member_id": people[0]["id"], "ratio": "50.0000"},
                        {"member_id": people[1]["id"], "ratio": "50.0001"}])
        self.assertError(r, 400)

    def test_a_single_ratio_above_one_hundred_is_400(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=1)
        for bad in ["100.0001", "101", "150", "999.9999"]:
            with self.subTest(ratio=bad):
                self.assertError(
                    c.allocate(bill["id"], item["id"], "ratio",
                               [{"member_id": people[0]["id"], "ratio": bad}]), 400)

    def test_under_one_hundred_is_allowed_and_reported(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=1)
        r = c.allocate(bill["id"], item["id"], "ratio",
                       [{"member_id": people[0]["id"], "ratio": "25"}])
        self.assertStatus(r, 200)
        self.assertSetMoney(r.json)
        self.assertEqual(r.json["allocated"], "2.5000")
        self.assertEqual(r.json["unallocated"], "7.5000")


class TestAmountBounds(AllocationCase):
    def test_amounts_summing_exactly_to_the_line_total_are_accepted(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=2)
        r = c.allocate(bill["id"], item["id"], "amount",
                       [{"member_id": people[0]["id"], "amount": "6.6667"},
                        {"member_id": people[1]["id"], "amount": "3.3333"}])
        self.assertStatus(r, 200)
        self.assertSetMoney(r.json)
        self.assertEqual(Decimal(r.json["allocated"]),
                         Decimal(r.json["line_total"]))
        self.assertEqual(r.json["allocated"], "10.0000")
        self.assertEqual(r.json["unallocated"], "0.0000")

    def test_one_hundredth_of_a_cent_over_the_line_total_is_400(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=2)
        r = c.allocate(bill["id"], item["id"], "amount",
                       [{"member_id": people[0]["id"], "amount": "6.6667"},
                        {"member_id": people[1]["id"], "amount": "3.3334"}])
        self.assertError(r, 400)

    def test_under_allocation_is_allowed(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=1)
        r = c.allocate(bill["id"], item["id"], "amount",
                       [{"member_id": people[0]["id"], "amount": "4"}])
        self.assertStatus(r, 200)
        self.assertEqual(r.json["allocated"], "4.0000")
        self.assertEqual(r.json["unallocated"], "6.0000")


class TestAllocationsDivideTheLineTotal(AllocationCase):
    """An allocation splits price * quantity, not the unit price."""

    def test_ratio_applies_to_the_line_total(self):
        c, split, people, bill, item = self.scenario(price="12.50", quantity=2,
                                                     members=2)
        self.assertEqual(item["line_total"], "25.0000")
        r = c.allocate(bill["id"], item["id"], "ratio",
                       [{"member_id": people[0]["id"], "ratio": "50"},
                        {"member_id": people[1]["id"], "ratio": "50"}])
        self.assertStatus(r, 200)
        self.assertEqual(r.json["line_total"], "25.0000")
        for a in r.json["allocations"]:
            self.assertEqual(Decimal(a["share"]),
                             Decimal("12.50") * 2 * Decimal("50") / 100)
            self.assertEqual(a["share"], "12.5000")
        self.assertEqual(r.json["allocated"], "25.0000")

    def test_an_amount_above_the_unit_price_but_within_the_line_is_accepted(self):
        c, split, people, bill, item = self.scenario(price="12.50", quantity=2,
                                                     members=1)
        r = c.allocate(bill["id"], item["id"], "amount",
                       [{"member_id": people[0]["id"], "amount": "20.00"}])
        self.assertStatus(r, 200, "the bound is the line total, not the unit price: ")
        self.assertEqual(r.json["allocated"], "20.0000")
        self.assertEqual(r.json["unallocated"], "5.0000")

    def test_an_amount_above_the_line_total_is_rejected(self):
        c, split, people, bill, item = self.scenario(price="12.50", quantity=2,
                                                     members=1)
        self.assertError(
            c.allocate(bill["id"], item["id"], "amount",
                       [{"member_id": people[0]["id"], "amount": "25.0001"}]), 400)


class TestFullReplace(AllocationCase):
    def test_put_replaces_the_whole_set(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=3)
        c.allocate(bill["id"], item["id"], "ratio",
                   [{"member_id": p["id"], "ratio": "33"} for p in people])
        r = c.allocate(bill["id"], item["id"], "ratio",
                       [{"member_id": people[0]["id"], "ratio": "60"}])
        self.assertStatus(r, 200)
        self.assertEqual(len(r.json["allocations"]), 1)
        self.assertEqual(r.json["allocations"][0]["member_id"], people[0]["id"])
        self.assertNotIn(people[1]["id"], self.shares_by_member(r.json))

    def test_an_empty_allocations_array_clears_the_item(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=3)
        c.allocate(bill["id"], item["id"], "ratio",
                   [{"member_id": p["id"], "ratio": "33"} for p in people])
        r = c.allocate(bill["id"], item["id"], "ratio", [])
        self.assertStatus(r, 200)
        self.assertEqual(r.json["allocations"], [])
        self.assertIsNone(r.json["mode"], "mode is null when there is nothing set")
        self.assertEqual(r.json["allocated"], "0.0000")
        self.assertEqual(r.json["unallocated"], r.json["line_total"])
        self.assertEqual(
            c.get(f"/api/bills/{bill['id']}/items/{item['id']}/allocations")
             .json["allocations"], [])

    def test_an_unallocated_item_reads_as_an_empty_set(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=1)
        r = c.get(f"/api/bills/{bill['id']}/items/{item['id']}/allocations")
        self.assertStatus(r, 200)
        self.assertSetMoney(r.json)
        self.assertEqual(r.json["allocations"], [])
        self.assertIsNone(r.json["mode"])
        self.assertEqual(r.json["allocated"], "0.0000")
        self.assertEqual(r.json["unallocated"], "10.0000")


class TestAtomicity(AllocationCase):
    """A rejected PUT must leave the saved set exactly as it was — same rows,
    same ids. Deleting first and validating later would destroy a good set on
    the way to reporting a bad one."""

    def test_a_rejected_put_leaves_the_saved_set_untouched(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=3)
        url = f"/api/bills/{bill['id']}/items/{item['id']}/allocations"

        saved = c.allocate(bill["id"], item["id"], "ratio",
                           [{"member_id": people[0]["id"], "ratio": "40"},
                            {"member_id": people[1]["id"], "ratio": "35"}]).json
        self.assertEqual(len(saved["allocations"]), 2)
        before = c.get(url).json

        rejected = [
            # over-allocated
            ("ratio", [{"member_id": people[0]["id"], "ratio": "60"},
                       {"member_id": people[1]["id"], "ratio": "60"}]),
            # duplicate member
            ("ratio", [{"member_id": people[0]["id"], "ratio": "10"},
                       {"member_id": people[0]["id"], "ratio": "10"}]),
            # a member from nowhere
            ("ratio", [{"member_id": MISSING_UUID, "ratio": "10"}]),
            # a zero share
            ("ratio", [{"member_id": people[2]["id"], "ratio": "0"}]),
            # mixed modes
            ("amount", [{"member_id": people[2]["id"], "ratio": "10"}]),
        ]
        for mode, allocs in rejected:
            with self.subTest(mode=mode, allocations=allocs):
                self.assertError(
                    c.allocate(bill["id"], item["id"], mode, allocs), 400)
                after = c.get(url).json
                self.assertEqual(after, before,
                                 "a rejected PUT changed the stored set")
                self.assertEqual([a["id"] for a in after["allocations"]],
                                 [a["id"] for a in before["allocations"]],
                                 "the original rows were replaced, not preserved")


class TestAllocationRejections(AllocationCase):
    def setUp(self):
        self.c, self.split, self.people, self.bill, self.item = \
            self.scenario(price="10.00", members=3)
        self.url = (f"/api/bills/{self.bill['id']}/items/{self.item['id']}"
                    "/allocations")

    def put(self, body):
        return self.c.put(self.url, body)

    def test_mixed_modes_are_rejected(self):
        a, b = self.people[0]["id"], self.people[1]["id"]
        for body in [
            {"mode": "ratio", "allocations": [{"member_id": a, "ratio": "50"},
                                              {"member_id": b, "amount": "5.00"}]},
            {"mode": "amount", "allocations": [{"member_id": a, "amount": "5.00"},
                                               {"member_id": b, "ratio": "50"}]},
            {"mode": "ratio", "allocations": [
                {"member_id": a, "ratio": "50", "allocation_mode": "amount"}]},
            {"mode": "ratio", "allocations": [
                {"member_id": a, "ratio": "50", "amount": "5.00"}]},
        ]:
            with self.subTest(body=body):
                self.assertError(self.put(body), 400)

    def test_duplicate_member_id_is_rejected(self):
        a = self.people[0]["id"]
        self.assertError(self.put({"mode": "ratio", "allocations": [
            {"member_id": a, "ratio": "10"}, {"member_id": a, "ratio": "20"}]}), 400)

    def test_zero_and_negative_values_are_rejected(self):
        a = self.people[0]["id"]
        for mode, value in [("ratio", "0"), ("ratio", "0.0000"), ("ratio", "-10"),
                            ("amount", "0"), ("amount", "0.0000"),
                            ("amount", "-5.00")]:
            with self.subTest(mode=mode, value=value):
                self.assertError(self.put({"mode": mode, "allocations": [
                    {"member_id": a, mode: value}]}), 400)

    def test_five_decimal_places_are_rejected(self):
        a = self.people[0]["id"]
        for mode, value in [("ratio", "33.33333"), ("amount", "1.00001")]:
            with self.subTest(mode=mode, value=value):
                self.assertError(self.put({"mode": mode, "allocations": [
                    {"member_id": a, mode: value}]}), 400)

    def test_non_decimal_values_are_rejected(self):
        a = self.people[0]["id"]
        for value in ["12,50", "1e2", "abc", "", "  ", "5%"]:
            with self.subTest(ratio=value):
                self.assertError(self.put({"mode": "ratio", "allocations": [
                    {"member_id": a, "ratio": value}]}), 400)

    def test_a_member_from_another_split_is_400_not_404(self):
        """The item was found; the body is what is wrong."""
        other = new_user()
        foreign = other.make_member(other.make_split()["id"], "Foreign")
        self.assertError(self.put({"mode": "ratio", "allocations": [
            {"member_id": foreign["id"], "ratio": "50"}]}), 400)

        # Same owner, different split: still not a member of this bill's split.
        sibling = self.c.make_member(self.c.make_split()["id"], "Sibling")
        self.assertError(self.put({"mode": "ratio", "allocations": [
            {"member_id": sibling["id"], "ratio": "50"}]}), 400)

    def test_a_member_that_does_not_exist_is_400(self):
        self.assertError(self.put({"mode": "ratio", "allocations": [
            {"member_id": MISSING_UUID, "ratio": "50"}]}), 400)

    def test_a_malformed_member_id_is_400(self):
        self.assertError(self.put({"mode": "ratio", "allocations": [
            {"member_id": "not-a-uuid", "ratio": "50"}]}), 400)

    def test_a_missing_or_unknown_mode_is_400(self):
        a = self.people[0]["id"]
        for body in [{"allocations": [{"member_id": a, "ratio": "50"}]},
                     {"mode": None, "allocations": []},
                     {"mode": "percent", "allocations": []},
                     {"mode": "ratio"},
                     {"mode": "ratio", "allocations": {}},
                     {"mode": "ratio", "allocations": [{"member_id": a}]},
                     {"mode": "amount", "allocations": [{"ratio": "50"}]}]:
            with self.subTest(body=body):
                self.assertError(self.put(body), 400)

    def test_a_client_supplied_share_is_rejected(self):
        a = self.people[0]["id"]
        self.assertError(self.put({"mode": "ratio", "allocations": [
            {"member_id": a, "ratio": "50", "share": "99.00"}]}), 400)


class TestAllocationOwnership(AllocationCase):
    def test_another_users_item_is_404(self):
        owner, other = new_user(), new_user()
        split = owner.make_split()
        member = owner.make_member(split["id"], "Alice")
        bill = owner.make_bill(split["id"])
        item = owner.make_item(bill["id"], price="10.00")
        url = f"/api/bills/{bill['id']}/items/{item['id']}/allocations"

        self.assertError(other.get(url), 404)
        self.assertError(other.put(url, {"mode": "ratio", "allocations": [
            {"member_id": member["id"], "ratio": "100"}]}), 404)
        self.assertError(other.post(
            f"/api/bills/{bill['id']}/items/{item['id']}/even-split",
            {"member_ids": [member["id"]]}), 404)

    def test_an_item_under_the_wrong_bill_is_404(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=1)
        other_bill = c.make_bill(split["id"], store_name="Other")
        url = f"/api/bills/{other_bill['id']}/items/{item['id']}/allocations"
        self.assertError(c.get(url), 404)
        self.assertError(c.put(url, {"mode": "ratio", "allocations": [
            {"member_id": people[0]["id"], "ratio": "100"}]}), 404)

    def test_unauthenticated_access_is_401(self):
        from harness import Client
        c, split, people, bill, item = self.scenario(price="10.00", members=1)
        url = f"/api/bills/{bill['id']}/items/{item['id']}/allocations"
        anon = Client()
        self.assertError(anon.get(url), 401)
        self.assertError(anon.put(url, {"mode": "ratio", "allocations": []}), 401)


class TestAllocationBadIds(AllocationCase):
    def test_bad_ids_never_500(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=1)
        good_body = {"mode": "ratio",
                     "allocations": [{"member_id": people[0]["id"], "ratio": "50"}]}
        for bad in BAD_IDS:
            with self.subTest(bad=bad):
                paths = [
                    f"/api/bills/{path_id(bad)}/items/{item['id']}/allocations",
                    f"/api/bills/{bill['id']}/items/{path_id(bad)}/allocations",
                ]
                for p in paths:
                    for resp in (c.get(p), c.put(p, good_body)):
                        self.assertLess(resp.status, 500, f"{resp}")
                        self.assertIn(resp.status, (400, 404), f"{resp}")
                    even = c.post(p.replace("/allocations", "/even-split"),
                                  {"member_ids": [people[0]["id"]]})
                    self.assertLess(even.status, 500, f"{even}")

    def test_malformed_json_body_is_400_not_500(self):
        c, split, people, bill, item = self.scenario(price="10.00", members=1)
        url = f"/api/bills/{bill['id']}/items/{item['id']}/allocations"
        r = c.request("PUT", url, None)
        self.assertLess(r.status, 500, f"{r}")


if __name__ == "__main__":
    unittest.main()
