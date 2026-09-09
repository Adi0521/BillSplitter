"""Bill items: derived line_total, price parsing, and the three-table join.

The item routes hang off /api/bills/:bid/... with no split id in the URL, which
makes authorization the crux of the slice: ownership can only be established by
joining bill_items -> bills -> splits. Every endpoint here is asserted against
a second user, and an item id under the wrong bill is asserted to be invisible
even when the caller owns both bills.

Expected amounts are literal 4-decimal strings or built with decimal.Decimal;
no float ever derives an expectation here.
"""
import unittest
from decimal import Decimal

from harness import ApiTestCase, BAD_IDS, MISSING_UUID, new_user

# BAD_IDS contains a space, which http.client refuses to put in a request line.
# Encoding only the space keeps the hostile string intact server-side.
def path_id(raw):
    return raw.replace(" ", "%20")


class ItemCase(ApiTestCase, unittest.TestCase):
    def bill(self, client=None, **kw):
        c = client or new_user()
        return c, c.make_bill(c.make_split(**kw)["id"])

    def subtotal_of(self, client, bill):
        r = client.get(f"/api/splits/{bill['split_id']}/bills/{bill['id']}")
        self.assertStatus(r, 200)
        self.assertMoneyString(r.json["subtotal"], "subtotal")
        return r.json["subtotal"]


class TestItemCrud(ItemCase):
    def test_create_returns_the_documented_shape(self):
        c, bill = self.bill()
        r = c.post(f"/api/bills/{bill['id']}/items",
                   {"name": "Olive oil", "price": "12.50", "quantity": 2})
        self.assertStatus(r, 201)
        item = r.json
        self.assertEqual(item["bill_id"], bill["id"])
        self.assertEqual(item["name"], "Olive oil")
        self.assertEqual(item["quantity"], 2)
        self.assertMoneyString(item["price"], "price")
        self.assertMoneyString(item["line_total"], "line_total")
        self.assertEqual(item["price"], "12.5000")
        self.assertEqual(item["line_total"], "25.0000")

    def test_quantity_defaults_to_one(self):
        c, bill = self.bill()
        item = c.make_item(bill["id"], price="4.99")
        self.assertEqual(item["quantity"], 1)
        self.assertEqual(item["line_total"], "4.9900")

    def test_line_total_is_price_times_quantity(self):
        c, bill = self.bill()
        for price, qty in [("12.50", 2), ("0.3333", 3), ("7", 100), ("0", 5)]:
            with self.subTest(price=price, quantity=qty):
                item = c.make_item(bill["id"], price=price, quantity=qty)
                self.assertMoneyString(item["line_total"], "line_total")
                self.assertEqual(Decimal(item["line_total"]),
                                 Decimal(price) * qty)

    def test_list_is_a_bare_array_in_insertion_order(self):
        c, bill = self.bill()
        names = ["First", "Second", "Third"]
        for n in names:
            c.make_item(bill["id"], name=n, price="1.00")
        r = c.get(f"/api/bills/{bill['id']}/items")
        self.assertStatus(r, 200)
        self.assertIsInstance(r.json, list)
        self.assertEqual([i["name"] for i in r.json], names)

    def test_owned_bill_with_no_items_is_an_empty_list_not_404(self):
        c, bill = self.bill()
        r = c.get(f"/api/bills/{bill['id']}/items")
        self.assertStatus(r, 200)
        self.assertEqual(r.json, [])

    def test_update_changes_only_the_fields_sent(self):
        c, bill = self.bill()
        item = c.make_item(bill["id"], name="Olive oil", price="12.50", quantity=2)
        r = c.put(f"/api/bills/{bill['id']}/items/{item['id']}", {"price": "10.00"})
        self.assertStatus(r, 200)
        self.assertEqual(r.json["name"], "Olive oil")
        self.assertEqual(r.json["quantity"], 2)
        self.assertEqual(r.json["price"], "10.0000")
        self.assertEqual(r.json["line_total"], "20.0000")

    def test_empty_update_body_is_400(self):
        c, bill = self.bill()
        item = c.make_item(bill["id"])
        self.assertError(c.put(f"/api/bills/{bill['id']}/items/{item['id']}", {}), 400)

    def test_delete_removes_the_item(self):
        c, bill = self.bill()
        item = c.make_item(bill["id"])
        self.assertStatus(c.delete(f"/api/bills/{bill['id']}/items/{item['id']}"), 200)
        self.assertEqual(c.get(f"/api/bills/{bill['id']}/items").json, [])
        self.assertError(c.delete(f"/api/bills/{bill['id']}/items/{item['id']}"), 404)

    def test_missing_item_is_404(self):
        c, bill = self.bill()
        path = f"/api/bills/{bill['id']}/items/{MISSING_UUID}"
        self.assertError(c.put(path, {"price": "1.00"}), 404)
        self.assertError(c.delete(path), 404)

    def test_currency_defaults_to_the_bills_currency(self):
        c = new_user()
        bill = c.make_bill(c.make_split(currency="EUR")["id"])
        self.assertEqual(c.make_item(bill["id"])["currency"], "EUR")
        self.assertEqual(
            c.make_item(bill["id"], currency="gbp")["currency"], "GBP")


class TestLineTotalIsDerived(ItemCase):
    def test_post_with_line_total_is_400(self):
        c, bill = self.bill()
        r = c.post(f"/api/bills/{bill['id']}/items",
                   {"name": "X", "price": "1.00", "line_total": "99.00"})
        self.assertError(r, 400)

    def test_put_with_line_total_is_400(self):
        c, bill = self.bill()
        item = c.make_item(bill["id"])
        r = c.put(f"/api/bills/{bill['id']}/items/{item['id']}",
                  {"line_total": "99.00"})
        self.assertError(r, 400)
        self.assertEqual(
            c.get(f"/api/bills/{bill['id']}/items").json[0]["line_total"], "10.0000")


class TestItemsMoveTheBillSubtotal(ItemCase):
    """Every mutation recomputes the parent bill's subtotal in the same
    transaction; a response that reports a new item while the bill still shows
    the old subtotal is the bug this asserts against."""

    def test_adding_editing_and_deleting_move_the_subtotal(self):
        c, bill = self.bill()
        self.assertEqual(self.subtotal_of(c, bill), "0.0000")

        first = c.make_item(bill["id"], price="12.50", quantity=2)
        self.assertEqual(self.subtotal_of(c, bill), "25.0000")

        second = c.make_item(bill["id"], price="4.99")
        self.assertEqual(Decimal(self.subtotal_of(c, bill)),
                         Decimal("12.50") * 2 + Decimal("4.99"))

        c.put(f"/api/bills/{bill['id']}/items/{first['id']}", {"quantity": 1})
        self.assertEqual(Decimal(self.subtotal_of(c, bill)),
                         Decimal("12.50") + Decimal("4.99"))

        c.delete(f"/api/bills/{bill['id']}/items/{second['id']}")
        self.assertEqual(self.subtotal_of(c, bill), "12.5000")

    def test_deleting_the_last_item_leaves_zero_not_null(self):
        c, bill = self.bill()
        item = c.make_item(bill["id"], price="10.00")
        c.delete(f"/api/bills/{bill['id']}/items/{item['id']}")
        subtotal = self.subtotal_of(c, bill)
        self.assertIsNotNone(subtotal)
        self.assertEqual(subtotal, "0.0000")

    def test_item_count_follows_the_items(self):
        c, bill = self.bill()
        item = c.make_item(bill["id"])
        detail = f"/api/splits/{bill['split_id']}/bills/{bill['id']}"
        self.assertEqual(c.get(detail).json["item_count"], 1)
        c.delete(f"/api/bills/{bill['id']}/items/{item['id']}")
        self.assertEqual(c.get(detail).json["item_count"], 0)


class TestPriceParsing(ItemCase):
    def test_prices_are_accepted_as_strings_and_normalized(self):
        c, bill = self.bill()
        for sent, expected in [("12.50", "12.5000"), ("12.5", "12.5000"),
                               ("007.5", "7.5000"), ("0", "0.0000"),
                               ("3", "3.0000"), ("0.0001", "0.0001"),
                               ("99999999.9999", "99999999.9999")]:
            with self.subTest(price=sent):
                item = c.make_item(bill["id"], price=sent)
                self.assertMoneyString(item["price"], "price")
                self.assertEqual(item["price"], expected)
                self.assertEqual(Decimal(item["price"]), Decimal(sent))

    def test_rejected_prices(self):
        c, bill = self.bill()
        for bad in ["-1.00", "-0.0001", "1.23456", "12,50", "1e3", "1E3",
                    "$12.50", "12.5.5", "abc", "", "   ", "100000000.0000"]:
            with self.subTest(price=bad):
                r = c.post(f"/api/bills/{bill['id']}/items",
                           {"name": "X", "price": bad})
                self.assertError(r, 400)

    def test_price_is_required(self):
        c, bill = self.bill()
        self.assertError(c.post(f"/api/bills/{bill['id']}/items", {"name": "X"}), 400)

    def test_name_is_required_and_bounded(self):
        c, bill = self.bill()
        for bad in ["", "   ", "\t\n", "x" * 201]:
            with self.subTest(name=bad[:12]):
                r = c.post(f"/api/bills/{bill['id']}/items",
                           {"name": bad, "price": "1.00"})
                self.assertError(r, 400)
        self.assertError(c.post(f"/api/bills/{bill['id']}/items", {"price": "1.00"}), 400)

    def test_rejected_quantities(self):
        c, bill = self.bill()
        for bad in [0, -1, 2.5, 100001, "2", True]:
            with self.subTest(quantity=bad):
                r = c.post(f"/api/bills/{bill['id']}/items",
                           {"name": "X", "price": "1.00", "quantity": bad})
                self.assertError(r, 400)

    def test_null_quantity_means_the_default(self):
        """null is "not supplied", the same reading currency gets."""
        c, bill = self.bill()
        r = c.post(f"/api/bills/{bill['id']}/items",
                   {"name": "X", "price": "1.00", "quantity": None})
        self.assertStatus(r, 201)
        self.assertEqual(r.json["quantity"], 1)

    def test_quantity_bounds_are_inclusive(self):
        c, bill = self.bill()
        self.assertEqual(c.make_item(bill["id"], price="1.00", quantity=1)["quantity"], 1)
        item = c.make_item(bill["id"], price="1.00", quantity=100000)
        self.assertEqual(item["line_total"], "100000.0000")


class TestWrongBill(ItemCase):
    def test_item_under_the_wrong_bill_is_404_even_for_its_owner(self):
        """Both bills belong to the caller; the item still does not exist under
        the bill named in the URL."""
        c = new_user()
        split = c.make_split()
        mine = c.make_bill(split["id"], store_name="Mine")
        other = c.make_bill(split["id"], store_name="Other")
        item = c.make_item(mine["id"], price="10.00")

        self.assertError(
            c.put(f"/api/bills/{other['id']}/items/{item['id']}", {"price": "1.00"}),
            404)
        self.assertError(
            c.delete(f"/api/bills/{other['id']}/items/{item['id']}"), 404)
        # And the item is untouched.
        self.assertEqual(
            c.get(f"/api/bills/{mine['id']}/items").json[0]["price"], "10.0000")


class TestItemOwnership(ItemCase):
    def test_another_user_gets_404_on_all_four_verbs(self):
        owner, other = new_user(), new_user()
        bill = owner.make_bill(owner.make_split()["id"])
        item = owner.make_item(bill["id"], price="10.00")
        path = f"/api/bills/{bill['id']}/items/{item['id']}"

        self.assertError(other.get(f"/api/bills/{bill['id']}/items"), 404)
        self.assertError(other.post(f"/api/bills/{bill['id']}/items",
                                    {"name": "Sneaky", "price": "1.00"}), 404)
        self.assertError(other.put(path, {"price": "0.01"}), 404)
        self.assertError(other.delete(path), 404)

        # Nothing leaked and nothing changed.
        items = owner.get(f"/api/bills/{bill['id']}/items").json
        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["price"], "10.0000")

    def test_unauthenticated_access_is_401(self):
        from harness import Client
        owner = new_user()
        bill = owner.make_bill(owner.make_split()["id"])
        item = owner.make_item(bill["id"])
        anon = Client()
        self.assertError(anon.get(f"/api/bills/{bill['id']}/items"), 401)
        self.assertError(anon.post(f"/api/bills/{bill['id']}/items",
                                   {"name": "X", "price": "1.00"}), 401)
        self.assertError(
            anon.put(f"/api/bills/{bill['id']}/items/{item['id']}", {"price": "1"}), 401)
        self.assertError(
            anon.delete(f"/api/bills/{bill['id']}/items/{item['id']}"), 401)

    def test_missing_bill_is_404(self):
        c = new_user()
        self.assertError(c.get(f"/api/bills/{MISSING_UUID}/items"), 404)
        self.assertError(c.post(f"/api/bills/{MISSING_UUID}/items",
                                {"name": "X", "price": "1.00"}), 404)


class TestItemBadIds(ItemCase):
    def test_bad_bill_id_never_500s(self):
        c = new_user()
        for bad in BAD_IDS:
            with self.subTest(bill_id=bad):
                base = f"/api/bills/{path_id(bad)}/items"
                for resp in (c.get(base),
                             c.post(base, {"name": "X", "price": "1.00"}),
                             c.put(f"{base}/{MISSING_UUID}", {"price": "1.00"}),
                             c.delete(f"{base}/{MISSING_UUID}")):
                    self.assertLess(resp.status, 500, f"{resp}")
                    self.assertIn(resp.status, (400, 404), f"{resp}")

    def test_bad_item_id_never_500s(self):
        c, bill = self.bill()
        for bad in BAD_IDS:
            with self.subTest(item_id=bad):
                path = f"/api/bills/{bill['id']}/items/{path_id(bad)}"
                for resp in (c.put(path, {"price": "1.00"}), c.delete(path)):
                    self.assertLess(resp.status, 500, f"{resp}")
                    self.assertIn(resp.status, (400, 404), f"{resp}")

    def test_malformed_json_body_is_400_not_500(self):
        c, bill = self.bill()
        r = c.request("POST", f"/api/bills/{bill['id']}/items", None)
        self.assertLess(r.status, 500, f"{r}")


if __name__ == "__main__":
    unittest.main()
