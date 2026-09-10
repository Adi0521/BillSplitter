"""Phase 6 — Payments.

A payment records that money actually moved. It never changes what anyone
*owes*, which is why nothing in this suite asserts a payment against a balance:
per docs/api.md, overpaying, paying early and settling someone else's debt are
all legitimate, and a server that rejected them would be wrong about people
rather than about arithmetic.

The points that carry the most weight here:

  * `amount` is a 4-decimal **string**, never a JSON number. A float in this
    field is the bug the whole money representation exists to prevent, so the
    type is asserted, not just the value.
  * The schema permits NULL for both `from_member` and `to_member`; the API does
    not. A payment with no sender or no recipient cannot affect a balance and is
    only ever a bug, so both are required.
  * `from_member == to_member` is a 400. Paying yourself would not fail loudly —
    it would silently distort the balances — so it has to be refused up front.
  * A member id from another split is a **400**, not a 404: the split in the URL
    was found, so the body is what is wrong. A *split* the caller does not own
    is a 404, so the API never reveals which split ids exist.
"""
import unittest
from decimal import Decimal

from harness import ApiTestCase, BAD_IDS, MISSING_UUID, Client, new_user

# BAD_IDS contains a raw space, which http.client refuses to put in a request
# line (InvalidURL, raised before anything is sent). Encoding just the space
# keeps the hostile string intact from the server's point of view.
def path_id(raw):
    return raw.replace(" ", "%20")


PAYMENT_KEYS = {
    "id", "split_id", "from_member", "from_name", "to_member", "to_name",
    "amount", "currency", "method", "notes", "paid_at",
}


class PaymentCase(ApiTestCase, unittest.TestCase):
    """Fixtures shared by the suites: a split with two members to move money
    between, since a payment needs a distinct sender and recipient."""

    def make_split_with_members(self, client, currency=None, names=("Alice", "Bob")):
        split = client.make_split(currency=currency)
        members = [client.make_member(split["id"], name=n) for n in names]
        return split, members

    def pay(self, client, split_id, **body):
        return client.post(f"/api/splits/{split_id}/payments", body)

    def make_payment(self, client, split_id, frm, to, amount="20.00", **kw):
        body = {"from_member": frm, "to_member": to, "amount": amount}
        body.update(kw)
        r = client.post(f"/api/splits/{split_id}/payments", body)
        assert r.status == 201, f"make_payment failed: {r}"
        return r.json


class TestCreatePayment(PaymentCase):

    def test_create_returns_201_and_every_documented_field(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        r = self.pay(c, split["id"], from_member=bob["id"], to_member=alice["id"],
                     amount="20.00", method="venmo", notes="for the cabin")
        self.assertStatus(r, 201)
        self.assertEqual(set(r.json.keys()), PAYMENT_KEYS,
                         f"Payment shape drifted: {sorted(r.json.keys())}")
        self.assertEqual(r.json["split_id"], split["id"])
        self.assertEqual(r.json["from_member"], bob["id"])
        self.assertEqual(r.json["to_member"], alice["id"])
        self.assertEqual(r.json["method"], "venmo")
        self.assertEqual(r.json["notes"], "for the cabin")
        self.assertTrue(r.json["paid_at"])

    def test_the_member_names_are_joined_in_so_the_ui_never_shows_a_bare_uuid(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        created = self.make_payment(c, split["id"], bob["id"], alice["id"])
        self.assertEqual(created["from_name"], "Bob")
        self.assertEqual(created["to_name"], "Alice")
        listed = c.get(f"/api/splits/{split['id']}/payments").json[0]
        self.assertEqual((listed["from_name"], listed["to_name"]), ("Bob", "Alice"))

    def test_amount_is_a_4_decimal_string_not_a_json_number(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        created = self.make_payment(c, split["id"], bob["id"], alice["id"],
                                    amount="20.5")
        self.assertMoneyString(created["amount"], "amount")
        self.assertEqual(created["amount"], "20.5000",
                         "the server normalizes to 4 decimal places")

    def test_amounts_are_stored_exactly_as_written_not_through_a_float(self):
        """0.1 + 0.2 is the canonical reason money is not a double. A value
        that a float cannot hold exactly must survive the round trip."""
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        for sent, expected in (("0.1", "0.1000"), ("0.2", "0.2000"),
                               ("1234.5678", "1234.5678"),
                               ("99999999.9999", "99999999.9999")):
            with self.subTest(amount=sent):
                created = self.make_payment(c, split["id"], bob["id"],
                                            alice["id"], amount=sent)
                self.assertEqual(created["amount"], expected)
                self.assertEqual(Decimal(created["amount"]), Decimal(sent))

    def test_method_and_notes_are_optional_and_come_back_null_when_absent(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        created = self.make_payment(c, split["id"], bob["id"], alice["id"])
        self.assertIsNone(created["method"], "an absent method is null, not \"\"")
        self.assertIsNone(created["notes"], "an absent note is null, not \"\"")

    def test_a_blank_or_null_method_or_note_stores_null(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        for value in ("", "   ", None):
            with self.subTest(value=repr(value)):
                created = self.make_payment(c, split["id"], bob["id"],
                                            alice["id"], method=value, notes=value)
                self.assertIsNone(created["method"])
                self.assertIsNone(created["notes"])

    def test_method_is_free_text_not_an_enum(self):
        """People pay each other through services we have not heard of;
        an allow-list here would be wrong within a year."""
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        for method in ("venmo", "cash", "zelle", "Revolut", "wise",
                       "IOU on a napkin", "个人转账"):
            with self.subTest(method=method):
                created = self.make_payment(c, split["id"], bob["id"],
                                            alice["id"], method=method)
                self.assertEqual(created["method"], method)

    def test_payments_are_never_validated_against_what_anyone_owes(self):
        """Overpaying, paying early and settling a debt someone else incurred
        are all real. There are no bills in this split at all."""
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        r = self.pay(c, split["id"], from_member=bob["id"],
                     to_member=alice["id"], amount="10000.00")
        self.assertStatus(r, 201, "an overpayment is a legitimate record: ")


class TestPaymentCurrency(PaymentCase):

    def test_an_omitted_currency_inherits_the_splits_currency_not_usd(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c, currency="EUR")
        created = self.make_payment(c, split["id"], bob["id"], alice["id"])
        self.assertEqual(created["currency"], "EUR")

    def test_an_empty_currency_also_means_use_the_splits(self):
        """That is what an untouched <select> submits, and it means the same
        thing as omitting the field — consistent with bills."""
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c, currency="EUR")
        for blank in ("", "   "):
            with self.subTest(currency=repr(blank)):
                created = self.make_payment(c, split["id"], bob["id"],
                                            alice["id"], currency=blank)
                self.assertEqual(created["currency"], "EUR")

    def test_an_explicit_currency_is_uppercased(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        created = self.make_payment(c, split["id"], bob["id"], alice["id"],
                                    currency="gbp")
        self.assertEqual(created["currency"], "GBP")

    def test_a_currency_that_is_not_three_ascii_letters_is_400(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        for bad in ("US", "USDD", "US1", "u$d", "usd1", 42, ["USD"]):
            with self.subTest(currency=bad):
                self.assertError(
                    self.pay(c, split["id"], from_member=bob["id"],
                             to_member=alice["id"], amount="1.00",
                             currency=bad), 400)


class TestPaymentValidation(PaymentCase):

    def setUp(self):
        self.c = new_user()
        self.split, (self.alice, self.bob) = self.make_split_with_members(self.c)

    def _post(self, body):
        return self.c.post(f"/api/splits/{self.split['id']}/payments", body)

    def _valid(self, **overrides):
        body = {"from_member": self.bob["id"], "to_member": self.alice["id"],
                "amount": "5.00"}
        body.update(overrides)
        return body

    def test_the_baseline_body_is_actually_valid(self):
        """Guards the negative cases below: they only prove anything if the
        body they mutate would otherwise have been accepted."""
        self.assertStatus(self._post(self._valid()), 201)

    def test_a_missing_from_or_to_member_is_400(self):
        for key in ("from_member", "to_member"):
            with self.subTest(missing=key):
                body = self._valid()
                del body[key]
                self.assertError(self._post(body), 400)

    def test_an_explicitly_null_member_is_400_even_though_the_column_allows_it(self):
        """A payment with no sender or no recipient cannot affect a balance."""
        for key in ("from_member", "to_member"):
            with self.subTest(null=key):
                self.assertError(self._post(self._valid(**{key: None})), 400)

    def test_a_blank_or_non_string_member_is_400_not_500(self):
        for key in ("from_member", "to_member"):
            for value in ("", "   ", 42, ["uuid"], {}):
                with self.subTest(field=key, value=value):
                    self.assertError(self._post(self._valid(**{key: value})), 400)

    def test_a_malformed_member_uuid_is_400_not_500(self):
        for bad in BAD_IDS:
            with self.subTest(member=bad):
                self.assertError(self._post(self._valid(from_member=bad)), 400)

    def test_paying_yourself_is_400(self):
        """It would silently distort the balance rather than fail loudly."""
        self.assertError(
            self._post(self._valid(from_member=self.bob["id"],
                                   to_member=self.bob["id"])), 400)

    def test_a_member_from_another_split_is_400_not_404(self):
        """The split in the URL was found, so the body is what is wrong."""
        other = self.c.make_split()
        outsider = self.c.make_member(other["id"], name="Outsider")
        for key in ("from_member", "to_member"):
            with self.subTest(field=key):
                self.assertError(self._post(self._valid(**{key: outsider["id"]})), 400)

    def test_a_member_that_does_not_exist_at_all_is_400(self):
        self.assertError(self._post(self._valid(from_member=MISSING_UUID)), 400)

    def test_a_missing_amount_is_400(self):
        body = self._valid()
        del body["amount"]
        self.assertError(self._post(body), 400)

    def test_an_empty_amount_is_rejected_not_read_as_zero(self):
        """A cleared field or a typo must not quietly become a real number."""
        for value in ("", "   ", None):
            with self.subTest(amount=repr(value)):
                self.assertError(self._post(self._valid(amount=value)), 400)

    def test_a_zero_or_negative_amount_is_400(self):
        for value in ("0", "0.00", "0.0000", "-5", "-0.0001"):
            with self.subTest(amount=value):
                self.assertError(self._post(self._valid(amount=value)), 400)

    def test_a_non_numeric_amount_is_400(self):
        for value in ("abc", "1,50", "1.2.3", "1e3", "$5", True, ["5"], {"v": 5}):
            with self.subTest(amount=value):
                self.assertError(self._post(self._valid(amount=value)), 400)

    def test_more_than_four_decimal_places_is_400_not_a_silent_round(self):
        self.assertError(self._post(self._valid(amount="1.00005")), 400)

    def test_an_amount_beyond_numeric_12_4_is_400_not_500(self):
        for value in ("100000000", "999999999.9999"):
            with self.subTest(amount=value):
                self.assertError(self._post(self._valid(amount=value)), 400)

    def test_a_50_character_method_is_accepted_and_51_is_rejected(self):
        self.assertStatus(self._post(self._valid(method="m" * 50)), 201)
        self.assertError(self._post(self._valid(method="m" * 51)), 400)

    def test_a_1000_character_note_is_accepted_and_1001_is_rejected(self):
        self.assertStatus(self._post(self._valid(notes="n" * 1000)), 201)
        self.assertError(self._post(self._valid(notes="n" * 1001)), 400)

    def test_a_non_string_method_or_note_is_400_not_500(self):
        for key in ("method", "notes"):
            with self.subTest(field=key):
                self.assertError(self._post(self._valid(**{key: 42})), 400)

    def test_a_rejected_payment_leaves_the_list_untouched(self):
        before = len(self.c.get(f"/api/splits/{self.split['id']}/payments").json)
        self.assertError(self._post(self._valid(amount="0")), 400)
        self.assertError(self._post(self._valid(from_member=self.bob["id"],
                                                to_member=self.bob["id"])), 400)
        self.assertError(self._post(self._valid(from_member=MISSING_UUID)), 400)
        after = len(self.c.get(f"/api/splits/{self.split['id']}/payments").json)
        self.assertEqual(after, before, "a 400 must not have inserted anything")


class TestListPayments(PaymentCase):

    def test_the_list_is_a_bare_array_newest_first(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        created = [self.make_payment(c, split["id"], bob["id"], alice["id"],
                                     amount=amt, notes=amt)
                   for amt in ("1.00", "2.00", "3.00")]

        r = c.get(f"/api/splits/{split['id']}/payments")
        self.assertStatus(r, 200)
        self.assertIsInstance(r.json, list, "list endpoints return a bare array")
        self.assertEqual({p["id"] for p in r.json}, {p["id"] for p in created})
        timestamps = [p["paid_at"] for p in r.json]
        self.assertEqual(timestamps, sorted(timestamps, reverse=True),
                         f"paid_at must be non-increasing: {timestamps}")
        if len(set(timestamps)) == len(created):
            self.assertEqual([p["id"] for p in r.json],
                             [p["id"] for p in reversed(created)],
                             "newest first")

    def test_every_listed_payment_has_the_documented_shape(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        self.make_payment(c, split["id"], bob["id"], alice["id"], method="cash")
        for p in c.get(f"/api/splits/{split['id']}/payments").json:
            self.assertEqual(set(p.keys()), PAYMENT_KEYS)
            self.assertEqual(p["split_id"], split["id"])
            self.assertMoneyString(p["amount"], "amount")

    def test_a_split_with_no_payments_lists_an_empty_array_not_a_404(self):
        c = new_user()
        split, _ = self.make_split_with_members(c)
        r = c.get(f"/api/splits/{split['id']}/payments")
        self.assertStatus(r, 200)
        self.assertEqual(r.json, [])

    def test_payments_of_an_unknown_split_are_404(self):
        c = new_user()
        self.assertError(c.get(f"/api/splits/{MISSING_UUID}/payments"), 404)

    def test_one_splits_payments_never_leak_into_another(self):
        c = new_user()
        a, (a_alice, a_bob) = self.make_split_with_members(c)
        b, _ = self.make_split_with_members(c)
        self.make_payment(c, a["id"], a_bob["id"], a_alice["id"], notes="only in A")
        notes = [p["notes"] for p in c.get(f"/api/splits/{b['id']}/payments").json]
        self.assertNotIn("only in A", notes)


class TestDeletePayment(PaymentCase):

    def test_delete_returns_the_documented_message_and_drops_the_payment(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        payment = self.make_payment(c, split["id"], bob["id"], alice["id"])
        r = c.delete(f"/api/splits/{split['id']}/payments/{payment['id']}")
        self.assertStatus(r, 200)
        self.assertEqual(r.json, {"message": "Deleted"})
        self.assertNotIn(payment["id"],
                         [p["id"] for p in
                          c.get(f"/api/splits/{split['id']}/payments").json])

    def test_deleting_the_same_payment_twice_is_404_the_second_time(self):
        c = new_user()
        split, (alice, bob) = self.make_split_with_members(c)
        payment = self.make_payment(c, split["id"], bob["id"], alice["id"])
        path = f"/api/splits/{split['id']}/payments/{payment['id']}"
        self.assertStatus(c.delete(path), 200)
        self.assertError(c.delete(path), 404)

    def test_deleting_an_unknown_payment_is_404(self):
        c = new_user()
        split, _ = self.make_split_with_members(c)
        self.assertError(
            c.delete(f"/api/splits/{split['id']}/payments/{MISSING_UUID}"), 404)

    def test_a_payment_from_another_split_of_the_same_owner_is_404(self):
        """Nesting under a url is not proof of membership in that collection."""
        c = new_user()
        a, (alice, bob) = self.make_split_with_members(c)
        b, _ = self.make_split_with_members(c)
        payment = self.make_payment(c, a["id"], bob["id"], alice["id"])
        self.assertError(
            c.delete(f"/api/splits/{b['id']}/payments/{payment['id']}"), 404)
        # And the payment is still there, in the split it belongs to.
        self.assertIn(payment["id"],
                      [p["id"] for p in
                       c.get(f"/api/splits/{a['id']}/payments").json])


class TestPaymentOwnership(PaymentCase):

    def test_another_user_gets_404_on_every_verb_not_403(self):
        """404, not 403, so the API does not leak which split ids exist."""
        owner, other = new_user(), new_user()
        split, (alice, bob) = self.make_split_with_members(owner)
        payment = self.make_payment(owner, split["id"], bob["id"], alice["id"])

        self.assertError(other.get(f"/api/splits/{split['id']}/payments"), 404)
        self.assertError(
            other.post(f"/api/splits/{split['id']}/payments",
                       {"from_member": bob["id"], "to_member": alice["id"],
                        "amount": "1.00"}), 404)
        self.assertError(
            other.delete(f"/api/splits/{split['id']}/payments/{payment['id']}"), 404)

        # Nothing was changed by the attempts.
        still = owner.get(f"/api/splits/{split['id']}/payments").json
        self.assertEqual([p["id"] for p in still], [payment["id"]])

    def test_a_valid_body_against_a_foreign_split_is_404_not_400(self):
        """Ownership is settled before membership: an outsider naming members
        who really are in that split still gets 404."""
        owner, other = new_user(), new_user()
        split, (alice, bob) = self.make_split_with_members(owner)
        self.assertError(
            other.post(f"/api/splits/{split['id']}/payments",
                       {"from_member": bob["id"], "to_member": alice["id"],
                        "amount": "1.00"}), 404)

    def test_a_bad_body_answers_the_same_whether_or_not_the_split_exists(self):
        """Body validation runs before the database, so it must not become an
        oracle for which split ids exist: the reply is identical either way."""
        owner, other = new_user(), new_user()
        split, _ = self.make_split_with_members(owner)
        for body in ({}, {"from_member": "nope", "to_member": "nope",
                          "amount": "-1"}):
            with self.subTest(body=body):
                real = other.post(f"/api/splits/{split['id']}/payments", body)
                fake = other.post(f"/api/splits/{MISSING_UUID}/payments", body)
                self.assertEqual(real.status, fake.status,
                                 "the status leaks whether the split exists")
                self.assertEqual(real.json, fake.json,
                                 "the message leaks whether the split exists")

    def test_unauthenticated_access_is_401(self):
        owner = new_user()
        split, (alice, bob) = self.make_split_with_members(owner)
        payment = self.make_payment(owner, split["id"], bob["id"], alice["id"])
        anon = Client()
        self.assertError(anon.get(f"/api/splits/{split['id']}/payments"), 401)
        self.assertError(
            anon.post(f"/api/splits/{split['id']}/payments",
                      {"from_member": bob["id"], "to_member": alice["id"],
                       "amount": "1.00"}), 401)
        self.assertError(
            anon.delete(f"/api/splits/{split['id']}/payments/{payment['id']}"), 401)


class TestPaymentBadIds(PaymentCase):
    """A hostile path parameter must never reach Postgres as a uuid cast."""

    def test_bad_split_id_never_500s(self):
        c = new_user()
        for bad in BAD_IDS:
            with self.subTest(split_id=bad):
                base = f"/api/splits/{path_id(bad)}/payments"
                for resp in (c.get(base),
                             c.post(base, {"from_member": MISSING_UUID,
                                           "to_member": MISSING_UUID,
                                           "amount": "1.00"})):
                    self.assertLess(resp.status, 500, f"{resp}")
                    self.assertIn(resp.status, (400, 404), f"{resp}")

    def test_bad_payment_id_never_500s(self):
        c = new_user()
        split, _ = self.make_split_with_members(c)
        for bad in BAD_IDS:
            with self.subTest(payment_id=bad):
                resp = c.delete(
                    f"/api/splits/{split['id']}/payments/{path_id(bad)}")
                self.assertLess(resp.status, 500, f"{resp}")
                self.assertIn(resp.status, (400, 404), f"{resp}")

    def test_malformed_json_body_is_400_not_500(self):
        c = new_user()
        split, _ = self.make_split_with_members(c)
        r = c.request("POST", f"/api/splits/{split['id']}/payments", None)
        self.assertLess(r.status, 500, f"{r}")

    def test_a_non_object_json_body_is_400_not_500(self):
        c = new_user()
        split, _ = self.make_split_with_members(c)
        for body in ([], "string", 42):
            with self.subTest(body=body):
                r = c.post(f"/api/splits/{split['id']}/payments", body)
                self.assertError(r, 400)


if __name__ == "__main__":
    unittest.main(verbosity=2)
