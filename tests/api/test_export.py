"""CSV export: the flat, pivotable table of a whole split.

Two of these tests are security tests rather than format tests, and they are the
reason this file is long:

  * **Formula injection.** A CSV cell beginning with `=`, `+`, `-`, `@`, a tab
    or a carriage return is executed as a formula by Excel and Google Sheets the
    moment the file is opened. Item names come from receipts and from user
    input, so the payload reaches the file through the ordinary API. The export
    is *for* opening in a spreadsheet, so this is not a theoretical concern.

  * **Header injection.** The split name is interpolated into
    `Content-Disposition`. A name containing CRLF must not be able to terminate
    that header and start one of its own.

Everything is parsed with the `csv` module rather than matched as a substring.
String matching cannot tell a correctly quoted field from a broken one, and
those are exactly the cases under test.
"""
import csv
import io
import re
import unittest

from harness import ApiTestCase, BAD_IDS, MISSING_UUID, make_collaborator, new_user

# BAD_IDS contains a space, which http.client refuses to put in a request line.
def path_id(raw):
    return raw.replace(" ", "%20")


HEADER = ["split", "bill_date", "store", "currency", "item",
          "unit_price", "quantity", "line_total", "member", "share"]

# The characters a spreadsheet treats as "this cell is a formula".
FORMULA_LEADERS = ("=", "+", "-", "@", "\t", "\r")


class ExportCase(ApiTestCase, unittest.TestCase):
    def export(self, client, split_id):
        r = client.get(f"/api/splits/{split_id}/export/csv")
        return r

    def csv_rows(self, resp):
        """The response parsed by Python's csv module.

        newline='' is required: without it, StringIO would translate the line
        breaks inside quoted fields and the round-trip test would pass on a
        file that is actually broken.
        """
        self.assertStatus(resp, 200)
        self.assertIn("text/csv", resp.headers.get("Content-Type", ""))
        return list(csv.reader(io.StringIO(resp.body, newline="")))

    def assertNoFormula(self, rows):
        """No cell anywhere in the file may begin with a formula character."""
        for r, row in enumerate(rows):
            for c, cell in enumerate(row):
                if cell.startswith(FORMULA_LEADERS):
                    self.fail(
                        f"cell ({r},{c}) is live formula text: {cell!r}"
                    )


class TestHappyPath(ExportCase):
    """Two bills, a multi-member allocation and an item nobody is on."""

    def setUp(self):
        self.c = new_user()
        self.split = self.c.make_split(name="Tahoe trip", currency="USD")
        self.alice = self.c.make_member(self.split["id"], "Alice")
        self.bob = self.c.make_member(self.split["id"], "Bob")

        # Bill one, the earlier date.
        self.b1 = self.c.make_bill(self.split["id"], "Safeway", "2026-08-30")
        self.oil = self.c.make_item(self.b1["id"], "Olive oil", "12.50", quantity=2)
        self.bread = self.c.make_item(self.b1["id"], "Bread", "3.25")
        r = self.c.allocate(self.b1["id"], self.oil["id"], "ratio", [
            {"member_id": self.alice["id"], "ratio": "60"},
            {"member_id": self.bob["id"], "ratio": "40"},
        ])
        self.assertStatus(r, 200)
        # Bread is deliberately left unallocated.

        # Bill two, the later date.
        self.b2 = self.c.make_bill(self.split["id"], "Trader Joe's", "2026-09-02")
        self.wine = self.c.make_item(self.b2["id"], "Wine", "20.00")
        r = self.c.allocate(self.b2["id"], self.wine["id"], "amount", [
            {"member_id": self.alice["id"], "amount": "12.00"},
        ])
        self.assertStatus(r, 200)

    def test_exact_rows(self):
        rows = self.csv_rows(self.export(self.c, self.split["id"]))
        self.assertEqual(rows, [
            HEADER,
            # An item split two ways is two rows; the item columns repeat.
            ["Tahoe trip", "2026-08-30", "Safeway", "USD",
             "Olive oil", "12.5000", "2", "25.0000", "Alice", "15.0000"],
            ["Tahoe trip", "2026-08-30", "Safeway", "USD",
             "Olive oil", "12.5000", "2", "25.0000", "Bob", "10.0000"],
            # The unallocated item still gets a row.
            ["Tahoe trip", "2026-08-30", "Safeway", "USD",
             "Bread", "3.2500", "1", "3.2500", "", ""],
            ["Tahoe trip", "2026-09-02", "Trader Joe's", "USD",
             "Wine", "20.0000", "1", "20.0000", "Alice", "12.0000"],
        ])

    def test_header_is_exactly_the_documented_columns(self):
        rows = self.csv_rows(self.export(self.c, self.split["id"]))
        self.assertEqual(rows[0], HEADER)

    def test_no_balances_or_settlements_are_exported(self):
        """Balances belong to GET /summary. A second copy of that arithmetic
        here is the drift the contract forbids, so the file must not grow
        columns for it."""
        rows = self.csv_rows(self.export(self.c, self.split["id"]))
        for banned in ("balance", "owes", "fronted", "settlement", "tax", "tip"):
            self.assertNotIn(banned, rows[0])

    def test_money_keeps_four_decimals(self):
        rows = self.csv_rows(self.export(self.c, self.split["id"]))
        for row in rows[1:]:
            for idx, name in ((5, "unit_price"), (7, "line_total")):
                self.assertRegex(row[idx], r"^\d+\.\d{4}$", name)
            if row[9]:
                self.assertRegex(row[9], r"^\d+\.\d{4}$", "share")

    def test_ordering_is_by_bill_date_then_item_then_member(self):
        rows = self.csv_rows(self.export(self.c, self.split["id"]))[1:]
        self.assertEqual([r[1] for r in rows],
                         ["2026-08-30", "2026-08-30", "2026-08-30", "2026-09-02"])
        self.assertEqual([r[4] for r in rows],
                         ["Olive oil", "Olive oil", "Bread", "Wine"])
        self.assertEqual([r[8] for r in rows[:2]], ["Alice", "Bob"])

    def test_two_exports_are_byte_identical(self):
        first = self.export(self.c, self.split["id"])
        second = self.export(self.c, self.split["id"])
        self.assertStatus(first, 200)
        self.assertStatus(second, 200)
        self.assertEqual(first.body, second.body)

    def test_content_disposition_is_an_attachment(self):
        r = self.export(self.c, self.split["id"])
        self.assertStatus(r, 200)
        disp = r.headers.get("Content-Disposition", "")
        self.assertIn("attachment", disp)
        self.assertIn("Tahoe trip", disp)
        self.assertIn(".csv", disp)


class TestFormulaInjection(ExportCase):
    """The headline test. An item name is attacker-controlled text that lands in
    a file whose whole purpose is to be opened by a spreadsheet."""

    PAYLOAD = "=cmd|' /C calc'!A0"

    def test_calc_payload_comes_back_neutralized(self):
        c = new_user()
        split = c.make_split(name="Injection")
        member = c.make_member(split["id"], "Alice")
        bill = c.make_bill(split["id"], "Store", "2026-01-15")
        item = c.make_item(bill["id"], self.PAYLOAD, "10.00")
        r = c.allocate(bill["id"], item["id"], "ratio",
                       [{"member_id": member["id"], "ratio": "100"}])
        self.assertStatus(r, 200)

        rows = self.csv_rows(self.export(c, split["id"]))
        self.assertEqual(len(rows), 2, rows)
        cell = rows[1][4]

        # Neutralized, not mangled: prefixed with the single quote a spreadsheet
        # reads as "this cell is text", with the original text intact after it.
        self.assertEqual(cell, "'" + self.PAYLOAD)
        self.assertFalse(cell.startswith("="))
        self.assertNoFormula(rows)

    def test_every_formula_leader_reachable_through_the_api_is_prefixed(self):
        """`\\t` and `\\r` are stripped by the item-name trim, so the four that
        survive validation are the four tested here. The writer prefixes all
        six."""
        c = new_user()
        split = c.make_split(name="Leaders")
        bill = c.make_bill(split["id"], "Store", "2026-01-15")
        names = ["=SUM(A1:A9)", "+1+1", "-2+3", "@SUM(A1)"]
        for n in names:
            c.make_item(bill["id"], n, "1.00")

        rows = self.csv_rows(self.export(c, split["id"]))
        self.assertEqual([r[4] for r in rows[1:]], ["'" + n for n in names])
        self.assertNoFormula(rows)

    def test_a_member_name_is_neutralized_too(self):
        """Every field is protected, not only the ones expected to be
        dangerous."""
        c = new_user()
        split = c.make_split(name="Members")
        member = c.make_member(split["id"], "=HYPERLINK(\"http://evil\")")
        bill = c.make_bill(split["id"], "Store", "2026-01-15")
        item = c.make_item(bill["id"], "Thing", "4.00")
        r = c.allocate(bill["id"], item["id"], "ratio",
                       [{"member_id": member["id"], "ratio": "50"}])
        self.assertStatus(r, 200)

        rows = self.csv_rows(self.export(c, split["id"]))
        self.assertEqual(rows[1][8], "'=HYPERLINK(\"http://evil\")")
        self.assertNoFormula(rows)

    def test_a_store_name_is_neutralized_too(self):
        c = new_user()
        split = c.make_split(name="Stores")
        c.make_bill(split["id"], "=1+1", "2026-01-15")
        rows = self.csv_rows(self.export(c, split["id"]))
        self.assertEqual(rows[1][2], "'=1+1")
        self.assertNoFormula(rows)


class TestRfc4180Quoting(ExportCase):
    """A field holding a comma, a quote or a newline must survive the round
    trip. These are the cases a substring assertion cannot distinguish."""

    NASTY = 'Nachos, "extra big"\nfor 2'

    def test_comma_quote_and_newline_round_trip(self):
        c = new_user()
        split = c.make_split(name="Quoting")
        bill = c.make_bill(split["id"], "Store", "2026-01-15")
        c.make_item(bill["id"], self.NASTY, "9.99", quantity=3)

        rows = self.csv_rows(self.export(c, split["id"]))
        self.assertEqual(len(rows), 2, rows)
        self.assertEqual(rows[1][4], self.NASTY)
        # The line break inside the field did not split the record.
        self.assertEqual(len(rows[1]), len(HEADER))
        self.assertEqual(rows[1][5], "9.9900")
        self.assertEqual(rows[1][7], "29.9700")

    def test_embedded_quotes_are_doubled_on_the_wire(self):
        c = new_user()
        split = c.make_split(name="Quoting")
        bill = c.make_bill(split["id"], "Store", "2026-01-15")
        c.make_item(bill["id"], 'He said "hi"', "1.00")

        r = self.export(c, split["id"])
        self.assertStatus(r, 200)
        self.assertIn('"He said ""hi"""', r.body)
        self.assertEqual(self.csv_rows(r)[1][4], 'He said "hi"')

    def test_a_comma_in_the_split_name_does_not_shift_the_columns(self):
        c = new_user()
        split = c.make_split(name="Tahoe, trip")
        bill = c.make_bill(split["id"], "Store", "2026-01-15")
        c.make_item(bill["id"], "Thing", "1.00")

        rows = self.csv_rows(self.export(c, split["id"]))
        self.assertEqual(rows[1][0], "Tahoe, trip")
        self.assertEqual(rows[1][4], "Thing")
        self.assertEqual(len(rows[1]), len(HEADER))


class TestHeaderInjection(ExportCase):
    """The split name reaches Content-Disposition. It must not be able to add a
    header of its own."""

    PAYLOAD = 'x"\r\nSet-Cookie: pwned=1'

    def test_crlf_in_the_split_name_adds_no_header(self):
        c = new_user()
        split = c.make_split(name=self.PAYLOAD)
        bill = c.make_bill(split["id"], "Store", "2026-01-15")
        c.make_item(bill["id"], "Thing", "1.00")

        r = self.export(c, split["id"])
        self.assertStatus(r, 200)

        lowered = {k.lower(): v for k, v in r.headers.items()}
        self.assertNotIn("set-cookie", lowered,
                         f"injected a header: {r.headers}")

        disp = r.headers.get("Content-Disposition", "")
        # The characters that make a header are what must not survive: the CR
        # and LF that would end this header, the quote that would end the
        # filename, and the colon that would separate a new header's name from
        # its value. The payload's harmless letters staying in the filename is
        # correct — the split really is called that, and a deny-list on
        # suspicious words would be the weaker defence.
        self.assertNotIn("\r", disp)
        self.assertNotIn("\n", disp)
        self.assertNotIn("Set-Cookie:", disp)
        self.assertEqual(disp.count('"'), 2, "the filename quoting is intact")
        # The filename is an allow-listed stem plus a date: letters, digits,
        # space, dash and underscore only.
        self.assertRegex(disp, r'^attachment; filename="[A-Za-z0-9 _-]+\.csv"$')

    def test_the_name_is_sanitized_only_in_the_header_not_in_the_data(self):
        """Stripping the payload from the header must not corrupt the export's
        own record of what the split is called."""
        c = new_user()
        split = c.make_split(name=self.PAYLOAD)
        bill = c.make_bill(split["id"], "Store", "2026-01-15")
        c.make_item(bill["id"], "Thing", "1.00")

        rows = self.csv_rows(self.export(c, split["id"]))
        self.assertEqual(rows[1][0], self.PAYLOAD)

    def test_a_name_with_nothing_usable_falls_back_to_a_default(self):
        c = new_user()
        split = c.make_split(name="…")
        r = self.export(c, split["id"])
        self.assertStatus(r, 200)
        disp = r.headers.get("Content-Disposition", "")
        self.assertRegex(disp, r'^attachment; filename="[A-Za-z0-9 _-]+\.csv"$')

    def test_a_very_long_name_is_capped(self):
        c = new_user()
        split = c.make_split(name="A" * 200)
        r = self.export(c, split["id"])
        self.assertStatus(r, 200)
        disp = r.headers.get("Content-Disposition", "")
        filename = re.search(r'filename="([^"]*)"', disp).group(1)
        self.assertLessEqual(len(filename), 80, filename)


class TestRowsThatMustNotVanish(ExportCase):
    """The export exists to surface what nobody is on the hook for. A row that
    silently disappears defeats the point."""

    def test_an_unallocated_item_still_yields_a_row(self):
        c = new_user()
        split = c.make_split(name="Gaps")
        bill = c.make_bill(split["id"], "Store", "2026-01-15")
        c.make_item(bill["id"], "Nobody's bread", "3.25")

        rows = self.csv_rows(self.export(c, split["id"]))
        self.assertEqual(len(rows), 2, rows)
        self.assertEqual(rows[1][4], "Nobody's bread")
        self.assertEqual(rows[1][7], "3.2500")
        self.assertEqual(rows[1][8], "", "member must be empty")
        self.assertEqual(rows[1][9], "", "share must be empty")

    def test_a_partially_allocated_item_does_not_get_a_phantom_empty_row(self):
        """One row per allocation — the remainder is visible as the gap between
        line_total and the shares, not as an extra blank-member row."""
        c = new_user()
        split = c.make_split(name="Partial")
        member = c.make_member(split["id"], "Alice")
        bill = c.make_bill(split["id"], "Store", "2026-01-15")
        item = c.make_item(bill["id"], "Pizza", "10.00")
        r = c.allocate(bill["id"], item["id"], "ratio",
                       [{"member_id": member["id"], "ratio": "50"}])
        self.assertStatus(r, 200)

        rows = self.csv_rows(self.export(c, split["id"]))
        self.assertEqual(len(rows), 2, rows)
        self.assertEqual(rows[1][8], "Alice")
        self.assertEqual(rows[1][9], "5.0000")

    def test_a_bill_with_no_items_still_appears(self):
        c = new_user()
        split = c.make_split(name="Empty bill")
        c.make_bill(split["id"], "Safeway", "2026-01-15")

        rows = self.csv_rows(self.export(c, split["id"]))
        self.assertEqual(len(rows), 2, rows)
        self.assertEqual(rows[1][:4], ["Empty bill", "2026-01-15", "Safeway", "USD"])
        self.assertEqual(rows[1][4:], ["", "", "", "", "", ""])

    def test_a_split_with_no_bills_returns_the_header_only(self):
        c = new_user()
        split = c.make_split(name="Nothing here")
        r = self.export(c, split["id"])
        rows = self.csv_rows(r)
        self.assertEqual(rows, [HEADER])

    def test_three_way_split_exports_three_rows(self):
        c = new_user()
        split = c.make_split(name="Three")
        members = [c.make_member(split["id"], n) for n in ("Ann", "Bea", "Cyd")]
        bill = c.make_bill(split["id"], "Store", "2026-01-15")
        item = c.make_item(bill["id"], "Cake", "10.00")
        r = c.allocate(bill["id"], item["id"], "ratio",
                       [{"member_id": m["id"], "ratio": "33.3333"} for m in members])
        self.assertStatus(r, 200)

        rows = self.csv_rows(self.export(c, split["id"]))
        self.assertEqual(len(rows), 4, rows)
        self.assertEqual([r[8] for r in rows[1:]], ["Ann", "Bea", "Cyd"])
        # The documented rounding: three shares of 3.3333, remainder unassigned.
        self.assertEqual([r[9] for r in rows[1:]], ["3.3333"] * 3)


class TestAuthorization(ExportCase):
    def test_another_users_split_is_404(self):
        owner = new_user()
        split = owner.make_split(name="Private")
        bill = owner.make_bill(split["id"], "Store", "2026-01-15")
        owner.make_item(bill["id"], "Secret caviar", "999.00")

        intruder = new_user()
        r = self.export(intruder, split["id"])
        self.assertError(r, 404)
        self.assertNotIn("caviar", r.body)

    def test_unauthenticated_is_401(self):
        owner = new_user()
        split = owner.make_split(name="Private")
        from harness import Client
        r = Client().get(f"/api/splits/{split['id']}/export/csv")
        self.assertError(r, 401)

    def test_missing_split_is_404(self):
        c = new_user()
        r = self.export(c, MISSING_UUID)
        self.assertError(r, 404)

    def test_malformed_ids_never_500(self):
        c = new_user()
        for bad in BAD_IDS:
            r = c.get(f"/api/splits/{path_id(bad)}/export/csv")
            self.assertIn(r.status, (400, 404),
                          f"{bad!r} produced {r.status}: {r.body[:200]}")
            self.assertIsInstance(r.json, dict, f"{bad!r}: {r.body[:200]}")
            self.assertIn("error", r.json)

    def test_errors_are_json_not_csv(self):
        """A failed download still speaks the API's error envelope."""
        c = new_user()
        r = self.export(c, MISSING_UUID)
        self.assertStatus(r, 404)
        self.assertIn("application/json", r.headers.get("Content-Type", ""))


class TestCollaborator(ExportCase):
    """Export is collaborative: a linked member downloads the same file the
    owner does. The intruder test above is unchanged — split_role() is NULL for
    them and NULL is still 404."""

    def setUp(self):
        self.owner = new_user()
        self.split = self.owner.make_split(name="Tahoe trip", currency="USD")
        self.collab, self.seat = make_collaborator(self.owner, self.split["id"], name="Bob")
        self.alice = self.owner.make_member(self.split["id"], "Alice")
        bill = self.owner.make_bill(self.split["id"], "Safeway", "2026-08-30")
        oil = self.owner.make_item(bill["id"], "Olive oil", "12.50", quantity=2)
        r = self.owner.allocate(bill["id"], oil["id"], "ratio", [
            {"member_id": self.alice["id"], "ratio": "60"},
            {"member_id": self.seat["id"], "ratio": "40"},
        ])
        self.assertStatus(r, 200)

    def test_collaborator_gets_the_identical_csv(self):
        mine = self.export(self.owner, self.split["id"])
        theirs = self.export(self.collab, self.split["id"])
        self.assertStatus(theirs, 200)
        self.assertIn("text/csv", theirs.headers.get("Content-Type", ""))
        self.assertEqual(theirs.body, mine.body,
                         "a member must download exactly the owner's file")
        rows = self.csv_rows(theirs)
        self.assertEqual(rows, [
            HEADER,
            ["Tahoe trip", "2026-08-30", "Safeway", "USD",
             "Olive oil", "12.5000", "2", "25.0000", "Alice", "15.0000"],
            ["Tahoe trip", "2026-08-30", "Safeway", "USD",
             "Olive oil", "12.5000", "2", "25.0000", "Bob", "10.0000"],
        ])

    def test_collaborator_exports_an_empty_split_as_header_only(self):
        """The access check and the row query are separate; a member of an
        empty split gets the header, not a 404."""
        empty = self.owner.make_split(name="Nothing yet")
        collab, _ = make_collaborator(self.owner, empty["id"])
        rows = self.csv_rows(self.export(collab, empty["id"]))
        self.assertEqual(rows, [HEADER])

    def test_collaborator_is_404_on_the_owners_other_split(self):
        other = self.owner.make_split(name="Private")
        bill = self.owner.make_bill(other["id"], "Store", "2026-01-15")
        self.owner.make_item(bill["id"], "Secret caviar", "999.00")
        r = self.export(self.collab, other["id"])
        self.assertError(r, 404)
        self.assertNotIn("caviar", r.body)


if __name__ == "__main__":
    unittest.main()
