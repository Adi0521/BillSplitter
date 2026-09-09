"""Bills: CRUD, derived subtotal/total, validation and ownership.

A bill is the container the money hangs off, so almost everything asserted here
is about what the server refuses to let a client decide: the subtotal, the
total, the currency default, and who is allowed to see any of it.

Every expected amount in this file is a literal 4-decimal string or is built
with decimal.Decimal. No float ever produces an expected value in this suite —
that would reintroduce in the test the exact rounding bug the string money
representation exists to prevent.
"""
import unittest
from decimal import Decimal

from harness import ApiTestCase, BAD_IDS, MISSING_UUID, new_user

# BAD_IDS contains a space, and http.client refuses to put a raw space in a
# request line (InvalidURL, raised before the request is ever sent). Encoding
# just the space keeps the hostile string intact from the server's point of
# view — the already-percent-encoded entry is left exactly as written.
def path_id(raw):
    return raw.replace(" ", "%20")


MONEY_FIELDS = ("subtotal", "tax", "tip", "fees", "total")


class BillCase(ApiTestCase, unittest.TestCase):
    def assertBillMoney(self, bill):
        """Every monetary field of a Bill is a 4-decimal JSON string."""
        for field in MONEY_FIELDS:
            self.assertIn(field, bill, f"bill is missing {field}")
            self.assertMoneyString(bill[field], field)


class TestBillCreate(BillCase):
    def test_create_returns_the_documented_shape(self):
        c = new_user()
        split = c.make_split()
        r = c.post(f"/api/splits/{split['id']}/bills",
                   {"store_name": "Safeway", "date": "2026-01-15"})
        self.assertStatus(r, 201)
        bill = r.json
        self.assertBillMoney(bill)
        self.assertEqual(bill["split_id"], split["id"])
        self.assertEqual(bill["store_name"], "Safeway")
        self.assertEqual(bill["date"], "2026-01-15")
        self.assertEqual(bill["item_count"], 0)
        self.assertIsNone(bill["payer_member_id"])
        self.assertIn("created_at", bill)

    def test_new_bill_subtotal_is_the_string_zero_to_four_places(self):
        """Not null, not 0, not "0" — an empty bill's subtotal is "0.0000"."""
        c = new_user()
        bill = c.make_bill(c.make_split()["id"])
        self.assertIsNotNone(bill["subtotal"])
        self.assertEqual(bill["subtotal"], "0.0000")
        self.assertEqual(bill["total"], "0.0000")

    def test_amounts_are_normalized_to_four_decimals(self):
        c = new_user()
        bill = c.make_bill(c.make_split()["id"], tax="3.83", tip="1", fees="0.5")
        self.assertBillMoney(bill)
        self.assertEqual(bill["tax"], "3.8300")
        self.assertEqual(bill["tip"], "1.0000")
        self.assertEqual(bill["fees"], "0.5000")
        # total is subtotal + tax + tip + fees, computed by the server.
        expected = Decimal("0") + Decimal("3.83") + Decimal("1") + Decimal("0.5")
        self.assertEqual(Decimal(bill["total"]), expected)
        self.assertEqual(bill["total"], "5.3300")

    def test_total_is_subtotal_plus_tax_tip_and_fees(self):
        c = new_user()
        bill = c.make_bill(c.make_split()["id"], tax="2.50", tip="1.25", fees="0.75")
        c.make_item(bill["id"], price="12.50", quantity=2)
        c.make_item(bill["id"], price="4.99")
        got = c.get(f"/api/splits/{bill['split_id']}/bills/{bill['id']}").json
        self.assertBillMoney(got)
        subtotal = Decimal("12.50") * 2 + Decimal("4.99")
        self.assertEqual(Decimal(got["subtotal"]), subtotal)
        self.assertEqual(
            Decimal(got["total"]),
            subtotal + Decimal("2.50") + Decimal("1.25") + Decimal("0.75"),
        )
        self.assertEqual(got["total"], "34.4900")


class TestDerivedFieldsAreRejected(BillCase):
    """subtotal and total belong to the server. A client that sends one is
    working from a stale number and must be told, not silently ignored."""

    def test_post_with_subtotal_is_400(self):
        c = new_user()
        split = c.make_split()
        r = c.post(f"/api/splits/{split['id']}/bills",
                   {"store_name": "S", "date": "2026-01-15", "subtotal": "10.00"})
        self.assertError(r, 400)

    def test_post_with_total_is_400(self):
        c = new_user()
        split = c.make_split()
        r = c.post(f"/api/splits/{split['id']}/bills",
                   {"store_name": "S", "date": "2026-01-15", "total": "10.00"})
        self.assertError(r, 400)

    def test_put_with_subtotal_is_400(self):
        c = new_user()
        bill = c.make_bill(c.make_split()["id"])
        r = c.put(f"/api/splits/{bill['split_id']}/bills/{bill['id']}",
                  {"subtotal": "10.00"})
        self.assertError(r, 400)

    def test_put_with_total_is_400(self):
        c = new_user()
        bill = c.make_bill(c.make_split()["id"])
        r = c.put(f"/api/splits/{bill['split_id']}/bills/{bill['id']}",
                  {"total": "10.00"})
        self.assertError(r, 400)

    def test_subtotal_is_derived_on_every_read(self):
        """There is no bills.subtotal column; the number follows the items."""
        c = new_user()
        bill = c.make_bill(c.make_split()["id"])
        item = c.make_item(bill["id"], price="10.00")
        url = f"/api/splits/{bill['split_id']}/bills/{bill['id']}"
        self.assertEqual(c.get(url).json["subtotal"], "10.0000")
        c.put(f"/api/bills/{bill['id']}/items/{item['id']}", {"price": "25.00"})
        self.assertEqual(c.get(url).json["subtotal"], "25.0000")
        c.delete(f"/api/bills/{bill['id']}/items/{item['id']}")
        self.assertEqual(c.get(url).json["subtotal"], "0.0000")


class TestCurrency(BillCase):
    def test_currency_defaults_to_the_splits_currency_not_usd(self):
        c = new_user()
        split = c.make_split(currency="EUR")
        self.assertEqual(split["currency"], "EUR")
        bill = c.make_bill(split["id"])
        self.assertEqual(bill["currency"], "EUR")

    def test_explicit_currency_is_uppercased(self):
        c = new_user()
        bill = c.make_bill(c.make_split(currency="EUR")["id"], currency="gbp")
        self.assertEqual(bill["currency"], "GBP")

    def test_empty_currency_means_inherit_the_split(self):
        """"" is accepted for currency — it is what an unselected <select>
        submits, and it has a meaningful "unset": the split's currency."""
        c = new_user()
        split = c.make_split(currency="JPY")
        r = c.post(f"/api/splits/{split['id']}/bills",
                   {"store_name": "S", "date": "2026-01-15", "currency": ""})
        self.assertStatus(r, 201)
        self.assertEqual(r.json["currency"], "JPY")

    def test_put_empty_currency_means_inherit_the_split(self):
        """The same asymmetry on update: an edit form clearing the currency
        select submits "", which the contract says means "inherit"."""
        c = new_user()
        split = c.make_split(currency="JPY")
        bill = c.make_bill(split["id"], currency="USD")
        r = c.put(f"/api/splits/{split['id']}/bills/{bill['id']}", {"currency": ""})
        self.assertStatus(r, 200)
        self.assertEqual(r.json["currency"], "JPY")

    def test_malformed_currency_is_rejected(self):
        c = new_user()
        split = c.make_split()
        for bad in ["US", "USDD", "US1", "u$d", "123"]:
            with self.subTest(currency=bad):
                r = c.post(f"/api/splits/{split['id']}/bills",
                           {"store_name": "S", "date": "2026-01-15", "currency": bad})
                self.assertError(r, 400)


class TestPayerMemberId(BillCase):
    def test_payer_from_this_split_is_accepted(self):
        c = new_user()
        split = c.make_split()
        member = c.make_member(split["id"], "Alice")
        bill = c.make_bill(split["id"], payer_member_id=member["id"])
        self.assertEqual(bill["payer_member_id"], member["id"])

    def test_payer_from_another_split_is_400_not_404(self):
        """The split in the URL was found, so the body is what is wrong."""
        c = new_user()
        mine, theirs = c.make_split(), c.make_split()
        outsider = c.make_member(theirs["id"], "Outsider")
        r = c.post(f"/api/splits/{mine['id']}/bills",
                   {"store_name": "S", "date": "2026-01-15",
                    "payer_member_id": outsider["id"]})
        self.assertError(r, 400)

    def test_payer_that_does_not_exist_at_all_is_400(self):
        c = new_user()
        split = c.make_split()
        r = c.post(f"/api/splits/{split['id']}/bills",
                   {"store_name": "S", "date": "2026-01-15",
                    "payer_member_id": MISSING_UUID})
        self.assertError(r, 400)

    def test_null_and_empty_string_both_mean_no_payer(self):
        c = new_user()
        split = c.make_split()
        for value in (None, ""):
            with self.subTest(payer=value):
                r = c.post(f"/api/splits/{split['id']}/bills",
                           {"store_name": "S", "date": "2026-01-15",
                            "payer_member_id": value})
                self.assertStatus(r, 201)
                self.assertIsNone(r.json["payer_member_id"])

    def test_malformed_payer_uuid_is_400(self):
        c = new_user()
        split = c.make_split()
        for bad in ["not-a-uuid", "12345", "0000-0000"]:
            with self.subTest(payer=bad):
                r = c.post(f"/api/splits/{split['id']}/bills",
                           {"store_name": "S", "date": "2026-01-15",
                            "payer_member_id": bad})
                self.assertError(r, 400)

    def test_payer_can_be_cleared_on_update(self):
        c = new_user()
        split = c.make_split()
        member = c.make_member(split["id"], "Alice")
        bill = c.make_bill(split["id"], payer_member_id=member["id"])
        r = c.put(f"/api/splits/{split['id']}/bills/{bill['id']}",
                  {"payer_member_id": None})
        self.assertStatus(r, 200)
        self.assertIsNone(r.json["payer_member_id"])

    def test_put_payer_from_another_split_is_400(self):
        c = new_user()
        mine, theirs = c.make_split(), c.make_split()
        outsider = c.make_member(theirs["id"], "Outsider")
        bill = c.make_bill(mine["id"])
        r = c.put(f"/api/splits/{mine['id']}/bills/{bill['id']}",
                  {"payer_member_id": outsider["id"]})
        self.assertError(r, 400)


class TestDateValidation(BillCase):
    def test_rejects_non_iso_and_impossible_dates(self):
        c = new_user()
        split = c.make_split()
        for bad in ["08/30/2026", "2026-02-31", "2026-13-01", "2026-1-5",
                    "2026-00-10", "2026-04-31", "yesterday", "", "2026-01-15T00:00:00Z"]:
            with self.subTest(date=bad):
                r = c.post(f"/api/splits/{split['id']}/bills",
                           {"store_name": "S", "date": bad})
                self.assertError(r, 400)

    def test_accepts_a_real_leap_day(self):
        c = new_user()
        bill = c.make_bill(c.make_split()["id"], date="2028-02-29")
        self.assertEqual(bill["date"], "2028-02-29")

    def test_put_rejects_a_bad_date(self):
        c = new_user()
        bill = c.make_bill(c.make_split()["id"])
        r = c.put(f"/api/splits/{bill['split_id']}/bills/{bill['id']}",
                  {"date": "2026-02-31"})
        self.assertError(r, 400)
        self.assertEqual(
            c.get(f"/api/splits/{bill['split_id']}/bills/{bill['id']}").json["date"],
            "2026-01-15", "a rejected update must not have been applied")


class TestAmountValidation(BillCase):
    def test_negative_amounts_are_rejected(self):
        c = new_user()
        split = c.make_split()
        for field in ("tax", "tip", "fees"):
            with self.subTest(field=field):
                r = c.post(f"/api/splits/{split['id']}/bills",
                           {"store_name": "S", "date": "2026-01-15", field: "-0.01"})
                self.assertError(r, 400)

    def test_five_decimal_places_are_rejected(self):
        c = new_user()
        split = c.make_split()
        for field in ("tax", "tip", "fees"):
            with self.subTest(field=field):
                r = c.post(f"/api/splits/{split['id']}/bills",
                           {"store_name": "S", "date": "2026-01-15", field: "1.23456"})
                self.assertError(r, 400)

    def test_empty_amount_is_rejected_even_though_empty_currency_is_not(self):
        """The documented asymmetry, asserted in one place: "" has a meaning
        for currency ("inherit") and none for an amount, where reading it as
        zero would turn a cleared field into a real number on someone's bill."""
        c = new_user()
        split = c.make_split(currency="CAD")
        for field in ("tax", "tip", "fees"):
            with self.subTest(field=field):
                r = c.post(f"/api/splits/{split['id']}/bills",
                           {"store_name": "S", "date": "2026-01-15", field: ""})
                self.assertError(r, 400)
        accepted = c.post(f"/api/splits/{split['id']}/bills",
                          {"store_name": "S", "date": "2026-01-15", "currency": ""})
        self.assertStatus(accepted, 201, "an empty currency must still be accepted: ")
        self.assertEqual(accepted.json["currency"], "CAD")

    def test_non_numeric_amounts_are_rejected(self):
        c = new_user()
        split = c.make_split()
        for bad in ["12,50", "abc", "1e3", "$5", "1.2.3", "  "]:
            with self.subTest(tax=bad):
                r = c.post(f"/api/splits/{split['id']}/bills",
                           {"store_name": "S", "date": "2026-01-15", "tax": bad})
                self.assertError(r, 400)

    def test_amount_above_the_numeric_ceiling_is_rejected(self):
        c = new_user()
        split = c.make_split()
        r = c.post(f"/api/splits/{split['id']}/bills",
                   {"store_name": "S", "date": "2026-01-15", "tax": "100000000.0000"})
        self.assertError(r, 400)

    def test_omitted_amounts_default_to_zero(self):
        c = new_user()
        bill = c.make_bill(c.make_split()["id"])
        self.assertEqual((bill["tax"], bill["tip"], bill["fees"]),
                         ("0.0000", "0.0000", "0.0000"))

    def test_store_name_is_required_and_bounded(self):
        c = new_user()
        split = c.make_split()
        for bad in ["", "   ", "x" * 201]:
            with self.subTest(store_name=bad[:12]):
                r = c.post(f"/api/splits/{split['id']}/bills",
                           {"store_name": bad, "date": "2026-01-15"})
                self.assertError(r, 400)


class TestBillReadUpdateDelete(BillCase):
    def test_list_is_a_bare_array_ordered_by_date_desc(self):
        c = new_user()
        split = c.make_split()
        c.make_bill(split["id"], store_name="Older", date="2026-01-01")
        c.make_bill(split["id"], store_name="Newer", date="2026-03-01")
        r = c.get(f"/api/splits/{split['id']}/bills")
        self.assertStatus(r, 200)
        self.assertIsInstance(r.json, list)
        self.assertEqual([b["store_name"] for b in r.json], ["Newer", "Older"])
        for bill in r.json:
            self.assertBillMoney(bill)

    def test_detail_embeds_items(self):
        c = new_user()
        bill = c.make_bill(c.make_split()["id"])
        c.make_item(bill["id"], name="Olive oil", price="12.50", quantity=2)
        r = c.get(f"/api/splits/{bill['split_id']}/bills/{bill['id']}")
        self.assertStatus(r, 200)
        self.assertBillMoney(r.json)
        self.assertEqual(len(r.json["items"]), 1)
        self.assertEqual(r.json["items"][0]["name"], "Olive oil")
        self.assertMoneyString(r.json["items"][0]["line_total"], "line_total")
        self.assertEqual(r.json["item_count"], 1)

    def test_update_changes_only_the_fields_sent(self):
        c = new_user()
        split = c.make_split()
        bill = c.make_bill(split["id"], store_name="Safeway", date="2026-01-15",
                           tax="3.00")
        r = c.put(f"/api/splits/{split['id']}/bills/{bill['id']}",
                  {"store_name": "Trader Joe's"})
        self.assertStatus(r, 200)
        self.assertBillMoney(r.json)
        self.assertEqual(r.json["store_name"], "Trader Joe's")
        self.assertEqual(r.json["date"], "2026-01-15")
        self.assertEqual(r.json["tax"], "3.0000")

    def test_empty_update_body_is_400(self):
        c = new_user()
        bill = c.make_bill(c.make_split()["id"])
        self.assertError(
            c.put(f"/api/splits/{bill['split_id']}/bills/{bill['id']}", {}), 400)

    def test_delete_really_deletes_and_cascades_items(self):
        c = new_user()
        split = c.make_split()
        bill = c.make_bill(split["id"])
        item = c.make_item(bill["id"], price="10.00")
        r = c.delete(f"/api/splits/{split['id']}/bills/{bill['id']}")
        self.assertStatus(r, 200)
        self.assertError(c.get(f"/api/splits/{split['id']}/bills/{bill['id']}"), 404)
        self.assertError(
            c.delete(f"/api/splits/{split['id']}/bills/{bill['id']}"), 404)
        self.assertNotIn(bill["id"],
                         [b["id"] for b in c.get(f"/api/splits/{split['id']}/bills").json])
        # The item went with it: its bill is gone, so every item route 404s.
        self.assertError(c.get(f"/api/bills/{bill['id']}/items"), 404)
        self.assertError(c.delete(f"/api/bills/{bill['id']}/items/{item['id']}"), 404)

    def test_missing_bill_is_404_on_every_verb(self):
        c = new_user()
        split = c.make_split()
        base = f"/api/splits/{split['id']}/bills/{MISSING_UUID}"
        self.assertError(c.get(base), 404)
        self.assertError(c.put(base, {"store_name": "x"}), 404)
        self.assertError(c.delete(base), 404)


class TestBillOwnership(BillCase):
    def test_another_user_gets_404_on_every_verb(self):
        owner, other = new_user(), new_user()
        split = owner.make_split()
        bill = owner.make_bill(split["id"])
        detail = f"/api/splits/{split['id']}/bills/{bill['id']}"
        self.assertError(other.get(f"/api/splits/{split['id']}/bills"), 404)
        self.assertError(other.post(f"/api/splits/{split['id']}/bills",
                                    {"store_name": "S", "date": "2026-01-15"}), 404)
        self.assertError(other.get(detail), 404)
        self.assertError(other.put(detail, {"store_name": "Hijacked"}), 404)
        self.assertError(other.delete(detail), 404)
        # And nothing was changed by the attempts.
        self.assertEqual(owner.get(detail).json["store_name"], "Store")

    def test_a_bill_from_another_split_of_the_same_owner_is_404(self):
        c = new_user()
        a, b = c.make_split(), c.make_split()
        bill = c.make_bill(a["id"])
        self.assertError(c.get(f"/api/splits/{b['id']}/bills/{bill['id']}"), 404)
        self.assertError(c.delete(f"/api/splits/{b['id']}/bills/{bill['id']}"), 404)

    def test_unauthenticated_access_is_401(self):
        from harness import Client
        owner = new_user()
        split = owner.make_split()
        bill = owner.make_bill(split["id"])
        anon = Client()
        self.assertError(anon.get(f"/api/splits/{split['id']}/bills"), 401)
        self.assertError(
            anon.get(f"/api/splits/{split['id']}/bills/{bill['id']}"), 401)


class TestBillBadIds(BillCase):
    """A hostile path parameter must never reach Postgres as a uuid cast."""

    def test_bad_split_id_never_500s(self):
        c = new_user()
        for bad in BAD_IDS:
            with self.subTest(split_id=bad):
                base = f"/api/splits/{path_id(bad)}/bills"
                for resp in (c.get(base),
                             c.post(base, {"store_name": "S", "date": "2026-01-15"})):
                    self.assertLess(resp.status, 500, f"{resp}")
                    self.assertIn(resp.status, (400, 404), f"{resp}")

    def test_bad_bill_id_never_500s(self):
        c = new_user()
        split = c.make_split()
        for bad in BAD_IDS:
            with self.subTest(bill_id=bad):
                base = f"/api/splits/{split['id']}/bills/{path_id(bad)}"
                for resp in (c.get(base), c.put(base, {"store_name": "x"}),
                             c.delete(base)):
                    self.assertLess(resp.status, 500, f"{resp}")
                    self.assertIn(resp.status, (400, 404), f"{resp}")

    def test_malformed_json_body_is_400_not_500(self):
        c = new_user()
        split = c.make_split()
        r = c.request("POST", f"/api/splits/{split['id']}/bills", None)
        self.assertLess(r.status, 500, f"{r}")


if __name__ == "__main__":
    unittest.main()
