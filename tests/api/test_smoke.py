"""Proves the harness and the runner work. If this fails, no other suite's
result means anything."""
import unittest
from harness import Client, new_user, ApiTestCase


class TestSmoke(ApiTestCase, unittest.TestCase):
    def test_health(self):
        r = Client().get("/api/health")
        self.assertStatus(r, 200)
        self.assertEqual(r.json, {"status": "ok"})

    def test_register_and_me(self):
        c = new_user(display_name="Smoke")
        r = c.get("/api/auth/me")
        self.assertStatus(r, 200)
        self.assertEqual(r.json["display_name"], "Smoke")

    def test_two_clients_have_separate_sessions(self):
        a, b = new_user(), new_user()
        self.assertNotEqual(a.get("/api/auth/me").json["id"],
                            b.get("/api/auth/me").json["id"])

    def test_fixture_builders(self):
        c = new_user()
        split = c.make_split()
        bill = c.make_bill(split["id"])
        item = c.make_item(bill["id"], price="12.50", quantity=2)
        self.assertMoneyString(item["line_total"], "line_total")
        self.assertEqual(item["line_total"], "25.0000")
