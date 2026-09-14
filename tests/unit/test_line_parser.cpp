// Receipt line parser — text in, draft out.
//
// The parser is a pile of heuristics over text that a camera and an OCR engine
// have already mangled, so the interesting cases are the ugly ones: a coupon
// that must never become an item, a price whose decimal point was eaten, a
// receipt whose own total disagrees with its own lines. Those are what this
// file is mostly made of.
#include <doctest/doctest.h>

#include "services/receipt/line_parser.h"

#include <cstdio>
#include <string>
#include <vector>

using receipt::Draft;
using receipt::ParsedItem;

namespace {

Draft parse(const std::vector<std::string>& lines,
            const std::vector<double>& confidences = {}) {
    return receipt::parse_lines(lines, confidences);
}

bool any_line_contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& l : lines) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Every draft, no matter how garbled the input, must satisfy these. A confirmed
// draft goes through POST /bills/:id/items, which rejects a negative price, a
// blank price and a quantity below 1 — a draft that cannot pass that is a bug
// here, not there.
void check_universal_invariants(const Draft& d) {
    for (const auto& it : d.items) {
        CHECK(!it.price.empty());
        CHECK(it.price.find('-') == std::string::npos);
        const auto dot = it.price.find('.');
        REQUIRE(dot != std::string::npos);
        CHECK(it.price.size() - dot - 1 == 4);   // always 4dp, always NUMERIC(12,4)-shaped
        CHECK(it.quantity >= 1);
        CHECK(it.confidence >= 0.0);
        CHECK(it.confidence <= 1.0);
        CHECK(it.name.size() <= 200);
    }
    CHECK(!d.items_total.empty());
    CHECK(d.items_total.find('-') == std::string::npos);
}

}  // namespace

// ── The recognition table, row by row ─────────────────────────────────────────

TEST_CASE("name followed by a trailing price is an item") {
    const Draft d = parse({"MILK 2%      3.49"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "MILK 2%");
    CHECK(d.items[0].price == "3.4900");
    CHECK(d.items[0].quantity == 1);
    CHECK(d.items_total == "3.4900");
    check_universal_invariants(d);
}

TEST_CASE("a trailing tax flag is stripped, not read as part of the name") {
    for (const char* flag : {"T", "F", "N", "TF", "*"}) {
        const Draft d = parse({std::string("MILK 2%      3.49 ") + flag});
        REQUIRE(d.items.size() == 1);
        CHECK(d.items[0].name == "MILK 2%");
        CHECK(d.items[0].price == "3.4900");
    }
}

TEST_CASE("an explicit quantity line yields quantity and unit price") {
    const Draft d = parse({"CANNED BEANS", "2 @ 1.75      3.50"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "CANNED BEANS");
    CHECK(d.items[0].price == "1.7500");   // price is the UNIT price
    CHECK(d.items[0].quantity == 2);
    CHECK(d.items_total == "3.5000");      // line total is price * quantity
    CHECK(d.warnings.empty());
    check_universal_invariants(d);
}

TEST_CASE("a quantity line with no extended price uses the printed unit price") {
    const Draft d = parse({"LIMES", "4 @ 0.50"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "LIMES");
    CHECK(d.items[0].price == "0.5000");
    CHECK(d.items[0].quantity == 4);
    CHECK(d.items_total == "2.0000");
}

TEST_CASE("a quantity line that does not multiply out is reported, not reconciled") {
    // 2 x 1.75 is 3.50, but the receipt says 3.75. Both numbers are printed;
    // the parser refuses to decide which one is the typo.
    const Draft d = parse({"BEANS", "2 @ 1.75      3.75"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].price == "1.7500");
    CHECK(d.items[0].quantity == 2);
    REQUIRE(d.warnings.size() == 1);
    CHECK(any_line_contains(d.warnings, "does not multiply out"));
}

TEST_CASE("weighted goods become one item at the extended price") {
    const Draft d = parse({"BANANAS", "0.87 lb @ 2.99/lb   2.60"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "BANANAS");
    CHECK(d.items[0].price == "2.6000");   // never 0.87 as a quantity: quantity is a count
    CHECK(d.items[0].quantity == 1);
    check_universal_invariants(d);
}

TEST_CASE("weighted goods in other units and spacings") {
    const Draft d = parse({"CHEESE", "0.42 KG @ 18.50 / KG   7.77",
                           "APPLES", "2.5 lbs @ 1.20/lb   3.00"});
    REQUIRE(d.items.size() == 2);
    CHECK(d.items[0].name == "CHEESE");
    CHECK(d.items[0].price == "7.7700");
    CHECK(d.items[1].name == "APPLES");
    CHECK(d.items[1].price == "3.0000");
}

TEST_CASE("a name on one line and its price on the next are joined") {
    const Draft d = parse({"SAFEWAY", "ORGANIC BABY SPINACH", "4.99"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "ORGANIC BABY SPINACH");
    CHECK(d.items[0].price == "4.9900");
    CHECK(d.store_name == "SAFEWAY");
}

TEST_CASE("a wrapped name only reaches back one line") {
    // The address must not become the name of the price two lines below it.
    const Draft d = parse({"SAFEWAY", "123 MAIN ST", "", "6.50"});
    CHECK(d.items.empty());
    CHECK(any_line_contains(d.unmatched_lines, "6.50"));
}

TEST_CASE("keyword lines are metadata, never items") {
    const Draft d = parse({"BREAD            3.49",
                           "SUBTOTAL         3.49",
                           "TAX              0.29",
                           "TOTAL            3.78",
                           "VISA             3.78",
                           "AUTH #123456",
                           "CHANGE           0.00",
                           "CASH             0.00",
                           "DEBIT            0.00",
                           "BALANCE          0.00"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "BREAD");
    CHECK(d.tax == "0.2900");
    CHECK(d.total_read == "3.7800");
    CHECK(d.items_total == "3.4900");
    CHECK(d.totals_agree);
    CHECK(d.warnings.empty());
}

TEST_CASE("a negative amount is never an item") {
    const Draft d = parse({"BREAD            3.49", "MFR COUPON  -0.50"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "BREAD");
    CHECK(d.items[0].price == "3.4900");     // the coupon did NOT rewrite it
    CHECK(d.items_total == "3.4900");
    REQUIRE(d.unmatched_lines.size() == 1);
    CHECK(d.unmatched_lines[0] == "MFR COUPON  -0.50");
    check_universal_invariants(d);
}

TEST_CASE("negatives in every notation receipts use") {
    const Draft d = parse({"ITEM ONE   -1.00", "ITEM TWO   1.00-", "ITEM THREE (1.00)",
                           "ITEM FOUR  -$1.00"});
    CHECK(d.items.empty());
    CHECK(d.unmatched_lines.size() == 4);
    CHECK(d.items_total == "0.0000");
}

TEST_CASE("a positive discount line is still not an item") {
    // From the spec's own example: MEMBER SAVINGS is an amount, but it is not a
    // thing that was bought, and bill_items has nowhere to put it.
    const Draft d = parse({"MEMBER SAVINGS 1.20", "TOTAL SAVINGS 4.32",
                           "STORE COUPON 2.00"});
    CHECK(d.items.empty());
    CHECK(d.unmatched_lines.size() == 3);
    CHECK(d.total_read.empty());     // "TOTAL SAVINGS" is not the total
}

TEST_CASE("lines with no price are ignored, not reported as unmatched") {
    const Draft d = parse({"SAFEWAY STORE #1234", "123 MAIN ST", "ANYTOWN, CA 90210",
                           "(555) 123-4567", "MEMBER SINCE 2019 - THANK YOU"});
    CHECK(d.items.empty());
    CHECK(d.unmatched_lines.empty());
    CHECK(d.items_total == "0.0000");
}

// ── A whole receipt ───────────────────────────────────────────────────────────

TEST_CASE("a realistic grocery receipt, item by item") {
    const Draft d = parse({
        "SAFEWAY",
        "STORE #1842   (555) 555-0123",
        "1500 MARKET ST, SAN FRANCISCO CA",
        "",
        "09/01/2026  14:22   REG 04   OP 118",
        "",
        "MILK 2%              3.49 T",
        "ORGANIC EGGS 12CT    5.99 T",
        "SOURDOUGH LOAF",
        "4.25",
        "BANANAS",
        "1.32 lb @ 0.69/lb    0.91",
        "SPARKLING WATER",
        "6 @ 0.99             5.94",
        "MFR COUPON          -1.00",
        "MEMBER SAVINGS       0.75",
        "",
        "SUBTOTAL            20.58",
        "TAX                  1.42",
        "TOTAL               22.00",
        "VISA CHIP           22.00",
        "AUTH 004512   APPROVED",
        "CHANGE               0.00",
        "",
        "ITEMS SOLD 5",
        "THANK YOU FOR SHOPPING SAFEWAY",
    });

    CHECK(d.store_name == "SAFEWAY");
    CHECK(d.date == "2026-09-01");

    REQUIRE(d.items.size() == 5);

    CHECK(d.items[0].name == "MILK 2%");
    CHECK(d.items[0].price == "3.4900");
    CHECK(d.items[0].quantity == 1);

    CHECK(d.items[1].name == "ORGANIC EGGS 12CT");
    CHECK(d.items[1].price == "5.9900");
    CHECK(d.items[1].quantity == 1);

    CHECK(d.items[2].name == "SOURDOUGH LOAF");
    CHECK(d.items[2].price == "4.2500");
    CHECK(d.items[2].quantity == 1);

    CHECK(d.items[3].name == "BANANAS");
    CHECK(d.items[3].price == "0.9100");
    CHECK(d.items[3].quantity == 1);

    CHECK(d.items[4].name == "SPARKLING WATER");
    CHECK(d.items[4].price == "0.9900");
    CHECK(d.items[4].quantity == 6);

    // 3.49 + 5.99 + 4.25 + 0.91 + 6*0.99 = 20.58
    CHECK(d.items_total == "20.5800");
    CHECK(d.tax == "1.4200");
    CHECK(d.tip.empty());
    CHECK(d.total_read == "22.0000");
    CHECK(d.totals_agree);
    CHECK(d.warnings.empty());

    REQUIRE(d.unmatched_lines.size() == 2);
    CHECK(d.unmatched_lines[0] == "MFR COUPON          -1.00");
    CHECK(d.unmatched_lines[1] == "MEMBER SAVINGS       0.75");

    check_universal_invariants(d);
}

TEST_CASE("a restaurant check with a tip") {
    const Draft d = parse({"THE CORNER BISTRO",
                           "2026-08-14",
                           "BURGER               14.00",
                           "FRIES                 5.00",
                           "TIP                   3.80",
                           "TOTAL                22.80"});
    REQUIRE(d.items.size() == 2);
    CHECK(d.date == "2026-08-14");
    CHECK(d.items_total == "19.0000");
    CHECK(d.tip == "3.8000");
    CHECK(d.tax.empty());
    CHECK(d.total_read == "22.8000");
    CHECK(d.totals_agree);
}

// ── Money notation ────────────────────────────────────────────────────────────

TEST_CASE("a comma decimal separator is a decimal separator") {
    const Draft d = parse({"KAFFEE          3,49", "BROT            2,00", "SUMME"});
    REQUIRE(d.items.size() == 2);
    CHECK(d.items[0].price == "3.4900");
    CHECK(d.items[1].price == "2.0000");
    CHECK(d.items_total == "5.4900");
}

TEST_CASE("currency symbols are stripped from the amount") {
    const Draft d = parse({"COFFEE   $3.49", "TEA      \xC2\xA3""2.50", "CAKE  \xE2\x82\xAC""4,25"});
    REQUIRE(d.items.size() == 3);
    CHECK(d.items[0].price == "3.4900");
    CHECK(d.items[1].price == "2.5000");
    CHECK(d.items[2].price == "4.2500");
}

TEST_CASE("thousands separators are understood in both conventions") {
    const Draft d = parse({"MACBOOK PRO      1,299.99", "GRAND TOTAL      1,299.99"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].price == "1299.9900");
    CHECK(d.total_read == "1299.9900");
    CHECK(d.totals_agree);

    const Draft e = parse({"LAPTOP        1.299,99"});
    REQUIRE(e.items.size() == 1);
    CHECK(e.items[0].price == "1299.9900");

    const Draft f = parse({"SERVER     12,345,678.90"});
    REQUIRE(f.items.size() == 1);
    CHECK(f.items[0].price == "12345678.9000");
}

TEST_CASE("four decimal places survive; three are too ambiguous to trust") {
    const Draft d = parse({"WIDGET   1.2345"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].price == "1.2345");

    // "1.234" is a thousands group as often as it is a price, and guessing
    // wrong changes the amount by a factor of a thousand.
    const Draft e = parse({"WIDGET   1.234"});
    CHECK(e.items.empty());
}

TEST_CASE("malformed and out-of-range amounts are not prices") {
    for (const char* line : {"WIDGET 12.34.56", "WIDGET 1,23,456.78", "WIDGET 123456789.00",
                             "WIDGET 1.2.3", "WIDGET ..", "WIDGET 12,345"}) {
        const Draft d = parse({line});
        CHECK(d.items.empty());
    }
}

TEST_CASE("a zero price is a legal item; the DB only forbids negatives") {
    const Draft d = parse({"FREE SAMPLE   0.00"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].price == "0.0000");
    check_universal_invariants(d);
}

// ── The totals cross-check ────────────────────────────────────────────────────

TEST_CASE("items that do not add up to the printed total raise a warning") {
    // A line was dropped by the scanner: the items sum to 5.98 but the receipt
    // says 12.50. Both numbers are reported; neither is edited to fit.
    const Draft d = parse({"MILK      3.49", "BREAD     2.49", "TAX       0.52",
                           "TOTAL    12.50"});
    CHECK(d.items_total == "5.9800");
    CHECK(d.tax == "0.5200");
    CHECK(d.total_read == "12.5000");
    CHECK_FALSE(d.totals_agree);
    REQUIRE(d.warnings.size() == 1);
    CHECK(any_line_contains(d.warnings, "do not add up"));
}

TEST_CASE("with no printed total there is nothing to disagree with") {
    const Draft d = parse({"MILK      3.49", "BREAD     2.49"});
    CHECK(d.total_read.empty());
    CHECK(d.totals_agree);
    CHECK(d.warnings.empty());
}

TEST_CASE("the cross-check is exact in integer units, not floating point") {
    // 0.10 + 0.20 is not 0.30 in binary floating point. It is here.
    const Draft d = parse({"A 0.10", "B 0.20", "TOTAL 0.30"});
    CHECK(d.items_total == "0.3000");
    CHECK(d.totals_agree);

    const Draft e = parse({"A 0.10", "B 0.20", "TOTAL 0.31"});
    CHECK_FALSE(e.totals_agree);
}

TEST_CASE("multiple tax lines are summed") {
    const Draft d = parse({"SODA 1.00", "TAX 1 0.05", "TAX 2 0.03", "TOTAL 1.08"});
    CHECK(d.tax == "0.0800");
    CHECK(d.totals_agree);
}

TEST_CASE("the first printed total wins over the payment lines that echo it") {
    const Draft d = parse({"SODA 1.00", "TOTAL 1.00", "VISA 1.00", "TOTAL DUE 0.00"});
    CHECK(d.total_read == "1.0000");
    CHECK(d.totals_agree);
}

TEST_CASE("a keyword whose amount wrapped to the next line is still metadata") {
    const Draft d = parse({"SODA 1.00", "TOTAL", "1.00"});
    CHECK(d.items.size() == 1);
    CHECK(d.total_read == "1.0000");
    CHECK(d.unmatched_lines.empty());
    CHECK(d.totals_agree);
}

TEST_CASE("items_total is exact over a long receipt") {
    // 40 items at 0.03, 0.10, 0.17 ... 2.76 — prices chosen so that every one of
    // them is inexact in binary floating point. The sum is 55.80 on the nose.
    std::vector<std::string> lines;
    long long expected_cents = 0;
    for (int i = 0; i < 40; ++i) {
        const long long c = 3 + 7 * i;
        expected_cents += c;
        char buf[64];
        std::snprintf(buf, sizeof buf, "ITEM %02d           %lld.%02lld", i, c / 100, c % 100);
        lines.push_back(buf);
    }
    CHECK(expected_cents == 5580);
    char total[64];
    std::snprintf(total, sizeof total, "TOTAL   %lld.%02lld", expected_cents / 100,
                  expected_cents % 100);
    lines.push_back(total);

    const Draft d = parse(lines);
    REQUIRE(d.items.size() == 40);
    CHECK(d.items_total == "55.8000");
    CHECK(d.total_read == "55.8000");
    CHECK(d.totals_agree);
    CHECK(d.warnings.empty());
    check_universal_invariants(d);
}

TEST_CASE("quantities multiply into items_total exactly") {
    const Draft d = parse({"NAILS", "144 @ 0.07", "SCREWS", "37 @ 0.13"});
    REQUIRE(d.items.size() == 2);
    // 144*0.07 = 10.08, 37*0.13 = 4.81
    CHECK(d.items_total == "14.8900");
}

// ── Degenerate input ──────────────────────────────────────────────────────────

TEST_CASE("empty input produces an empty draft, not a crash") {
    const Draft d = parse({});
    CHECK(d.items.empty());
    CHECK(d.items_total == "0.0000");
    CHECK(d.store_name.empty());
    CHECK(d.date.empty());
    CHECK(d.tax.empty());
    CHECK(d.tip.empty());
    CHECK(d.total_read.empty());
    CHECK(d.totals_agree);
    CHECK(d.unmatched_lines.empty());
    CHECK(d.warnings.empty());
    check_universal_invariants(d);
}

TEST_CASE("blank and whitespace-only lines are skipped") {
    const Draft d = parse({"", "   ", "\t", "MILK 1.00", "   ", ""});
    REQUIRE(d.items.size() == 1);
    CHECK(d.unmatched_lines.empty());
}

TEST_CASE("a receipt of nothing but a store header") {
    const Draft d = parse({"TRADER JOE'S", "STORE 142", "SAN FRANCISCO, CA"});
    CHECK(d.store_name == "TRADER JOE'S");
    CHECK(d.items.empty());
    CHECK(d.items_total == "0.0000");
    CHECK(d.unmatched_lines.empty());
    CHECK(d.warnings.empty());
}

TEST_CASE("a receipt with no prices at all") {
    const Draft d = parse({"THANK YOU", "PLEASE COME AGAIN", "SURVEY AT EXAMPLE COM"});
    CHECK(d.items.empty());
    CHECK(d.items_total == "0.0000");
    CHECK(d.totals_agree);
}

TEST_CASE("a line that is only a price, with no name anywhere, is unmatched") {
    const Draft d = parse({"3.49"});
    CHECK(d.items.empty());
    REQUIRE(d.unmatched_lines.size() == 1);
    CHECK(d.unmatched_lines[0] == "3.49");
}

TEST_CASE("a price with a structural quantity but no name is kept, and flagged") {
    // The "2 @" proves the line is a purchase; the name is the one thing missing,
    // and the user can type it. Nothing is guessed.
    const Draft d = parse({"2 @ 1.75      3.50"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name.empty());
    CHECK(d.items[0].price == "1.7500");
    CHECK(d.items[0].quantity == 2);
    CHECK(any_line_contains(d.warnings, "no readable name"));
    check_universal_invariants(d);
}

TEST_CASE("a numeric-only name is not a name") {
    const Draft d = parse({"0123456   5.99"});
    CHECK(d.items.empty());
    CHECK(any_line_contains(d.unmatched_lines, "0123456"));
}

TEST_CASE("a leading UPC is dropped from the name it precedes") {
    const Draft d = parse({"011110001234 MILK 2%    3.49"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "MILK 2%");
}

TEST_CASE("an implausible quantity is not a quantity") {
    const Draft d = parse({"200000 @ 0.01   2000.00"});
    CHECK(d.items.empty());
    CHECK(d.unmatched_lines.size() == 1);
}

// ── OCR damage ────────────────────────────────────────────────────────────────

TEST_CASE("a garbled name is kept verbatim; the parser does not correct spelling") {
    const Draft d = parse({"8REAO             3.49"}, {0.42});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "8REAO");          // not "BREAD" — guessing is not its job
    CHECK(d.items[0].price == "3.4900");
    CHECK(d.items[0].confidence == doctest::Approx(0.42));
}

TEST_CASE("a dropped decimal point is not silently repaired") {
    // "349" could be 3.49, 34.90, or a store number. Inventing one of them puts a
    // number in front of the user that no one ever printed; leaving the line out
    // shows up in the totals cross-check instead, which is visible.
    const Draft d = parse({"BREAD    349", "TOTAL   3.49"});
    CHECK(d.items.empty());
    CHECK(d.items_total == "0.0000");
    CHECK_FALSE(d.totals_agree);
    CHECK(any_line_contains(d.warnings, "do not add up"));
}

TEST_CASE("doubled spaces, dot leaders and stray punctuation") {
    const Draft d = parse({"MILK   2%      ....    3.49",
                           "BREAD -------- 2.50",
                           "EGGS ......... 4.00",
                           "***  TOTAL  ***   9.99"});
    REQUIRE(d.items.size() == 3);
    CHECK(d.items[0].name == "MILK 2%");
    CHECK(d.items[1].name == "BREAD");
    CHECK(d.items[2].name == "EGGS");
    CHECK(d.total_read == "9.9900");
    CHECK(d.totals_agree);           // 3.49 + 2.50 + 4.00, dot leaders and all
}

TEST_CASE("a colon between the label and its amount") {
    const Draft d = parse({"SODA: 1.00", "TAX: 0.08", "TOTAL: 1.08"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "SODA");
    CHECK(d.tax == "0.0800");
    CHECK(d.total_read == "1.0800");
    CHECK(d.totals_agree);
}

TEST_CASE("stray letters after the price are treated as a flag, not a name") {
    const Draft d = parse({"MILK 2%   3.49 TF", "BREAD     2.50 EA"});
    REQUIRE(d.items.size() == 2);
    CHECK(d.items[0].price == "3.4900");
    CHECK(d.items[1].price == "2.5000");
}

TEST_CASE("an item whose name starts with a keyword-like word is still an item") {
    const Draft d = parse({"TIP TOP BREAD      3.49"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "TIP TOP BREAD");
    CHECK(d.tip.empty());
}

// ── Confidence ────────────────────────────────────────────────────────────────

TEST_CASE("confidence is carried through per line") {
    const Draft d = parse({"MILK  3.49", "BREAD 2.50"}, {0.94, 0.31});
    REQUIRE(d.items.size() == 2);
    CHECK(d.items[0].confidence == doctest::Approx(0.94));
    CHECK(d.items[1].confidence == doctest::Approx(0.31));
}

TEST_CASE("missing confidence data means fully legible") {
    const Draft d = parse({"MILK  3.49", "BREAD 2.50"});
    REQUIRE(d.items.size() == 2);
    CHECK(d.items[0].confidence == doctest::Approx(1.0));
    CHECK(d.items[1].confidence == doctest::Approx(1.0));
}

TEST_CASE("out-of-range confidence is clamped, and short vectors do not overrun") {
    const Draft d = parse({"MILK 3.49", "BREAD 2.50", "EGGS 4.00"}, {-5.0, 7.0});
    REQUIRE(d.items.size() == 3);
    CHECK(d.items[0].confidence == doctest::Approx(0.0));
    CHECK(d.items[1].confidence == doctest::Approx(1.0));
    CHECK(d.items[2].confidence == doctest::Approx(1.0));
    check_universal_invariants(d);
}

TEST_CASE("a wrapped item takes the lower confidence of its two lines") {
    const Draft d = parse({"STORE", "ORGANIC SPINACH", "4.99"}, {1.0, 0.55, 0.90});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].confidence == doctest::Approx(0.55));
}

// ── Dates and store names ─────────────────────────────────────────────────────

TEST_CASE("dates in the formats receipts print") {
    CHECK(parse({"09/01/2026"}).date == "2026-09-01");
    CHECK(parse({"9/1/26"}).date == "2026-09-01");
    CHECK(parse({"2026-09-01"}).date == "2026-09-01");
    CHECK(parse({"01.09.2026"}).date == "2026-01-09");   // month-first when plausible
    CHECK(parse({"25/12/2026"}).date == "2026-12-25");   // day-first only when it must be
    CHECK(parse({"DATE 09/01/2026 14:22:07"}).date == "2026-09-01");
}

TEST_CASE("impossible dates are not dates") {
    CHECK(parse({"13/32/2026"}).date.empty());
    CHECK(parse({"02/30/2026"}).date.empty());
    CHECK(parse({"(555) 123-4567"}).date.empty());
    CHECK(parse({"TOTAL 12.50"}).date.empty());
    CHECK(parse({"REG 4-1-1"}).date.empty());            // a 1-digit year is not a year
}

TEST_CASE("the store name is the first header line, and may be absent") {
    CHECK(parse({"WHOLE FOODS MARKET", "MILK 3.49"}).store_name == "WHOLE FOODS MARKET");
    CHECK(parse({"MILK 3.49"}).store_name.empty());
    CHECK(parse({}).store_name.empty());
}

// ── Invariants over everything above ──────────────────────────────────────────

TEST_CASE("no receipt shape can produce a negative, blank or malformed price") {
    const std::vector<std::vector<std::string>> receipts = {
        {},
        {"MFR COUPON -0.50"},
        {"TOTAL -12.00"},
        {"-3.49"},
        {"(4.00)"},
        {"1.00-"},
        {"WIDGET -0.0001"},
        {"@ @ @"},
        {"0 @ 0"},
        {"1 @ -1.00   -1.00"},
        {"...", "---", "***"},
        {"\xE2\x82\xAC", "$", "$$$"},
        {"A 0.0000"},
        {"99999999.9999"},
        {"ITEM 99999999.9999"},
        {"ITEM", "100000 @ 99999999.9999"},
    };
    for (const auto& lines : receipts) {
        const Draft d = parse(lines);
        check_universal_invariants(d);
        for (const auto& it : d.items) {
            CHECK(it.price != "");
            CHECK(it.price[0] != '-');
        }
    }
}

TEST_CASE("a very long name is clamped to what the item endpoint accepts") {
    const std::string long_name(400, 'X');
    const Draft d = parse({long_name + "   1.00"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name.size() == 200);
    check_universal_invariants(d);
}

TEST_CASE("an absurdly long input is truncated rather than parsed forever") {
    std::vector<std::string> lines(2500, "MILK 1.00");
    const Draft d = parse(lines);
    CHECK(d.items.size() == 2000);
    CHECK(d.items_total == "2000.0000");
    CHECK(any_line_contains(d.warnings, "longer than 2000 lines"));
    check_universal_invariants(d);
}

// ── OCR punctuation damage ───────────────────────────────────────────────────
// Tesseract reads a printed "-" as an em dash often enough that the negative
// -amount rule must not depend on getting ASCII. Before normalization these
// lines fell into unmatched_lines only because they parsed as no amount at all.

TEST_CASE("a coupon written with an em dash is still recognized as negative") {
    const auto d = receipt::parse_lines({
        "MILK 2%           3.49",
        "MFR COUPON       \xE2\x80\x94" "0.50",   // U+2014 EM DASH
        "TOTAL             2.99",
    });
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].name == "MILK 2%");
    CHECK(d.items[0].price == "3.4900");
    REQUIRE(d.unmatched_lines.size() == 1);
    CHECK(d.unmatched_lines[0].find("COUPON") != std::string::npos);
}

TEST_CASE("en dash and the unicode minus sign are treated the same way") {
    for (const char* dash : {"\xE2\x80\x93", "\xE2\x88\x92"}) {  // U+2013, U+2212
        const auto d = receipt::parse_lines({
            "BREAD             2.99",
            std::string("STORE CREDIT     ") + dash + "1.00",
        });
        REQUIRE(d.items.size() == 1);
        CHECK(d.items[0].price == "2.9900");
        CHECK(d.unmatched_lines.size() == 1);
    }
}

TEST_CASE("a no-break space does not split a price from its name") {
    const auto d = receipt::parse_lines({"MILK\xC2\xA0" "2%\xC2\xA0    3.49"});
    REQUIRE(d.items.size() == 1);
    CHECK(d.items[0].price == "3.4900");
    CHECK(d.items[0].name.find("MILK") != std::string::npos);
}
