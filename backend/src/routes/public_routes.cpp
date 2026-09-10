#include "routes/public_routes.h"
#include "auth/middleware.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// ── Response helpers ─────────────────────────────────────────────────────────

crow::response json_error(int code, const std::string& message) {
    crow::response res(code, json({{"error", message}}).dump());
    res.add_header("Content-Type", "application/json");
    return res;
}

// The one message an unresolvable token ever produces. A token that never
// existed, a token that was regenerated, and a token that is not even the right
// shape must be indistinguishable: anything else turns the endpoint into an
// oracle for which links used to be valid.
const char* const NOT_FOUND_MESSAGE = "Share link not found";

// ── Input helpers ────────────────────────────────────────────────────────────

// splits.share_token is encode(gen_random_bytes(16), 'hex') — exactly 32
// lowercase hex characters. Anything else cannot be a token, so it is rejected
// before it reaches the database rather than after.
bool is_share_token(const std::string& s) {
    if (s.size() != 32) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// Path parameters are raw user input. Casting a non-UUID string to uuid makes
// Postgres raise, which would surface as a 500, so the shape is checked here
// and anything malformed is treated as "not found".
bool is_uuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

// ── Exact money arithmetic for settlements ───────────────────────────────────
//
// Settlements are the one computed amount Postgres does not derive, because
// they are produced by pairing balances rather than by re-deriving them. The
// pairing is done on integers scaled by 10000 — the same scale NUMERIC(12,4)
// stores — so a transfer is exactly the balance it clears. A double would make
// a settlement a hundredth of a cent off from the number it is supposed to
// cancel, which is precisely the bug the money representation exists to stop.

// Parses "-38.7600" into -387600. Input always comes from a NUMERIC(20,4)
// column, so a parse failure means the query changed shape underneath this
// code; that is a bug, not a user error, and it must not be rounded past.
long long parse_scaled(const std::string& s) {
    std::size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        negative = (s[i] == '-');
        ++i;
    }

    long long units = 0;
    std::size_t digits = 0;
    for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i, ++digits) {
        units = units * 10 + (s[i] - '0');
        if (units > 100000000000LL) throw std::runtime_error("money value out of range");
    }
    if (digits == 0) throw std::runtime_error("unparsable money value: " + s);

    long long fraction = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        int place = 0;
        for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i, ++place) {
            if (place < 4) fraction = fraction * 10 + (s[i] - '0');
        }
        for (; place < 4; ++place) fraction *= 10;
    }
    if (i != s.size()) throw std::runtime_error("unparsable money value: " + s);

    const long long scaled = units * 10000 + fraction;
    return negative ? -scaled : scaled;
}

// The inverse: -387600 back to "-38.7600", always four decimal places.
std::string format_scaled(long long v) {
    const bool negative = v < 0;
    const unsigned long long magnitude =
        negative ? (~static_cast<unsigned long long>(v) + 1ULL)
                 : static_cast<unsigned long long>(v);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%llu.%04llu", negative ? "-" : "",
                  magnitude / 10000ULL, magnitude % 10000ULL);
    return std::string(buf);
}

// ── SQL ──────────────────────────────────────────────────────────────────────
//
// Every query below names the columns it wants. None of them is SELECT *: the
// public payload must not grow a field because a later migration added a
// column, and the only way to guarantee that structurally is for the column
// list to be written out by hand.
//
// splits.id is selected once, to scope the remaining queries; it is never
// serialized. Nothing here reads users.email, split_members.email or
// split_members.user_id at all.

// $1 = share token (already shape-checked)
const char* const SPLIT_BY_TOKEN_SQL =
    "SELECT s.id, s.name, COALESCE(s.description, ''), s.type, s.currency, "
    "       s.created_at, (s.archived_at IS NOT NULL) AS archived "
    "  FROM splits s WHERE s.share_token = $1";

// $1 = split id. payer_name comes from the member row, so the public view names
// who paid without exposing the member id or any account link.
const char* const PUBLIC_BILLS_SQL =
    "SELECT b.store_name, b.date, b.currency, "
    "       (COALESCE((SELECT SUM(i.price * i.quantity) "
    "                    FROM bill_items i WHERE i.bill_id = b.id), 0.0000) "
    "        + b.tax + b.tip + b.fees)::numeric(20,4) AS total, "
    "       pm.name AS payer_name, "
    "       (SELECT COUNT(*) FROM bill_items i WHERE i.bill_id = b.id) AS item_count "
    "  FROM bills b "
    "  LEFT JOIN split_members pm ON pm.id = b.payer_member_id "
    " WHERE b.split_id = $1::uuid "
    " ORDER BY b.date DESC, b.created_at DESC, b.id";

// $1 = split id. Names only — never the member ids, which the UI does not need
// to key a payment row, and never the notes, which are free text the owner
// wrote for himself.
const char* const PUBLIC_PAYMENTS_SQL =
    "SELECT fm.name AS from_name, tm.name AS to_name, "
    "       p.amount::numeric(20,4) AS amount, p.currency, p.method, p.paid_at "
    "  FROM payments p "
    "  LEFT JOIN split_members fm ON fm.id = p.from_member "
    "  LEFT JOIN split_members tm ON tm.id = p.to_member "
    " WHERE p.split_id = $1::uuid "
    " ORDER BY p.paid_at DESC, p.id";

// The shared prefix for the two by_currency queries. $1 = split id.
//
// bill
//   Every bill in the split with its derived subtotal — SUM(price * quantity)
//   over its items, because there is no bills.subtotal column (migration 003).
//
// shares
//   One row per allocation, rounded per row exactly as the per-bill share
//   endpoint reports it. Summing first and rounding once would disagree with
//   the numbers the user was already shown.
//
// bill_member
//   Every (bill, member) pair, including members with no allocations, which the
//   LEFT JOIN turns into 0.0000 rather than dropping. Joining split_members on
//   this split also confines the arithmetic to this split's members.
const char* const BY_CURRENCY_CTE =
    "WITH bill AS ("
    "    SELECT b.id, b.currency, b.tax, b.tip, b.fees, b.payer_member_id, "
    "           COALESCE((SELECT SUM(i.price * i.quantity) "
    "                       FROM bill_items i WHERE i.bill_id = b.id), 0.0000) "
    "               AS subtotal "
    "      FROM bills b WHERE b.split_id = $1::uuid"
    "), "
    "bill_total AS ("
    "    SELECT bl.id, bl.currency, bl.payer_member_id, bl.subtotal, "
    "           (bl.subtotal + bl.tax + bl.tip + bl.fees) AS total "
    "      FROM bill bl"
    "), "
    "shares AS ("
    "    SELECT i.bill_id, a.member_id, "
    "           CASE WHEN a.allocation_mode = 'ratio' "
    "                THEN ROUND(i.price * i.quantity * a.ratio / 100, 4) "
    "                ELSE ROUND(a.amount, 4) END AS share "
    "      FROM item_allocations a "
    "      JOIN bill_items i ON i.id = a.bill_item_id "
    "      JOIN bill bl      ON bl.id = i.bill_id"
    "), "
    "bill_member AS ("
    "    SELECT bl.id AS bill_id, bl.currency, m.id AS member_id, "
    "           COALESCE(SUM(sh.share), 0.0000) AS items "
    "      FROM bill bl "
    "      JOIN split_members m ON m.split_id = $1::uuid "
    "      LEFT JOIN shares sh  ON sh.member_id = m.id AND sh.bill_id = bl.id "
    "     GROUP BY bl.id, bl.currency, m.id"
    "), "
    "bill_alloc AS ("
    "    SELECT bill_id, SUM(items) AS allocated FROM bill_member GROUP BY bill_id"
    ")";

// Per-currency totals and the unallocated block.
//
// The unallocated remainder is the same arithmetic a member's share gets,
// applied to the part of each bill's subtotal nobody is assigned to: its
// proportional tax/tip/fees use the bill's FULL subtotal as the denominator, so
// an unassigned item carries its own tax instead of pushing it onto the people
// who are present. It is computed per bill and then summed, never derived as
// `tax - SUM(members.tax)` — that definition would make the columns tie by
// construction and hide the rounding residual.
//
// GREATEST(..., 0.0000) keeps the remainder from going negative when per-row
// rounding pushes the allocated sum a hundredth of a cent past the subtotal.
//
// NULLIF(subtotal, 0) is the division-by-zero guard: Postgres is never asked to
// divide by zero, and when the subtotal is zero the COALESCE fallback is the
// FULL amount, because nothing can be allocated against a zero subtotal and the
// whole of tax/tip/fees therefore belongs to nobody.
std::string currency_totals_sql() {
    return std::string(BY_CURRENCY_CTE) +
        ", bill_un AS ("
        "    SELECT bl.currency, u.items, "
        "           COALESCE(ROUND(u.items / NULLIF(bl.subtotal, 0) "
        "                          * bl.tax,  4), bl.tax)  AS tax, "
        "           COALESCE(ROUND(u.items / NULLIF(bl.subtotal, 0) "
        "                          * bl.tip,  4), bl.tip)  AS tip, "
        "           COALESCE(ROUND(u.items / NULLIF(bl.subtotal, 0) "
        "                          * bl.fees, 4), bl.fees) AS fees "
        "      FROM bill bl "
        "      LEFT JOIN bill_alloc ba ON ba.bill_id = bl.id "
        "      CROSS JOIN LATERAL ("
        "          SELECT GREATEST(bl.subtotal - COALESCE(ba.allocated, 0.0000), "
        "                          0.0000) AS items"
        "      ) u"
        "), "
        "un AS ("
        "    SELECT currency, SUM(items) AS items, SUM(tax) AS tax, "
        "           SUM(tip) AS tip, SUM(fees) AS fees "
        "      FROM bill_un GROUP BY currency"
        "), "
        // Bills UNION payments: a payment defaults to the split's currency, so
        // in a USD split whose bills are all EUR a recorded settlement would
        // otherwise belong to no block and vanish from every balance. Such a
        // currency reports zero bills and a zero total, which is the truth.
        "cur_all AS ("
        "    SELECT currency FROM bill_total "
        "    UNION "
        "    SELECT currency FROM payments WHERE split_id = $1::uuid"
        ") "
        "SELECT ca.currency, COUNT(bt.currency)::bigint AS bill_count, "
        "       COALESCE(SUM(bt.total), 0.0000)::numeric(20,4) AS total, "
        "       COALESCE(un.items, 0.0000)::numeric(20,4), "
        "       COALESCE(un.tax,   0.0000)::numeric(20,4), "
        "       COALESCE(un.tip,   0.0000)::numeric(20,4), "
        "       COALESCE(un.fees,  0.0000)::numeric(20,4), "
        "       (COALESCE(un.items, 0.0000) + COALESCE(un.tax, 0.0000) "
        "        + COALESCE(un.tip, 0.0000) + COALESCE(un.fees, 0.0000))::numeric(20,4) AS un_total "
        "  FROM cur_all ca "
        "  LEFT JOIN bill_total bt ON bt.currency = ca.currency "
        "  LEFT JOIN un ON un.currency = ca.currency "
        " GROUP BY ca.currency, un.items, un.tax, un.tip, un.fees "
        " ORDER BY ca.currency";
}

// Per-currency, per-member balances.
//
//   balance = fronted + payments_made - owes - payments_received
//
// A member's `owes` is their allocated items plus their proportional part of
// tax, tip and fees, per bill, summed. Every member of the split appears in
// every currency the split's bills use, with zeros when they have no activity,
// because "Bob is on this split and owes nothing" and "Bob is not on this
// split" are different facts.
//
// Currencies come from the split's bills. A payment recorded in a currency no
// bill uses therefore has no row to land in — a split with no bills has no
// balances at all, by the same rule that makes by_currency empty for it.
std::string member_balances_sql() {
    return std::string(BY_CURRENCY_CTE) +
        ", bill_member_owes AS ("
        "    SELECT bm.currency, bm.member_id, "
        "           bm.items "
        "           + COALESCE(ROUND(bm.items / NULLIF(bl.subtotal, 0) "
        "                            * bl.tax,  4), 0.0000) "
        "           + COALESCE(ROUND(bm.items / NULLIF(bl.subtotal, 0) "
        "                            * bl.tip,  4), 0.0000) "
        "           + COALESCE(ROUND(bm.items / NULLIF(bl.subtotal, 0) "
        "                            * bl.fees, 4), 0.0000) AS owes "
        "      FROM bill_member bm JOIN bill bl ON bl.id = bm.bill_id"
        "), "
        "member_owes AS ("
        "    SELECT currency, member_id, SUM(owes) AS owes "
        "      FROM bill_member_owes GROUP BY currency, member_id"
        "), "
        "fronted AS ("
        "    SELECT currency, payer_member_id AS member_id, SUM(total) AS fronted "
        "      FROM bill_total WHERE payer_member_id IS NOT NULL "
        "     GROUP BY currency, payer_member_id"
        "), "
        "paid AS ("
        "    SELECT p.currency, p.from_member AS member_id, SUM(p.amount) AS amount "
        "      FROM payments p "
        "     WHERE p.split_id = $1::uuid AND p.from_member IS NOT NULL "
        "     GROUP BY p.currency, p.from_member"
        "), "
        "received AS ("
        "    SELECT p.currency, p.to_member AS member_id, SUM(p.amount) AS amount "
        "      FROM payments p "
        "     WHERE p.split_id = $1::uuid AND p.to_member IS NOT NULL "
        "     GROUP BY p.currency, p.to_member"
        "), "
        "cur AS (SELECT currency FROM bill "
        "         UNION "
        "         SELECT currency FROM payments WHERE split_id = $1::uuid) "
        "SELECT c.currency, m.id AS member_id, m.name, "
        "       COALESCE(mo.owes,    0.0000)::numeric(20,4) AS owes, "
        "       COALESCE(f.fronted,  0.0000)::numeric(20,4) AS fronted, "
        "       COALESCE(pd.amount,  0.0000)::numeric(20,4) AS payments_made, "
        "       COALESCE(rc.amount,  0.0000)::numeric(20,4) AS payments_received, "
        "       (COALESCE(f.fronted, 0.0000) + COALESCE(pd.amount, 0.0000) "
        "        - COALESCE(mo.owes, 0.0000) - COALESCE(rc.amount, 0.0000)"
        "       )::numeric(20,4) AS balance "
        "  FROM cur c "
        "  JOIN split_members m ON m.split_id = $1::uuid "
        "  LEFT JOIN member_owes mo ON mo.currency = c.currency AND mo.member_id = m.id "
        "  LEFT JOIN fronted f      ON f.currency  = c.currency AND f.member_id  = m.id "
        "  LEFT JOIN paid pd        ON pd.currency = c.currency AND pd.member_id = m.id "
        "  LEFT JOIN received rc    ON rc.currency = c.currency AND rc.member_id = m.id "
        " ORDER BY c.currency, m.joined_at ASC, m.id ASC";
}

// ── Settlements ──────────────────────────────────────────────────────────────

struct Balance {
    std::string member_id;
    std::string name;
    long long   scaled;   // 1/10000ths; positive means the split owes them
};

// Repeatedly matches the largest debtor against the largest creditor. This is
// *a* settlement that clears the balances, not provably the minimal one, and it
// is not promised to be stable across calls.
//
// Rounding residue means the balances need not sum to zero. What can be matched
// is settled and the rest is left alone: inventing a transfer to absorb the
// difference would hand somebody a cent they do not owe.
json settlements_json(const std::vector<Balance>& balances) {
    std::vector<Balance> debtors, creditors;
    for (const auto& b : balances) {
        if (b.scaled < 0) debtors.push_back(b);
        else if (b.scaled > 0) creditors.push_back(b);
    }

    // stable_sort so members with equal balances keep the input order, which is
    // the split's own member order.
    std::stable_sort(debtors.begin(), debtors.end(),
                     [](const Balance& a, const Balance& b) { return a.scaled < b.scaled; });
    std::stable_sort(creditors.begin(), creditors.end(),
                     [](const Balance& a, const Balance& b) { return a.scaled > b.scaled; });

    json out = json::array();
    std::size_t d = 0, c = 0;
    while (d < debtors.size() && c < creditors.size()) {
        const long long owed = -debtors[d].scaled;
        const long long due  = creditors[c].scaled;
        const long long amount = std::min(owed, due);
        if (amount <= 0) break;

        out.push_back({
            {"from_member", debtors[d].member_id},
            {"from_name",   debtors[d].name},
            {"to_member",   creditors[c].member_id},
            {"to_name",     creditors[c].name},
            {"amount",      format_scaled(amount)},
        });

        debtors[d].scaled   += amount;
        creditors[c].scaled -= amount;
        if (debtors[d].scaled == 0) ++d;
        if (creditors[c].scaled == 0) ++c;
    }
    return out;
}

}  // namespace

void register_public_routes(BsApp& app, DbPool& pool) {

    // ── GET /api/splits/share/<token> ────────────────────────────────────────
    //
    // The only unauthenticated endpoint that returns user data. What it leaves
    // out is the specification: no email addresses (owner's or members'), no
    // user_ids, no split_id or any other id that grants access elsewhere, and
    // the token is never echoed back. Member ids appear only inside
    // by_currency, where the UI needs them to key balance and settlement rows.
    //
    // Route note: this pattern and "/api/splits/<string>" cannot collide.
    // Crow's <string> tag stops at '/', so "/api/splits/<string>" cannot match
    // the three segments after /api/splits/share/, and a 32-hex token is never
    // the literal "share".
    CROW_ROUTE(app, "/api/splits/share/<string>").methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req, const std::string& token) {
        (void)req;   // deliberately unused: this route reads no session at all

        crow::response res;
        res.add_header("Content-Type", "application/json");

        if (!is_share_token(token)) return json_error(404, NOT_FOUND_MESSAGE);

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto split_rows = txn.exec(SPLIT_BY_TOKEN_SQL, pqxx::params{token});
            if (split_rows.empty()) {
                txn.commit();
                // Same status and same message as a malformed token: the caller
                // cannot tell "never existed" from "regenerated".
                return json_error(404, NOT_FOUND_MESSAGE);
            }

            const pqxx::row& s = split_rows[0];
            // Used to scope every query below. Never serialized.
            const std::string split_id = s[0].as<std::string>();

            // An archived split still resolves: a link shared before archiving
            // must not break. The flag lets the UI say the split is closed.
            json out;
            out["name"]          = s[1].as<std::string>();
            out["description"]   = s[2].as<std::string>();
            out["type"]          = s[3].as<std::string>();
            out["base_currency"] = s[4].as<std::string>();
            out["created_at"]    = s[5].as<std::string>();
            out["archived"]      = s[6].as<bool>();

            auto bill_rows     = txn.exec(PUBLIC_BILLS_SQL,    pqxx::params{split_id});
            auto totals_rows   = txn.exec(currency_totals_sql(),  pqxx::params{split_id});
            auto balance_rows  = txn.exec(member_balances_sql(),  pqxx::params{split_id});
            auto payment_rows  = txn.exec(PUBLIC_PAYMENTS_SQL, pqxx::params{split_id});
            txn.commit();

            json bills = json::array();
            for (const auto& b : bill_rows) {
                bills.push_back({
                    {"store_name", b[0].as<std::string>()},
                    {"date",       b[1].as<std::string>()},
                    {"currency",   b[2].as<std::string>()},
                    {"total",      b[3].as<std::string>()},
                    {"payer_name", b[4].is_null() ? json(nullptr)
                                                  : json(b[4].as<std::string>())},
                    {"item_count", b[5].as<long long>()},
                });
            }
            out["bills"] = bills;

            // by_currency: one entry per currency the split's bills actually
            // use. Nothing is ever converted or summed across currencies — FX
            // is Phase 7, and adding EUR to USD would invent a number.
            json by_currency = json::array();
            // pqxx::result::size_type is signed; using std::size_t here would
            // be a signed/unsigned comparison against balance_rows.size().
            pqxx::result::size_type balance_pos = 0;
            for (const auto& t : totals_rows) {
                const std::string currency = t[0].as<std::string>();

                // balance_rows is ordered by currency, matching totals_rows, so
                // the two are walked in step rather than re-scanned per entry.
                std::vector<Balance> balances;
                json members = json::array();
                while (balance_pos < balance_rows.size() &&
                       balance_rows[balance_pos][0].as<std::string>() == currency) {
                    const auto& m = balance_rows[balance_pos];
                    members.push_back({
                        {"member_id",         m[1].as<std::string>()},
                        {"name",              m[2].as<std::string>()},
                        {"owes",              m[3].as<std::string>()},
                        {"fronted",           m[4].as<std::string>()},
                        {"payments_made",     m[5].as<std::string>()},
                        {"payments_received", m[6].as<std::string>()},
                        {"balance",           m[7].as<std::string>()},
                    });
                    balances.push_back(Balance{m[1].as<std::string>(),
                                               m[2].as<std::string>(),
                                               parse_scaled(m[7].as<std::string>())});
                    ++balance_pos;
                }

                by_currency.push_back({
                    {"currency",    currency},
                    {"bill_count",  t[1].as<long long>()},
                    {"total",       t[2].as<std::string>()},
                    {"unallocated", {
                        {"items", t[3].as<std::string>()},
                        {"tax",   t[4].as<std::string>()},
                        {"tip",   t[5].as<std::string>()},
                        {"fees",  t[6].as<std::string>()},
                        {"total", t[7].as<std::string>()},
                    }},
                    {"members",     members},
                    {"settlements", settlements_json(balances)},
                });
            }
            out["by_currency"]    = by_currency;
            out["mixed_currency"] = by_currency.size() > 1;

            json payments = json::array();
            for (const auto& p : payment_rows) {
                payments.push_back({
                    {"from_name", p[0].is_null() ? json(nullptr)
                                                 : json(p[0].as<std::string>())},
                    {"to_name",   p[1].is_null() ? json(nullptr)
                                                 : json(p[1].as<std::string>())},
                    {"amount",    p[2].as<std::string>()},
                    {"currency",  p[3].as<std::string>()},
                    {"method",    p[4].is_null() ? json(nullptr)
                                                 : json(p[4].as<std::string>())},
                    {"paid_at",   p[5].as<std::string>()},
                });
            }
            out["payments"] = payments;

            res.code = 200;
            res.body = out.dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "public_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── POST /api/splits/<id>/share/regenerate ───────────────────────────────
    //
    // Owner only; a non-owner gets the same 404 as a nonexistent split. The new
    // token replaces the old one in place, so the previous link stops resolving
    // the moment this commits — there is no grace period and no second valid
    // token. Archived splits can still be regenerated: revoking a link that was
    // shared before archiving is exactly when an owner wants this.
    CROW_ROUTE(app, "/api/splits/<string>/share/regenerate")
        .methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req, const std::string& id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id)) return json_error(404, "Split not found");

        try {
            auto conn = pool.acquire();

            // share_token is UNIQUE. A collision between two 128-bit random
            // values is not a real event, but the constraint can still fire, and
            // handing the owner a 500 for something a retry fixes would be
            // wrong. Each attempt is its own transaction because a failed
            // statement poisons the one it ran in.
            for (int attempt = 0; attempt < 3; ++attempt) {
                try {
                    pqxx::work txn(*conn);
                    auto rows = txn.exec(
                        "UPDATE splits "
                        "   SET share_token = encode(gen_random_bytes(16), 'hex') "
                        " WHERE id = $1::uuid AND owner_id = $2::uuid "
                        " RETURNING share_token",
                        pqxx::params{id, user->id});

                    if (rows.empty()) {
                        txn.commit();
                        return json_error(404, "Split not found");
                    }
                    txn.commit();

                    res.code = 200;
                    res.body = json({{"share_token", rows[0][0].as<std::string>()}}).dump();
                    return res;
                } catch (const pqxx::unique_violation& e) {
                    CROW_LOG_WARNING << "public_routes: share_token collision, retrying: "
                                     << e.what();
                }
            }
            return json_error(500, "Could not generate a unique share token");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "public_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
    });
}
