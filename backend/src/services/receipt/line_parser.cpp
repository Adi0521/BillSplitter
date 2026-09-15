#include "services/receipt/line_parser.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace receipt {
namespace {

// ── Money ─────────────────────────────────────────────────────────────────────
//
// Amounts are held as `long long` counts of 1/10000 of a currency unit — the
// exact scale of the NUMERIC(12,4) columns a confirmed draft lands in, and the
// same scale a cent lives at (1 cent == 100 units). Parsing, summing and the
// totals cross-check are all integer; no amount is ever a double, so nothing
// here can drift by a hundredth and quietly change what a user owes.

constexpr long long SCALE      = 10000;         // units per currency unit
constexpr long long MAX_UNITS  = 999999999999LL;  // 99999999.9999, the NUMERIC(12,4) ceiling
constexpr long long MAX_QUANTITY = 100000;      // matches the item endpoint's limit
constexpr size_t    MAX_NAME_LEN = 200;         // matches the item endpoint's limit
constexpr size_t    MAX_LINES    = 2000;        // a receipt, not a novel
constexpr size_t    MAX_DECIMALS = 4;

// Renders units as the canonical 4dp decimal string Postgres will store.
std::string money_to_string(long long units) {
    if (units < 0) units = 0;  // never reached: negatives are rejected upstream
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld.%04lld", units / SCALE, units % SCALE);
    return std::string(buf);
}

// ── Small string helpers ──────────────────────────────────────────────────────

std::string trim(const std::string& s) {
    const char* ws = " \t\n\r\f\v";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

std::string upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }

size_t count_letters(const std::string& s) {
    size_t n = 0;
    for (char c : s) {
        if (is_alpha(c)) ++n;
    }
    return n;
}

std::string join(const std::vector<std::string>& tokens, size_t begin, size_t end) {
    std::string out;
    for (size_t i = begin; i < end && i < tokens.size(); ++i) {
        if (!out.empty()) out += ' ';
        out += tokens[i];
    }
    return out;
}

// Splits a line into tokens, normalizing the punctuation receipts use as
// column separators: dot and dash leaders ("MILK.....3.49"), colons
// ("TOTAL:6.50") and the "@" of quantity lines all become whitespace or
// standalone tokens. A *single* '-' survives, because it is a minus sign.
std::vector<std::string> tokenize(const std::string& line) {
    std::string flat;
    flat.reserve(line.size() + 8);
    for (size_t i = 0; i < line.size();) {
        const char c = line[i];
        if (c == '.' || c == '-' || c == '_' || c == '=' || c == '*' || c == '~') {
            size_t j = i;
            while (j < line.size() && line[j] == c) ++j;
            if (j - i >= 2) {  // a run is decoration, not punctuation
                flat += ' ';
                i = j;
                continue;
            }
        }
        if (c == ':' || c == '\t' || c == '|') {
            flat += ' ';
            ++i;
            continue;
        }
        if (c == '@') {
            flat += " @ ";
            ++i;
            continue;
        }
        flat += c;
        ++i;
    }

    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < flat.size()) {
        while (i < flat.size() && std::isspace(static_cast<unsigned char>(flat[i]))) ++i;
        size_t start = i;
        while (i < flat.size() && !std::isspace(static_cast<unsigned char>(flat[i]))) ++i;
        if (i > start) tokens.push_back(flat.substr(start, i - start));
    }
    return tokens;
}

// ── Amount parsing ────────────────────────────────────────────────────────────

// Parses one whitespace-delimited token as a money amount, returning the value
// in 1/10000 units plus whether it was negative.
//
// Accepts a leading or trailing currency symbol (including multi-byte ones like
// € and £, which arrive as bytes with the high bit set), '.' or ',' as the
// decimal separator, the other of the two as a thousands separator, and a
// leading '-', a trailing '-' or parentheses for negatives.
//
// Deliberately rejects a token with no decimal separator at all: on a receipt,
// a bare integer is a store number, a UPC, an item count or an OCR drop of the
// decimal point at least as often as it is a price, and guessing which would
// mean inventing a number the user never saw. Three decimal places are rejected
// for the same reason — "1.234" is a thousands group as often as a price.
bool parse_amount(const std::string& token, long long& units, bool& negative) {
    negative = false;
    std::string s = token;

    auto is_symbol = [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return c == '$' || u >= 0x80;
    };

    for (bool changed = true; changed;) {
        changed = false;
        if (!s.empty() && is_symbol(s.front()))            { s.erase(s.begin()); changed = true; }
        if (!s.empty() && is_symbol(s.back()))             { s.pop_back();       changed = true; }
        if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
            negative = negative || s.front() == '-';
            s.erase(s.begin());
            changed = true;
        }
        if (!s.empty() && s.back() == '-')                 { negative = true; s.pop_back(); changed = true; }
        if (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
            negative = true;
            s = s.substr(1, s.size() - 2);
            changed = true;
        }
    }
    if (s.empty()) return false;

    size_t last_sep = std::string::npos;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '.' || s[i] == ',') {
            last_sep = i;
        } else if (!is_digit(s[i])) {
            return false;
        }
    }
    if (last_sep == std::string::npos) return false;  // a bare integer is not a price

    const size_t frac_len = s.size() - last_sep - 1;
    if (frac_len < 1 || frac_len > MAX_DECIMALS || frac_len == 3) return false;

    // Everything left of the decimal separator must be digits in groups of
    // three delimited by the *other* separator character. "12.34.56" and
    // "1,23,456.78" are ambiguous, not amounts.
    const char decimal_sep  = s[last_sep];
    const char grouping_sep = (decimal_sep == '.') ? ',' : '.';
    std::string int_digits;
    size_t since_group = 0;
    bool grouped = false;
    for (size_t i = 0; i < last_sep; ++i) {
        const char c = s[i];
        if (is_digit(c)) {
            int_digits += c;
            ++since_group;
        } else if (c == grouping_sep) {
            if (!grouped) {
                if (since_group < 1 || since_group > 3) return false;
            } else if (since_group != 3) {
                return false;
            }
            grouped = true;
            since_group = 0;
        } else {
            return false;  // the decimal separator appearing twice
        }
    }
    if (grouped && since_group != 3) return false;

    std::string frac_digits = s.substr(last_sep + 1);
    if (int_digits.empty() && frac_digits.empty()) return false;

    const size_t first_significant = int_digits.find_first_not_of('0');
    int_digits = (first_significant == std::string::npos) ? "" : int_digits.substr(first_significant);
    if (int_digits.size() > 8) return false;  // beyond NUMERIC(12,4)

    long long whole = 0;
    for (char c : int_digits) whole = whole * 10 + (c - '0');

    frac_digits.append(MAX_DECIMALS - frac_digits.size(), '0');
    long long frac = 0;
    for (char c : frac_digits) frac = frac * 10 + (c - '0');

    units = whole * SCALE + frac;
    return units <= MAX_UNITS;
}

// A plain non-negative number, decimal or integer — used for the weight of a
// weighed line ("0.87 lb @ ..."), never for a price.
bool looks_numeric(const std::string& token) {
    bool seen_digit = false;
    size_t separators = 0;
    for (char c : token) {
        if (is_digit(c)) {
            seen_digit = true;
        } else if (c == '.' || c == ',') {
            if (++separators > 1) return false;
        } else {
            return false;
        }
    }
    return seen_digit;
}

// A whole count, in the range the item endpoint will accept.
bool parse_quantity(const std::string& token, long long& out) {
    if (token.empty() || token.size() > 7) return false;
    long long v = 0;
    for (char c : token) {
        if (!is_digit(c)) return false;
        v = v * 10 + (c - '0');
    }
    if (v < 1 || v > MAX_QUANTITY) return false;
    out = v;
    return true;
}

// The tax/department flag receipts print after a price: "3.49 T", "1.29 F".
bool is_flag(const std::string& token) {
    if (token.empty() || token.size() > 2) return false;
    for (char c : token) {
        if (!is_alpha(c) && c != '*') return false;
    }
    return true;
}

bool is_unit_word(const std::string& token) {
    static const char* kUnits[] = {"LB", "LBS", "POUND", "POUNDS", "KG",  "KGS",
                                   "G",  "GR",  "GRAM",  "GRAMS",  "OZ",  "EA",
                                   "EACH", "CT", "GAL", "L", "ML"};
    const std::string u = upper(token);
    for (const char* w : kUnits) {
        if (u == w) return true;
    }
    return false;
}

// ── Keyword lines ─────────────────────────────────────────────────────────────

enum class Meta {
    None,      // an ordinary line: maybe an item
    Total,     // what the receipt claims the total is
    Tax,
    Tip,
    Discount,  // coupon / savings: an amount, but never an item
    Other      // payment, change, subtotal, auth: metadata we read past
};

bool word_at(const std::string& s, size_t pos, const std::string& word) {
    if (pos + word.size() > s.size()) return false;
    if (s.compare(pos, word.size(), word) != 0) return false;
    if (pos > 0 && is_alpha(s[pos - 1])) return false;
    const size_t end = pos + word.size();
    if (end < s.size() && is_alpha(s[end])) return false;
    return true;
}

bool has_word(const std::string& s, const std::string& word) {
    for (size_t p = s.find(word); p != std::string::npos; p = s.find(word, p + 1)) {
        if (word_at(s, p, word)) return true;
    }
    return false;
}

bool starts_word(const std::string& s, const std::string& word) { return word_at(s, 0, word); }

// Classifies the text to the left of a line's trailing price.
//
// `name_count` is how many tokens that text is. The amount-bearing categories
// (Total/Tax/Tip) additionally require a short label, because a long line that
// merely contains the word "TIP" is far more likely to be "TIP TOP BREAD 3.49"
// than a gratuity — and misreading an item as the total would corrupt the one
// cross-check this parser exists to provide.
Meta classify(const std::vector<std::string>& tokens, size_t name_count) {
    if (name_count == 0) return Meta::None;

    std::string head = upper(join(tokens, 0, name_count));
    size_t b = 0;
    while (b < head.size() && !is_alpha(head[b])) ++b;
    head = head.substr(b);
    if (head.empty()) return Meta::None;

    // A discount is an amount that is not an item and not metadata either: it
    // cannot be a bill_item (price >= 0) and folding it into a neighbouring
    // item would rewrite a price the user never saw. Surface it as text.
    static const char* kDiscount[] = {"COUPON",  "SAVINGS", "SAVED",  "DISCOUNT",
                                      "PROMO",   "PROMOTION", "REBATE", "REWARD",
                                      "REWARDS", "MFR",     "MANUFACTURER"};
    for (const char* w : kDiscount) {
        if (has_word(head, w)) return Meta::Discount;
    }

    if (starts_word(head, "SUBTOTAL") || starts_word(head, "SUB TOTAL")) return Meta::Other;

    // "TIP" gets a tighter leash than the rest: "TIP TOP BREAD 3.49" is a loaf
    // of bread, and reading it as a gratuity would delete an item.
    if (name_count <= 2 && (starts_word(head, "TIP") || starts_word(head, "GRATUITY"))) {
        return Meta::Tip;
    }
    if (name_count <= 3) {
        if (has_word(head, "TAX") || starts_word(head, "VAT") || starts_word(head, "GST") ||
            starts_word(head, "HST") || starts_word(head, "PST") || starts_word(head, "QST")) {
            return Meta::Tax;
        }
        if (has_word(head, "TOTAL")) {
            static const char* kNotATotal[] = {"ITEM",  "ITEMS", "NUMBER", "COUNT",
                                               "QTY",   "SOLD",  "PIECES"};
            for (const char* w : kNotATotal) {
                if (has_word(head, w)) return Meta::Other;
            }
            return Meta::Total;
        }
    }

    static const char* kOther[] = {
        "SUBTOTAL", "TOTAL",  "CHANGE",  "CASH",    "VISA",   "MASTERCARD", "AMEX",
        "DISCOVER", "DEBIT",  "CREDIT",  "CARD",    "AUTH",   "AUTHORIZATION",
        "APPROVED", "APPROVAL", "BALANCE", "TENDER", "TENDERED", "PAYMENT",
        "ACCOUNT",  "ACCT",   "AMOUNT",  "DUE",     "REF",    "REFERENCE",
        "TRANS",    "TRANSACTION", "TERMINAL", "MERCHANT", "CHIP", "AID",
        "SIGNATURE", "ROUNDING", "EBT", "SNAP", "CASHBACK"};
    for (const char* w : kOther) {
        if (starts_word(head, w)) return Meta::Other;
    }
    return Meta::None;
}

// ── Dates ─────────────────────────────────────────────────────────────────────

bool valid_ymd(int y, int m, int d) {
    if (y < 1970 || y > 2100 || m < 1 || m > 12 || d < 1) return false;
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int limit = kDays[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) limit = 29;
    return d <= limit;
}

// Finds the first date in a line, in either "YYYY-MM-DD" or the "MM/DD/YY(YY)"
// receipts print. Day-first is only assumed when month-first is impossible
// (first field > 12), which is the least-surprising reading of an ambiguous
// pair and is why the result is a suggestion the user confirms.
bool find_date(const std::string& line, std::string& out) {
    auto digits_at = [&line](size_t i, size_t max_len, std::string& got) {
        got.clear();
        while (i < line.size() && got.size() < max_len && is_digit(line[i])) got += line[i++];
        return i;
    };

    for (size_t i = 0; i < line.size(); ++i) {
        if (!is_digit(line[i])) continue;
        if (i > 0 && is_digit(line[i - 1])) continue;

        std::string a, b, c;
        size_t p = digits_at(i, 4, a);
        if (a.empty() || p >= line.size()) continue;
        const char sep = line[p];
        if (sep != '/' && sep != '-' && sep != '.') continue;
        p = digits_at(p + 1, 2, b);
        if (b.empty() || p >= line.size() || line[p] != sep) continue;
        p = digits_at(p + 1, 4, c);
        if (c.size() < 2) continue;
        if (p < line.size() && is_digit(line[p])) continue;

        int y = 0, m = 0, d = 0;
        if (a.size() == 4) {
            y = std::stoi(a);
            m = std::stoi(b);
            d = std::stoi(c);
        } else {
            const int first = std::stoi(a);
            const int second = std::stoi(b);
            y = std::stoi(c);
            if (c.size() == 2) y += (y < 80) ? 2000 : 1900;
            if (first > 12 && second <= 12) {
                d = first;
                m = second;
            } else {
                m = first;
                d = second;
            }
        }
        if (!valid_ymd(y, m, d)) continue;

        // valid_ymd() has already bounded these to a real calendar date, but
        // size for the widest an int can print ("-2147483648" is 11 chars, so
        // 3 fields + 2 dashes + NUL = 36). That makes the buffer provably big
        // enough rather than safe-by-inference from a check two lines up —
        // which is also what stops GCC's -Wformat-truncation firing.
        char buf[40];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
        out = buf;
        return true;
    }
    return false;
}

// ── Names ─────────────────────────────────────────────────────────────────────

// Builds an item name from the tokens left of the price: drops a leading UPC,
// strips decorative punctuation and clamps to what the item endpoint accepts,
// so a confirmed draft does not fail validation on length.
std::string clean_name(const std::vector<std::string>& tokens, size_t count) {
    size_t begin = 0;
    if (count >= 2 && tokens[0].size() >= 8 && looks_numeric(tokens[0]) &&
        tokens[0].find_first_of(".,") == std::string::npos) {
        // A leading run of >= 8 digits is a UPC, not part of the name — but only
        // drop it when something with letters is left behind.
        for (size_t i = 1; i < count; ++i) {
            if (count_letters(tokens[i]) > 0) {
                begin = 1;
                break;
            }
        }
    }

    std::string name = join(tokens, begin, count);
    const char* junk = " \t#*-.,";
    const size_t b = name.find_first_not_of(junk);
    if (b == std::string::npos) return "";
    const size_t e = name.find_last_not_of(junk);
    name = name.substr(b, e - b + 1);
    if (name.size() > MAX_NAME_LEN) name = name.substr(0, MAX_NAME_LEN);
    return name;
}

double confidence_at(const std::vector<double>& confidences, size_t i) {
    if (i >= confidences.size()) return 1.0;
    const double v = confidences[i];
    if (!(v >= 0.0)) return 0.0;  // also catches NaN
    return (v > 1.0) ? 1.0 : v;
}

}  // namespace

// OCR routinely returns typographic punctuation where a receipt printed ASCII:
// Tesseract reads a coupon's "-0.50" as an em dash, "—0.50", which then parses
// as no amount at all. That happened to land the line in unmatched_lines, but
// only by accident — the negative-amount rule never ran. Normalizing first makes
// that outcome deliberate, and stops a stray non-breaking space from splitting a
// price off its name.
//
// Handled (UTF-8): U+2010..U+2015 dashes and U+2212 minus -> '-';
// U+00A0 no-break space -> ' '.
std::string normalize_punctuation(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0xE2 && i + 2 < in.size()) {
            const unsigned char b1 = static_cast<unsigned char>(in[i + 1]);
            const unsigned char b2 = static_cast<unsigned char>(in[i + 2]);
            if ((b1 == 0x80 && b2 >= 0x90 && b2 <= 0x95) ||   // U+2010..U+2015
                (b1 == 0x88 && b2 == 0x92)) {                  // U+2212 minus
                out += '-';
                i += 3;
                continue;
            }
        }
        if (c == 0xC2 && i + 1 < in.size() &&
            static_cast<unsigned char>(in[i + 1]) == 0xA0) {   // U+00A0 nbsp
            out += ' ';
            i += 2;
            continue;
        }
        out += in[i];
        ++i;
    }
    return out;
}

Draft parse_lines(const std::vector<std::string>& lines,
                  const std::vector<double>& confidences) {
    Draft draft;

    // Normalize before anything reads a line, so every downstream rule — the
    // negative-amount check most of all — sees ASCII punctuation.
    std::vector<std::string> normalized;
    normalized.reserve(lines.size());
    for (const auto& raw : lines) normalized.push_back(normalize_punctuation(raw));
    const std::vector<std::string>& lines_in = normalized;

    long long items_units = 0;
    long long tax_units = 0, tip_units = 0, total_units = 0;
    bool have_tax = false, have_tip = false, have_total = false;

    // A name whose price is on the next line, or a keyword whose amount is on
    // the next line. Only ever one line deep: a wrapped name is adjacent to its
    // price, and reaching further back is how an address becomes an item.
    enum class Pending { None, Name, Field };
    Pending pending = Pending::None;
    std::string pending_name;
    double pending_confidence = 1.0;
    Meta pending_field = Meta::None;

    const size_t line_count = std::min(lines_in.size(), MAX_LINES);
    if (lines_in.size() > line_count) {
        draft.warnings.push_back(
            "The receipt was longer than " + std::to_string(MAX_LINES) +
            " lines; only the first " + std::to_string(MAX_LINES) + " were parsed.");
    }

    for (size_t i = 0; i < line_count; ++i) {
        const std::string raw = trim(lines_in[i]);
        const double confidence = confidence_at(confidences, i);

        if (draft.date.empty()) find_date(raw, draft.date);

        if (raw.empty()) {
            pending = Pending::None;
            continue;
        }
        const std::vector<std::string> tokens = tokenize(raw);
        if (tokens.empty()) {
            pending = Pending::None;
            continue;
        }

        // 1. The trailing price, if the line has one, plus the tokens left of it.
        long long units = 0;
        bool negative = false;
        bool has_price = false;
        size_t name_count = tokens.size();
        {
            const size_t last = tokens.size() - 1;
            long long u = 0;
            bool n = false;
            if (parse_amount(tokens[last], u, n)) {
                has_price = true;
                units = u;
                negative = n;
                name_count = last;
            } else if (tokens.size() >= 2 && is_flag(tokens[last]) &&
                       parse_amount(tokens[last - 1], u, n)) {
                has_price = true;
                units = u;
                negative = n;
                name_count = last - 1;
            }
        }

        const Meta meta = classify(tokens, name_count);

        // 2. Coupons and savings: an amount that can never be an item.
        if (meta == Meta::Discount) {
            if (has_price) draft.unmatched_lines.push_back(raw);
            pending = Pending::None;
            continue;
        }

        // 3. Keyword lines are metadata, not items.
        if (meta != Meta::None) {
            if (has_price) {
                if (!negative) {
                    if (meta == Meta::Total && !have_total) {
                        total_units = units;
                        have_total = true;
                    } else if (meta == Meta::Tax) {
                        tax_units += units;   // receipts often print two or three tax lines
                        have_tax = true;
                    } else if (meta == Meta::Tip) {
                        tip_units += units;
                        have_tip = true;
                    }
                }
                pending = Pending::None;
            } else {
                pending = Pending::Field;    // "TOTAL" with its amount on the next line
                pending_field = meta;
            }
            continue;
        }

        // 4. The amount belonging to a keyword line above it.
        if (pending == Pending::Field) {
            if (has_price && name_count == 0) {
                if (!negative) {
                    if (pending_field == Meta::Total && !have_total) {
                        total_units = units;
                        have_total = true;
                    } else if (pending_field == Meta::Tax) {
                        tax_units += units;
                        have_tax = true;
                    } else if (pending_field == Meta::Tip) {
                        tip_units += units;
                        have_tip = true;
                    }
                }
                pending = Pending::None;
                continue;
            }
            pending = Pending::None;
        }

        // 5. No price on the line: a store header, an address, a phone number,
        //    loyalty blurb — or the first half of a wrapped item. Ignored either
        //    way unless the very next line turns out to be a bare price.
        if (!has_price) {
            if (count_letters(raw) >= 2) {
                if (draft.store_name.empty() && i < 5) {
                    draft.store_name = clean_name(tokens, tokens.size());
                }
                pending = Pending::Name;
                pending_name = clean_name(tokens, tokens.size());
                pending_confidence = confidence;
            } else {
                pending = Pending::None;
            }
            continue;
        }

        // 6. A negative amount is never an item: bill_items.price is CHECK
        //    (price >= 0), and quietly subtracting it from a neighbouring line
        //    would rewrite a price the user never saw.
        if (negative) {
            draft.unmatched_lines.push_back(raw);
            pending = Pending::None;
            continue;
        }

        // 7. Quantity and weight structure, left of the price.
        int quantity = 1;
        long long unit_units = units;
        bool structured = false;
        std::string line_warning;

        size_t at = std::string::npos;
        for (size_t k = 0; k < name_count; ++k) {
            if (tokens[k] == "@") {
                at = k;
                break;
            }
        }
        if (at != std::string::npos && at >= 1) {
            long long unit_price = 0;
            bool unit_negative = false;
            long long count = 0;
            if (at >= 2 && is_unit_word(tokens[at - 1]) && looks_numeric(tokens[at - 2])) {
                // Weighted goods: "0.87 lb @ 2.99/lb   2.60". The weight is not a
                // quantity — bill_items.quantity is a whole count — so the line
                // becomes one item at its extended price, exactly as printed.
                quantity = 1;
                unit_units = units;
                name_count = at - 2;
                structured = true;
            } else if (parse_quantity(tokens[at - 1], count)) {
                bool ok = true;
                if (at + 1 < name_count) {
                    if (parse_amount(tokens[at + 1], unit_price, unit_negative) && !unit_negative) {
                        unit_units = unit_price;
                        if (count * unit_price != units) {
                            // Report the disagreement; do not pick a winner.
                            line_warning = "A quantity line does not multiply out: \"" + raw + "\".";
                        }
                    } else {
                        ok = false;  // "@" was part of the name after all
                    }
                } else {
                    unit_units = units;  // "2 @ 1.75" with no extended price printed
                }
                if (ok) {
                    quantity = static_cast<int>(count);
                    name_count = at - 1;
                    structured = true;
                }
            }
        }

        if (unit_units > MAX_UNITS || unit_units * quantity > MAX_UNITS) {
            draft.unmatched_lines.push_back(raw);  // implausible for a receipt line
            pending = Pending::None;
            continue;
        }

        // 8. The name, possibly wrapped from the line above.
        std::string name = clean_name(tokens, name_count);
        double item_confidence = confidence;
        if (name.empty() && pending == Pending::Name) {
            name = pending_name;
            item_confidence = std::min(confidence, pending_confidence);
        }
        pending = Pending::None;

        if (name.empty()) {
            if (!structured) {
                // A bare number with nothing around it. It could be a price, a
                // total, or a fragment; guessing would invent an item.
                draft.unmatched_lines.push_back(raw);
                continue;
            }
            line_warning = "A line had a price but no readable name: \"" + raw + "\".";
        } else if (count_letters(name) == 0) {
            draft.unmatched_lines.push_back(raw);
            continue;
        }

        ParsedItem item;
        item.name = name;
        item.price = money_to_string(unit_units);
        item.quantity = quantity;
        item.confidence = item_confidence;
        draft.items.push_back(item);
        items_units += unit_units * quantity;
        if (!line_warning.empty()) draft.warnings.push_back(line_warning);
    }

    draft.items_total = money_to_string(items_units);
    if (have_tax) draft.tax = money_to_string(tax_units);
    if (have_tip) draft.tip = money_to_string(tip_units);

    // The cross-check. total_read is what the receipt claims; items_total is
    // what the items add to. Both are reported and neither is adjusted to fit
    // the other: a mismatch is the signal that a line was dropped or misread,
    // and reconciling it would throw that signal away.
    if (have_total) {
        draft.total_read = money_to_string(total_units);
        draft.totals_agree = (items_units + tax_units + tip_units == total_units);
        if (!draft.totals_agree) {
            draft.warnings.push_back(
                "The parsed items do not add up to the total printed on the receipt. "
                "Some lines may be missing or misread.");
        }
    }

    return draft;
}

}  // namespace receipt
