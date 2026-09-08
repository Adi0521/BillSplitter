#include "routes/share_routes.h"
#include "auth/middleware.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <cctype>
#include <cstddef>
#include <string>

using json = nlohmann::json;

namespace {

// ── Response helpers ─────────────────────────────────────────────────────────

crow::response json_error(int code, const std::string& message) {
    crow::response res(code, json({{"error", message}}).dump());
    res.add_header("Content-Type", "application/json");
    return res;
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

// ── SQL ──────────────────────────────────────────────────────────────────────
//
// Two queries run in one transaction: the bill-level totals and the per-member
// rows. They share the CTE prefix below so the two answers cannot drift apart.
//
//   $1 = bill id, $2 = split id, $3 = owner (session) user id
//
// bill
//   The ownership join. Zero rows here means "no such bill for this user", so
//   another user's bill and a nonexistent one are indistinguishable (404).
//   subtotal is derived as SUM(price * quantity) over the bill's items; there
//   is no bills.subtotal column (dropped in migration 003). The 0.0000 fallback
//   is written with four decimals so an empty bill renders "0.0000", not "0".
//
// shares
//   One row per allocation on this bill, carrying what that allocation is
//   actually worth. The share is rounded per allocation row, exactly as the
//   allocation endpoints report it, and the rounded values are then summed —
//   summing first and rounding once would disagree with the per-item numbers
//   the user was shown.
//
// member_items
//   Every member of the split, including those with no allocations, which the
//   LEFT JOIN turns into 0.0000 rather than dropping the row. The UI needs to
//   show who is not yet on the bill.
const char* const SHARES_CTE =
    "WITH bill AS ("
    "    SELECT b.id, b.split_id, b.currency, b.tax, b.tip, b.fees, "
    "           b.payer_member_id, "
    "           COALESCE((SELECT SUM(i.price * i.quantity) "
    "                       FROM bill_items i WHERE i.bill_id = b.id), 0.0000) "
    "               AS subtotal "
    "      FROM bills b JOIN splits s ON s.id = b.split_id "
    "     WHERE b.id = $1::uuid AND b.split_id = $2::uuid "
    "       AND s.owner_id = $3::uuid"
    "), "
    "shares AS ("
    "    SELECT a.member_id, "
    "           CASE WHEN a.allocation_mode = 'ratio' "
    "                THEN ROUND(i.price * i.quantity * a.ratio / 100, 4) "
    "                ELSE ROUND(a.amount, 4) END AS share "
    "      FROM item_allocations a "
    "      JOIN bill_items i ON i.id = a.bill_item_id "
    "      JOIN bill bl      ON bl.id = i.bill_id"
    "), "
    "member_items AS ("
    "    SELECT m.id AS member_id, m.name, m.joined_at, "
    "           COALESCE(SUM(sh.share), 0.0000) AS items "
    "      FROM bill bl "
    "      JOIN split_members m ON m.split_id = bl.split_id "
    "      LEFT JOIN shares sh  ON sh.member_id = m.id "
    "     GROUP BY m.id, m.name, m.joined_at"
    ")";

// Bill-level row: the totals plus the unallocated remainder.
//
// allocated is summed over member_items rather than over shares, so it is by
// construction the sum of the members[] array in the same response. (An
// allocation pointing at a member of another split cannot be created by the
// API; if one existed it would be excluded from both, not from one of them.)
//
// The unallocated block is the same arithmetic applied to the part of the
// subtotal nobody is on the hook for: its proportional tax/tip/fees use the
// bill's FULL subtotal as the denominator, exactly as a member's do. That is
// what makes an unassigned item carry its own tax instead of pushing it onto
// the people who are present.
//
// GREATEST(..., 0.0000) keeps the remainder from going negative when per-row
// rounding pushes the allocated sum a hundredth of a cent past the subtotal.
//
// NULLIF(subtotal, 0) is the division-by-zero guard: a bill with no items, or
// with everything priced zero, makes the division yield NULL. Postgres is never
// asked to divide by zero.
//
// In this block the COALESCE fallback is the full amount rather than 0.0000.
// When the subtotal is zero nothing is allocated to anybody, so every member's
// proportional share is zero and the whole of tax, tip and fees belongs here --
// otherwise a bill of nothing but tax would report charges that sit in no
// bucket at all. Read as a proportion, the unallocated fraction of a zero
// subtotal is 1.
std::string bill_totals_sql() {
    return std::string(SHARES_CTE) +
        ", agg AS (SELECT COALESCE(SUM(items), 0.0000) AS allocated "
        "            FROM member_items) "
        "SELECT bl.id, bl.currency, bl.subtotal, bl.tax, bl.tip, bl.fees, "
        "       (bl.subtotal + bl.tax + bl.tip + bl.fees) AS total, "
        "       agg.allocated, "
        "       un.items, un.tax, un.tip, un.fees, "
        "       (un.items + un.tax + un.tip + un.fees) AS un_total, "
        "       bl.payer_member_id "
        "  FROM bill bl "
        "  CROSS JOIN agg "
        "  CROSS JOIN LATERAL ("
        "      SELECT u.items, "
        "             COALESCE(ROUND(u.items / NULLIF(bl.subtotal, 0) "
        "                            * bl.tax,  4), bl.tax) AS tax, "
        "             COALESCE(ROUND(u.items / NULLIF(bl.subtotal, 0) "
        "                            * bl.tip,  4), bl.tip) AS tip, "
        "             COALESCE(ROUND(u.items / NULLIF(bl.subtotal, 0) "
        "                            * bl.fees, 4), bl.fees) AS fees "
        "        FROM (SELECT GREATEST(bl.subtotal - agg.allocated, 0.0000) "
        "                         AS items) u"
        "  ) un";
}

// Per-member rows, ordered like GET /api/splits/:id/members so the two lists
// line up in the UI. The id tiebreaker keeps the order stable for members that
// share a joined_at.
//
// owes_payer is the member's total, except that the payer does not owe himself,
// and when the bill has no payer nobody owes anybody yet.
std::string member_shares_sql() {
    return std::string(SHARES_CTE) +
        " SELECT mi.member_id, mi.name, mi.items, p.tax, p.tip, p.fees, "
        "        (mi.items + p.tax + p.tip + p.fees) AS total, "
        "        CASE WHEN bl.payer_member_id IS NULL "
        "               OR bl.payer_member_id = mi.member_id "
        "             THEN 0.0000 "
        "             ELSE (mi.items + p.tax + p.tip + p.fees) END AS owes_payer "
        "   FROM member_items mi "
        "   CROSS JOIN bill bl "
        "   CROSS JOIN LATERAL ("
        "       SELECT COALESCE(ROUND(mi.items / NULLIF(bl.subtotal, 0) "
        "                             * bl.tax,  4), 0.0000) AS tax, "
        "              COALESCE(ROUND(mi.items / NULLIF(bl.subtotal, 0) "
        "                             * bl.tip,  4), 0.0000) AS tip, "
        "              COALESCE(ROUND(mi.items / NULLIF(bl.subtotal, 0) "
        "                             * bl.fees, 4), 0.0000) AS fees "
        "   ) p "
        "  ORDER BY mi.joined_at ASC, mi.member_id ASC";
}

// ── Serialization ────────────────────────────────────────────────────────────

// Every NUMERIC column is read with as<std::string>() and emitted as a JSON
// string: "1.5572", never 1.5572. No monetary value becomes a double here.
json member_share_to_json(const pqxx::row& r) {
    json j;
    j["member_id"]  = r[0].as<std::string>();
    j["name"]       = r[1].as<std::string>();
    j["items"]      = r[2].as<std::string>();
    j["tax"]        = r[3].as<std::string>();
    j["tip"]        = r[4].as<std::string>();
    j["fees"]       = r[5].as<std::string>();
    j["total"]      = r[6].as<std::string>();
    j["owes_payer"] = r[7].as<std::string>();
    return j;
}

}  // namespace

void register_share_routes(BsApp& app, DbPool& pool) {

    // ── GET /api/splits/<id>/bills/<bid>/shares ──────────────────────────────
    // What each member owes on one bill: their allocated items plus their
    // proportional part of tax, tip and fees, with whatever nobody is assigned
    // to reported separately.
    CROW_ROUTE(app, "/api/splits/<string>/bills/<string>/shares")
        .methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req, const std::string& id,
             const std::string& bid) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id) || !is_uuid(bid)) return json_error(404, "Bill not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto totals = txn.exec(bill_totals_sql(),
                                   pqxx::params{bid, id, user->id});
            if (totals.empty()) {
                txn.commit();
                return json_error(404, "Bill not found");
            }

            // Safe without a second ownership check, and correct to run in the
            // same transaction: the bill above is already proven to belong to
            // the caller, and both queries see the same snapshot, so the member
            // rows cannot be aggregated from items the totals did not see.
            auto members = txn.exec(member_shares_sql(),
                                    pqxx::params{bid, id, user->id});
            txn.commit();

            const pqxx::row& t = totals[0];

            json out;
            out["bill_id"]              = t[0].as<std::string>();
            out["currency"]             = t[1].as<std::string>();
            out["subtotal"]             = t[2].as<std::string>();
            out["tax"]                  = t[3].as<std::string>();
            out["tip"]                  = t[4].as<std::string>();
            out["fees"]                 = t[5].as<std::string>();
            out["total"]                = t[6].as<std::string>();
            out["allocated_subtotal"]   = t[7].as<std::string>();
            out["unallocated_subtotal"] = t[8].as<std::string>();
            out["payer_member_id"]      = t[13].is_null()
                                              ? json(nullptr)
                                              : json(t[13].as<std::string>());

            json member_array = json::array();
            for (const auto& row : members) {
                member_array.push_back(member_share_to_json(row));
            }
            out["members"] = member_array;

            // Reported, not reconciled: SUM(members[].total) + unallocated.total
            // ties with total only up to the per-allocation rounding residual,
            // and hiding that residual by handing it to somebody would be the
            // bug the rounding rule exists to prevent.
            out["unallocated"] = {
                {"items", t[8].as<std::string>()},
                {"tax",   t[9].as<std::string>()},
                {"tip",   t[10].as<std::string>()},
                {"fees",  t[11].as<std::string>()},
                {"total", t[12].as<std::string>()},
            };

            res.code = 200;
            res.body = out.dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "share_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });
}
