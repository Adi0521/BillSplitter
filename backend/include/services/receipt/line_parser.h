#pragma once

#include <string>
#include <vector>

// Receipt line parser — the heuristics layer of receipt import.
//
// Pure text in, draft out: no database, no network, no OCR. The OCR engine (or
// an HTML export reader) produces one string per visual line of the receipt and
// hands them here; what comes back is a *suggestion* the user reviews and edits.
// Nothing in this file writes anything anywhere.
//
// Unlike the rest of the backend's helpers, this lives in a named namespace
// rather than an anonymous one inside a .cpp, so tests/unit can drive it
// directly. That is the entire reason it is a separate component.
//
// Money is never a double here. Prices are decimal strings normalized to the
// four decimal places of the NUMERIC(12,4) columns they will eventually be
// confirmed into, and every sum and comparison is done in integer units of
// 1/10000 (a cent is 100 of them). `confidence` is the only floating-point
// value in the interface, and it is a legibility hint from OCR, not an amount.
namespace receipt {

// One proposed line item. Nothing has been saved; the user may edit or drop it.
struct ParsedItem {
    std::string name;              // may be "" when a price had no readable name
    std::string price;             // UNIT price, 4dp decimal string, e.g. "3.4900"
    int         quantity = 1;      // >= 1; a line's contribution is price * quantity
    double      confidence = 1.0;  // 0..1, passed through from OCR; 1.0 when unknown
};

// The whole parse. Every string field is "" when the receipt did not say.
struct Draft {
    std::string store_name;                  // best guess, "" when unknown
    std::string date;                        // "YYYY-MM-DD", "" when unknown
    std::vector<ParsedItem> items;

    std::string tax;                         // read off the receipt, "" when absent
    std::string tip;                         // read off the receipt, "" when absent
    std::string total_read;                  // what the receipt CLAIMS the total is

    std::string items_total = "0.0000";      // what the parsed items actually sum to
    bool        totals_agree = true;         // items_total + tax + tip == total_read

    std::vector<std::string> unmatched_lines;  // read, but not turnable into an item
    std::vector<std::string> warnings;         // plain English, for the user
};

// Parses a receipt.
//
// `lines` is one string per visual line, in reading order. `confidences` is
// parallel to `lines` and carries the OCR engine's mean word confidence for
// each; an empty vector (or a short one) means "no confidence data", which is
// treated as 1.0. Extra confidences are ignored.
//
// The parser never invents a line it could not read: an ambiguous line is
// reported in `unmatched_lines`, never as an item with a guessed price.
Draft parse_lines(const std::vector<std::string>& lines,
                  const std::vector<double>& confidences = {});

}  // namespace receipt
