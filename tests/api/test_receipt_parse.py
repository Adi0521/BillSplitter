"""Receipt parsing: upload limits, content sniffing, ownership, and the HTML path.

This is the first endpoint that accepts a file, so most of what is asserted here
is about what the server refuses: a body over the cap, a PDF, a file whose
declared type is a lie, another user's split. The one happy path that is fully
checkable without a rendering library is the HTML receipt, and it is the most
valuable test in the file — it exercises upload, sniffing, tag stripping, the
line parser and the totals cross-check end to end, with no image fixture and no
dependence on OCR accuracy.

There is no PIL and no ImageMagick on the test machine and the harness is
deliberately stdlib-only, so the images used here are generated at test time
with zlib and struct rather than committed as binary fixtures. That yields a
*structurally valid* PNG, not one with legible text on it: the image tests
therefore assert the contract (accepted, decoded, ReceiptDraft returned) and
never assert what OCR read. Recognition quality is not something an integration
test can pin down without making the suite flaky.

Nothing in this suite may create a row. The endpoint persists nothing, and
test_parse_persists_nothing is the assertion that keeps it that way.
"""
import struct
import unittest
import urllib.error
import urllib.request
import uuid
import zlib

from harness import ApiTestCase, BAD_IDS, MISSING_UUID, Response, new_user

MAX_UPLOAD = 10 * 1024 * 1024


# ── fixtures generated at test time ─────────────────────────────────────────

def png_bytes(width=120, height=60, fill=0xFF):
    """A structurally valid 8-bit grayscale PNG, built from nothing but zlib.

    Blank, so OCR finds no text in it — which is exactly the contract this is
    used to test: a real image is accepted and comes back as an empty draft
    with a warning, rather than as an error.
    """
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes([fill]) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


PDF_BYTES = b"%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\ntrailer\n%%EOF\n"
GIF_BYTES = b"GIF89a" + b"\x01\x00\x01\x00" + b"\x00" * 20

RECEIPT_HTML = b"""<!DOCTYPE html>
<html><head><title>Your online order</title>
<style>.total { font-weight: bold; }</style>
<script>var analytics = {"grand_total": 999.99};</script>
</head>
<body>
<h1>SAFEWAY</h1>
<p>1234 Market St &mdash; 09/01/2026</p>
<table>
  <tr><td>MILK 2%</td><td>$3.49</td></tr>
  <tr><td>SOURDOUGH LOAF</td><td>$4.99</td></tr>
  <tr><td>BANANAS</td><td>$1.29</td></tr>
  <tr><td>MFR COUPON</td><td>-$0.50</td></tr>
  <tr><td>SUBTOTAL</td><td>$9.77</td></tr>
  <tr><td>TAX</td><td>$0.52</td></tr>
  <tr class="total"><td>TOTAL</td><td>$10.29</td></tr>
</table>
</body></html>
"""

BOUNDARY = "----BillSplitterTestBoundary9f3c"


def multipart(content, filename="receipt.png", content_type="image/png", field="file"):
    """A multipart/form-data body. Written by hand because the harness speaks
    JSON only and this is the one endpoint that does not."""
    parts = [
        f"--{BOUNDARY}\r\n".encode(),
        f'Content-Disposition: form-data; name="{field}"; filename="{filename}"\r\n'.encode(),
        f"Content-Type: {content_type}\r\n\r\n".encode(),
        content,
        f"\r\n--{BOUNDARY}--\r\n".encode(),
    ]
    return b"".join(parts)


def post_file(client, path, content, **kw):
    """POST a multipart upload on `client`'s session (its cookie jar)."""
    body = multipart(content, **kw)
    req = urllib.request.Request(client.base_url + path, data=body, method="POST")
    req.add_header("Content-Type", f"multipart/form-data; boundary={BOUNDARY}")
    req.add_header("Content-Length", str(len(body)))
    try:
        # OCR is slower than every other endpoint in this API, so the timeout is
        # generous — but still finite: a hung upload is a real failure mode.
        with client.opener.open(req, timeout=60) as resp:
            return Response(resp.status, resp.read().decode(), dict(resp.headers))
    except urllib.error.HTTPError as e:
        return Response(e.code, e.read().decode(), dict(e.headers))


def parse_path(split_id):
    # BAD_IDS contains a space, and http.client refuses to put a raw space in a
    # request line (InvalidURL, raised before the request is ever sent).
    return f"/api/splits/{str(split_id).replace(' ', '%20')}/bills/parse"


DRAFT_FIELDS = ("source", "store_name", "date", "currency", "items", "tax", "tip",
                "total_read", "items_total", "totals_agree", "unmatched_lines",
                "warnings")


class ReceiptCase(ApiTestCase):
    def assertDraftShape(self, draft):
        """Every documented field of a ReceiptDraft is present and typed."""
        for field in DRAFT_FIELDS:
            self.assertIn(field, draft, f"draft is missing {field}")
        self.assertIn(draft["source"], ("image", "html"))
        self.assertIsInstance(draft["store_name"], str)   # "" when unknown, never null
        self.assertIsInstance(draft["items"], list)
        self.assertIsInstance(draft["unmatched_lines"], list)
        self.assertIsInstance(draft["warnings"], list)
        # bool when the receipt printed a total, None when it did not — those
        # are different facts, and reporting "nothing to check" as True would
        # claim a verification that never ran.
        self.assertIn(type(draft["totals_agree"]), (bool, type(None)))
        if draft["total_read"] is None:
            self.assertIsNone(
                draft["totals_agree"],
                "no total was read, so there was nothing to agree with",
            )
        self.assertMoneyString(draft["items_total"], "items_total")
        for field in ("tax", "tip", "total_read"):
            if draft[field] is not None:
                self.assertMoneyString(draft[field], field)
        for item in draft["items"]:
            self.assertMoneyString(item["price"], "item price")
            self.assertIsInstance(item["quantity"], int)
            self.assertGreaterEqual(item["quantity"], 1)
            self.assertIsInstance(item["confidence"], float)
            self.assertGreaterEqual(item["confidence"], 0.0)
            self.assertLessEqual(item["confidence"], 1.0)


# ── authorization ───────────────────────────────────────────────────────────

class TestReceiptParseAuthorization(ReceiptCase, unittest.TestCase):
    def test_unauthenticated_is_401(self):
        owner = new_user()
        split = owner.make_split()
        anon = type(owner)()          # a Client with an empty cookie jar
        r = post_file(anon, parse_path(split["id"]), png_bytes())
        self.assertError(r, 401)

    def test_another_users_split_is_404_not_403(self):
        """Nesting under /api/splits/<id>/ is not proof of ownership, and the
        API must not leak which split ids exist."""
        owner = new_user()
        split = owner.make_split()
        stranger = new_user()
        r = post_file(stranger, parse_path(split["id"]), png_bytes())
        self.assertError(r, 404)
        self.assertNotIn("403", str(r.status))

    def test_missing_split_is_404(self):
        c = new_user()
        r = post_file(c, parse_path(MISSING_UUID), png_bytes())
        self.assertError(r, 404)

    def test_malformed_split_id_is_404_never_500(self):
        c = new_user()
        for bad in BAD_IDS:
            with self.subTest(split_id=bad):
                r = post_file(c, parse_path(bad), png_bytes())
                self.assertError(r, 404)

    def test_ownership_is_checked_before_the_file_is_read(self):
        """A stranger uploading junk gets 404, not 400: the server must not
        confirm the split exists by grading the file first."""
        owner = new_user()
        split = owner.make_split()
        stranger = new_user()
        r = post_file(stranger, parse_path(split["id"]), b"not an image at all")
        self.assertError(r, 404)


# ── limits ──────────────────────────────────────────────────────────────────

class TestReceiptParseLimits(ReceiptCase, unittest.TestCase):
    def test_over_ten_megabytes_is_413(self):
        c = new_user()
        split = c.make_split()
        oversize = b"\x00" * (MAX_UPLOAD + 1024)
        r = post_file(c, parse_path(split["id"]), oversize)
        self.assertError(r, 413)
        self.assertIn("10 MB", r.error)

    def test_pdf_is_415_and_says_so(self):
        """Tesseract cannot read a PDF. Detected by name so the user is told
        that, rather than being told their file is corrupt."""
        c = new_user()
        split = c.make_split()
        r = post_file(c, parse_path(split["id"]), PDF_BYTES,
                      filename="receipt.pdf", content_type="application/pdf")
        self.assertError(r, 415)
        self.assertIn("PDF", r.error)

    def test_pdf_disguised_as_png_is_still_415(self):
        """The declared Content-Type is client-controlled; content decides."""
        c = new_user()
        split = c.make_split()
        r = post_file(c, parse_path(split["id"]), PDF_BYTES,
                      filename="receipt.png", content_type="image/png")
        self.assertError(r, 415)
        self.assertIn("PDF", r.error)

    def test_unaccepted_image_format_is_415(self):
        c = new_user()
        split = c.make_split()
        r = post_file(c, parse_path(split["id"]), GIF_BYTES,
                      filename="receipt.gif", content_type="image/gif")
        self.assertError(r, 415)

    def test_non_image_body_is_400(self):
        c = new_user()
        split = c.make_split()
        r = post_file(c, parse_path(split["id"]), b"just some words, not a file",
                      filename="receipt.png", content_type="image/png")
        self.assertError(r, 400)

    def test_truncated_image_is_400_not_500(self):
        """Valid magic bytes, corrupt payload: the decoder must fail into a
        400 with a readable message, never a crash and never a 500."""
        c = new_user()
        split = c.make_split()
        r = post_file(c, parse_path(split["id"]), png_bytes()[:40],
                      filename="receipt.png", content_type="image/png")
        self.assertError(r, 400)

    def test_empty_file_is_400(self):
        c = new_user()
        split = c.make_split()
        r = post_file(c, parse_path(split["id"]), b"")
        self.assertError(r, 400)

    def test_missing_file_field_is_400(self):
        c = new_user()
        split = c.make_split()
        r = post_file(c, parse_path(split["id"]), png_bytes(), field="attachment")
        self.assertError(r, 400)

    def test_body_that_is_not_multipart_is_400(self):
        c = new_user()
        split = c.make_split()
        r = c.post(f"/api/splits/{split['id']}/bills/parse", {"file": "nope"})
        self.assertError(r, 400)


# ── the HTML path, end to end ───────────────────────────────────────────────

class TestReceiptParseHtml(ReceiptCase, unittest.TestCase):
    def draft_for(self, currency="USD"):
        c = new_user()
        split = c.make_split(currency=currency)
        r = post_file(c, parse_path(split["id"]), RECEIPT_HTML,
                      filename="order.html", content_type="text/html")
        self.assertStatus(r, 200)
        return c, split, r.json

    def test_html_receipt_parses_into_items(self):
        _, _, draft = self.draft_for()
        self.assertDraftShape(draft)
        self.assertEqual(draft["source"], "html")

        names = [i["name"] for i in draft["items"]]
        self.assertIn("MILK 2%", names)
        self.assertIn("SOURDOUGH LOAF", names)
        self.assertIn("BANANAS", names)

        by_name = {i["name"]: i for i in draft["items"]}
        self.assertEqual(by_name["MILK 2%"]["price"], "3.4900")
        self.assertEqual(by_name["SOURDOUGH LOAF"]["price"], "4.9900")
        self.assertEqual(by_name["BANANAS"]["price"], "1.2900")

    def test_html_keyword_lines_are_metadata_not_items(self):
        """SUBTOTAL/TAX/TOTAL are read as totals, never charged to anyone as
        items — which would double the bill."""
        _, _, draft = self.draft_for()
        names = [i["name"].upper() for i in draft["items"]]
        for keyword in ("SUBTOTAL", "TAX", "TOTAL"):
            self.assertNotIn(keyword, names)
        self.assertEqual(draft["tax"], "0.5200")
        self.assertEqual(draft["total_read"], "10.2900")
        self.assertEqual(draft["items_total"], "9.7700")

    def test_html_negative_line_is_unmatched_never_an_item(self):
        """bill_items.price is CHECK (price >= 0), so a coupon cannot be an
        item; folding it into a neighbour would rewrite a price silently."""
        _, _, draft = self.draft_for()
        self.assertTrue(any("COUPON" in line.upper() for line in draft["unmatched_lines"]),
                        f"coupon line missing from unmatched_lines: {draft['unmatched_lines']}")
        for item in draft["items"]:
            self.assertFalse(item["price"].startswith("-"))

    def test_html_confidence_is_one_because_nothing_was_guessed(self):
        _, _, draft = self.draft_for()
        for item in draft["items"]:
            self.assertEqual(item["confidence"], 1.0)

    def test_currency_comes_from_the_split_not_the_receipt(self):
        """The receipt prints "$". That says nothing about USD vs CAD, and the
        split already knows the answer."""
        _, _, draft = self.draft_for(currency="EUR")
        self.assertEqual(draft["currency"], "EUR")

    def test_script_and_style_contents_do_not_reach_the_draft(self):
        _, _, draft = self.draft_for()
        blob = " ".join(draft["unmatched_lines"] + [i["name"] for i in draft["items"]])
        self.assertNotIn("analytics", blob)
        self.assertNotIn("999.99", blob)
        self.assertNotIn("font-weight", blob)

    def test_parse_persists_nothing(self):
        """The endpoint returns a suggestion. Not one row is written: the user
        confirms a draft through the ordinary bill and item endpoints, which
        run the ordinary validation."""
        c, split, draft = self.draft_for()
        self.assertTrue(draft["items"], "precondition: the draft found items")
        bills = c.get(f"/api/splits/{split['id']}/bills")
        self.assertStatus(bills, 200)
        self.assertEqual(bills.json, [], "parsing a receipt created a bill")


# ── the image path ──────────────────────────────────────────────────────────

class TestReceiptParseImage(ReceiptCase, unittest.TestCase):
    """Contract only. What OCR *reads* is not asserted anywhere in this suite:
    a synthetic image the stdlib can draw carries no legible text, and asserting
    on recognition output would make the suite a flaky measure of Tesseract."""

    def test_valid_png_returns_a_draft(self):
        c = new_user()
        split = c.make_split()
        r = post_file(c, parse_path(split["id"]), png_bytes())
        self.assertStatus(r, 200)
        self.assertDraftShape(r.json)
        self.assertEqual(r.json["source"], "image")
        self.assertEqual(r.json["currency"], split["currency"])

    def test_an_unreadable_image_is_an_empty_draft_with_a_warning(self):
        """A blank page is not an error. It is a draft with nothing in it and
        an explanation — the parser never invents a line it could not read."""
        c = new_user()
        split = c.make_split()
        r = post_file(c, parse_path(split["id"]), png_bytes())
        self.assertStatus(r, 200)
        self.assertEqual(r.json["items"], [])
        self.assertEqual(r.json["items_total"], "0.0000")
        self.assertTrue(r.json["warnings"], "an empty draft must explain itself")

    def test_image_parse_persists_nothing(self):
        c = new_user()
        split = c.make_split()
        self.assertStatus(post_file(c, parse_path(split["id"]), png_bytes()), 200)
        bills = c.get(f"/api/splits/{split['id']}/bills")
        self.assertEqual(bills.json, [])


if __name__ == "__main__":
    unittest.main()


class TestTotalsCrossCheck(ApiTestCase, unittest.TestCase):
    """The cross-check is the parser's most useful output, so its three states
    must stay distinguishable: agreed, disagreed, and never checked."""

    def setUp(self):
        self.c = new_user()
        self.split = self.c.make_split()

    def parse(self, html):
        return post_file(
            self.c, f"/api/splits/{self.split['id']}/bills/parse",
            html.encode(), filename="receipt.html", content_type="text/html")

    def test_a_receipt_whose_items_match_its_total_agrees(self):
        r = self.parse("<html><body><h1>S</h1><p>A 2.00</p><p>B 3.00</p>"
                       "<p>TOTAL 5.00</p></body></html>")
        self.assertStatus(r, 200)
        self.assertEqual(r.json["items_total"], "5.0000")
        self.assertEqual(r.json["total_read"], "5.0000")
        self.assertIs(r.json["totals_agree"], True)
        self.assertEqual(r.json["warnings"], [])

    def test_a_receipt_whose_items_do_not_match_is_flagged_not_reconciled(self):
        r = self.parse("<html><body><h1>S</h1><p>A 2.00</p>"
                       "<p>TOTAL 9.00</p></body></html>")
        self.assertStatus(r, 200)
        # Neither number is adjusted to fit the other.
        self.assertEqual(r.json["items_total"], "2.0000")
        self.assertEqual(r.json["total_read"], "9.0000")
        self.assertIs(r.json["totals_agree"], False)
        self.assertTrue(r.json["warnings"], "a mismatch must carry a warning")

    def test_a_receipt_with_no_total_reports_null_not_true(self):
        r = self.parse("<html><body><h1>S</h1><p>A 2.00</p></body></html>")
        self.assertStatus(r, 200)
        self.assertIsNone(r.json["total_read"])
        self.assertIsNone(r.json["totals_agree"],
                          "claiming agreement without a total to check is a false claim")

    def test_a_coupon_never_becomes_an_item_or_changes_a_price(self):
        r = self.parse("<html><body><h1>S</h1><p>MILK 3.49</p>"
                       "<p>MFR COUPON -0.50</p></body></html>")
        self.assertStatus(r, 200)
        self.assertEqual(len(r.json["items"]), 1)
        self.assertEqual(r.json["items"][0]["price"], "3.4900")
        self.assertTrue(any("COUPON" in u for u in r.json["unmatched_lines"]))
        for item in r.json["items"]:
            self.assertFalse(item["price"].startswith("-"))
