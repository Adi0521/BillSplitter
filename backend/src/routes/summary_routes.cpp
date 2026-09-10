#include "routes/summary_routes.h"
#include "auth/middleware.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <cctype>
#include <cstddef>
#include <cstdint>
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

// The :id path parameter is raw user input. Casting a non-UUID string to uuid
// makes Postgres raise, which would surface as a 500, so the shape is checked
// here and anything malformed is treated as "not found".
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
// Three queries run in one transaction and therefore against one snapshot: the
// split header (which is also the ownership check), one row per currency, and
// one row per (currency, member). The last two share the CTE prefix below, so
// the per-currency totals and the member rows underneath them cannot be
// aggregated from different sets of bills.
//
//   $1 = split id, $2 = owner (session) user id
//
// sp
//   The ownership join, and the only place ownership is established. Zero rows
//   means "no such split for this user", so another user's split and a
//   nonexistent one are indistinguishable (404). Archived splits still resolve:
//   GET /api/splits/:id returns them, and a summary of a finished trip is
//   exactly when someone wants one.
//
// bill
//   Every bill of the split with its derived subtotal. There is no
//   bills.subtotal column (dropped in migration 003), so it is SUM(price *
//   quantity) over the bill's items, with the 0.0000 fallback written to four
//   decimals so an empty bill reads "0.0000" and not "0".
//
// shares
//   One row per allocation, worth what that allocation is worth. Rounded per
//   allocation row, exactly as GET .../shares reports it, and only then summed:
//   summing first and rounding once would disagree with the per-bill numbers
//   the user was already shown.
//
// member_bill
//   The full (member x bill) grid, so a member with nothing on a bill is a
//   0.0000 row rather than a missing one, and a member with nothing anywhere
//   still reaches the output with zeros.
//
// member_owes
//   The Phase 4 per-bill arithmetic, added up per currency. The denominator is
//   the bill's FULL subtotal, never the allocated part: that is what makes an
//   unassigned item carry its own tax instead of pushing it onto the people who
//   are present. NULLIF(subtotal, 0) is the division-by-zero guard -- Postgres
//   is never asked to divide by zero -- and the COALESCE turns the resulting
//   NULL into 0.0000, so a bill whose subtotal is zero contributes nothing to
//   anybody rather than erroring.
//
// bill_un
//   The same arithmetic applied to the part of the subtotal nobody is on the
//   hook for. Here the COALESCE fallback is the FULL tax/tip/fees rather than
//   zero: when the subtotal is zero nothing is allocated to anyone, so the
//   unallocated fraction is 1 and the whole of tax, tip and fees belongs here
//   -- otherwise a bill of nothing but tax would report charges sitting in no
//   bucket at all. GREATEST(..., 0.0000) keeps the remainder from going
//   negative when per-row rounding pushes the allocated sum a hundredth of a
//   cent past the subtotal.
//
// fronted / paid / received
//   The other three terms of the balance, each matched on currency so nothing
//   is ever combined across currencies. Payments with a NULL member cannot
//   affect anybody's balance and are excluded rather than silently attributed.
//
// cur
//   The currency universe: one entry per currency actually used by the split's
//   BILLS. A split with no bills yields no rows here, which is what makes
//   by_currency an empty array instead of a fabricated zero row for the split's
//   own currency.
const char* const SUMMARY_CTE =
    "WITH sp AS ("
    "    SELECT s.id, s.name, s.currency "
    "      FROM splits s "
    "     WHERE s.id = $1::uuid AND s.owner_id = $2::uuid"
    "), "
    "bill AS ("
    "    SELECT b.id, b.currency, b.tax, b.tip, b.fees, b.payer_member_id, "
    "           COALESCE((SELECT SUM(i.price * i.quantity) "
    "                       FROM bill_items i WHERE i.bill_id = b.id), 0.0000) "
    "               AS subtotal "
    "      FROM bills b JOIN sp ON sp.id = b.split_id"
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
    "member_bill AS ("
    "    SELECT m.id AS member_id, m.name, m.joined_at, "
    "           bl.id AS bill_id, bl.currency, "
    "           COALESCE(SUM(sh.share), 0.0000) AS items "
    "      FROM sp "
    "      JOIN split_members m ON m.split_id = sp.id "
    "      CROSS JOIN bill bl "
    "      LEFT JOIN shares sh ON sh.member_id = m.id AND sh.bill_id = bl.id "
    "     GROUP BY m.id, m.name, m.joined_at, bl.id, bl.currency"
    "), "
    "member_owes AS ("
    "    SELECT mb.member_id, mb.currency, "
    "           SUM(mb.items + p.tax + p.tip + p.fees) AS owes "
    "      FROM member_bill mb "
    "      JOIN bill bl ON bl.id = mb.bill_id "
    "      CROSS JOIN LATERAL ("
    "          SELECT COALESCE(ROUND(mb.items / NULLIF(bl.subtotal, 0) "
    "                                * bl.tax,  4), 0.0000) AS tax, "
    "                 COALESCE(ROUND(mb.items / NULLIF(bl.subtotal, 0) "
    "                                * bl.tip,  4), 0.0000) AS tip, "
    "                 COALESCE(ROUND(mb.items / NULLIF(bl.subtotal, 0) "
    "                                * bl.fees, 4), 0.0000) AS fees "
    "      ) p "
    "     GROUP BY mb.member_id, mb.currency"
    "), "
    "bill_alloc AS ("
    "    SELECT bill_id, COALESCE(SUM(items), 0.0000) AS allocated "
    "      FROM member_bill GROUP BY bill_id"
    "), "
    "bill_un AS ("
    "    SELECT bl.currency, un.items, un.tax, un.tip, un.fees "
    "      FROM bill bl "
    "      LEFT JOIN bill_alloc ba ON ba.bill_id = bl.id "
    "      CROSS JOIN LATERAL ("
    "          SELECT u.items, "
    "                 COALESCE(ROUND(u.items / NULLIF(bl.subtotal, 0) "
    "                                * bl.tax,  4), bl.tax)  AS tax, "
    "                 COALESCE(ROUND(u.items / NULLIF(bl.subtotal, 0) "
    "                                * bl.tip,  4), bl.tip)  AS tip, "
    "                 COALESCE(ROUND(u.items / NULLIF(bl.subtotal, 0) "
    "                                * bl.fees, 4), bl.fees) AS fees "
    "            FROM (SELECT GREATEST(bl.subtotal "
    "                                  - COALESCE(ba.allocated, 0.0000), "
    "                                  0.0000) AS items) u"
    "      ) un"
    "), "
    "fronted AS ("
    "    SELECT bl.payer_member_id AS member_id, bl.currency, "
    "           SUM(bl.subtotal + bl.tax + bl.tip + bl.fees) AS fronted "
    "      FROM bill bl "
    "     WHERE bl.payer_member_id IS NOT NULL "
    "     GROUP BY bl.payer_member_id, bl.currency"
    "), "
    "paid AS ("
    "    SELECT p.from_member AS member_id, p.currency, "
    "           SUM(p.amount) AS amount "
    "      FROM payments p JOIN sp ON sp.id = p.split_id "
    "     WHERE p.from_member IS NOT NULL "
    "     GROUP BY p.from_member, p.currency"
    "), "
    "received AS ("
    "    SELECT p.to_member AS member_id, p.currency, "
    "           SUM(p.amount) AS amount "
    "      FROM payments p JOIN sp ON sp.id = p.split_id "
    "     WHERE p.to_member IS NOT NULL "
    "     GROUP BY p.to_member, p.currency"
    "), "
    // The currency universe is bills UNION payments, not bills alone. A
    // payment defaults to the split's currency, so in a USD split whose bills
    // are all in EUR a recorded settlement would otherwise belong to no block
    // and vanish from every balance — a payment the user entered, silently
    // ignored. Such a currency yields bill_count 0 and total 0.0000, which is
    // the truth: no bills, but money did move.
    "cur AS ("
    "    SELECT u.currency, "
    "           COUNT(bl.currency) AS bill_count, "
    "           COALESCE(SUM(bl.subtotal + bl.tax + bl.tip + bl.fees), 0.0000) AS total "
    "      FROM (SELECT bl2.currency FROM bill bl2 "
    "            UNION "
    "            SELECT p.currency FROM payments p JOIN sp ON sp.id = p.split_id) u "
    " LEFT JOIN bill bl ON bl.currency = u.currency "
    "     GROUP BY u.currency"
    ")";

// The split header. Also the ownership check: empty means 404.
const char* const SPLIT_SQL =
    "SELECT s.id, s.name, s.currency "
    "  FROM splits s "
    " WHERE s.id = $1::uuid AND s.owner_id = $2::uuid";

// One row per currency: how many bills, what they came to, and the part of
// them nobody is on the hook for.
//
// Every money column is wrapped in ROUND(x, 4) so it renders with exactly four
// decimals whatever scale the aggregate happened to produce: "8.0000", never
// "8". Currencies come back alphabetically, which is arbitrary but stable.
std::string currency_sql() {
    return std::string(SUMMARY_CTE) +
        " SELECT c.currency, c.bill_count, ROUND(c.total, 4) AS total, "
        "        ROUND(COALESCE(u.items, 0), 4) AS un_items, "
        "        ROUND(COALESCE(u.tax,   0), 4) AS un_tax, "
        "        ROUND(COALESCE(u.tip,   0), 4) AS un_tip, "
        "        ROUND(COALESCE(u.fees,  0), 4) AS un_fees, "
        "        ROUND(COALESCE(u.items, 0) + COALESCE(u.tax, 0) "
        "              + COALESCE(u.tip, 0) + COALESCE(u.fees, 0), 4) AS un_total "
        "   FROM cur c "
        // LEFT, not inner: a currency that exists only because of a payment has
        // no bills and therefore no unallocated row. An inner join here dropped
        // the whole block, which is what made a recorded payment invisible.
        "   LEFT JOIN (SELECT currency, SUM(items) AS items, SUM(tax) AS tax, "
        "                     SUM(tip) AS tip, SUM(fees) AS fees "
        "                FROM bill_un GROUP BY currency) u "
        "     ON u.currency = c.currency "
        "  ORDER BY c.currency ASC";
}

// One row per (currency, member). The cross join against cur is what puts a
// member with no activity in the output with zeros instead of dropping them,
// and what keeps a member's USD row and EUR row separate.
//
// balance = fronted + payments_made - owes - payments_received. It is reported,
// not reconciled: rounding residue and the unallocated remainder mean the
// column does not sum to zero, and making it do so would hide exactly what this
// design exists to surface.
//
// Ordered like GET /api/splits/:id/members so the lists line up in the UI, with
// the member id as a tiebreaker for members sharing a joined_at.
std::string member_sql() {
    return std::string(SUMMARY_CTE) +
        " SELECT c.currency, m.id, m.name, "
        "        ROUND(COALESCE(o.owes, 0), 4)    AS owes, "
        "        ROUND(COALESCE(f.fronted, 0), 4) AS fronted, "
        "        ROUND(COALESCE(pd.amount, 0), 4) AS payments_made, "
        "        ROUND(COALESCE(rc.amount, 0), 4) AS payments_received, "
        "        ROUND(COALESCE(f.fronted, 0) + COALESCE(pd.amount, 0) "
        "              - COALESCE(o.owes, 0) - COALESCE(rc.amount, 0), 4) "
        "            AS balance "
        "   FROM cur c "
        "   CROSS JOIN sp "
        "   JOIN split_members m ON m.split_id = sp.id "
        "   LEFT JOIN member_owes o ON o.member_id = m.id "
        "                          AND o.currency  = c.currency "
        "   LEFT JOIN fronted f     ON f.member_id = m.id "
        "                          AND f.currency  = c.currency "
        "   LEFT JOIN paid pd       ON pd.member_id = m.id "
        "                          AND pd.currency  = c.currency "
        "   LEFT JOIN received rc   ON rc.member_id = m.id "
        "                          AND rc.currency  = c.currency "
        "  ORDER BY c.currency ASC, m.joined_at ASC, m.id ASC";
}

// ── Scaled integer money ─────────────────────────────────────────────────────
//
// Settlements are the one computed amount Postgres does not derive. They only
// *pair* balances it already computed -- nothing is re-derived, no proportion is
// taken -- so exactness is achievable, and a suggested transfer that is a
// hundredth of a cent off from the balance it clears is a bug. The pairing is
// therefore done on int64 counts of 1/10000ths. No double appears anywhere in
// this file.

using Scaled = std::int64_t;

constexpr Scaled SCALE = 10000;

// Refuses anything that would overflow the int64 when scaled. NUMERIC(12,4)
// caps a single amount near 1e8, so this bound is never reached by real data;
// it is here so that a corrupt or absurd value fails loudly rather than
// wrapping around into a plausible-looking settlement.
constexpr Scaled MAX_UNITS = static_cast<Scaled>(9e17);

// Parses "38.7600" / "-12.5" / "7" into 1/10000ths. Values arrive from
// ROUND(x, 4) so they always carry exactly four decimals, but fewer are
// accepted (and padded) rather than trusted.
Scaled parse_scaled(const std::string& s) {
    std::size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = (s[i] == '-');
        ++i;
    }

    Scaled units = 0;
    std::size_t digits = 0;
    for (; i < s.size() && s[i] != '.'; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            throw std::runtime_error("malformed numeric from database: " + s);
        }
        if (units > MAX_UNITS / 10) {
            throw std::runtime_error("numeric out of range: " + s);
        }
        units = units * 10 + (s[i] - '0');
        ++digits;
    }
    if (digits == 0) throw std::runtime_error("malformed numeric: " + s);

    Scaled frac = 0;
    std::size_t places = 0;
    if (i < s.size() && s[i] == '.') {
        for (++i; i < s.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
                throw std::runtime_error("malformed numeric: " + s);
            }
            if (places == 4) {
                // A fifth decimal would have to be dropped or rounded, and
                // either is a silent change to what somebody owes.
                throw std::runtime_error("numeric has more than 4 decimals: " + s);
            }
            frac = frac * 10 + (s[i] - '0');
            ++places;
        }
    }
    while (places < 4) { frac *= 10; ++places; }

    if (units > MAX_UNITS / SCALE) {
        throw std::runtime_error("numeric out of range: " + s);
    }
    const Scaled total = units * SCALE + frac;
    return negative ? -total : total;
}

// The inverse: 387600 -> "38.7600", always four decimals, matching the way
// Postgres renders every other money field in this response.
std::string format_scaled(Scaled v) {
    const bool negative = v < 0;
    // Built on the magnitude, but computed without negating v, so the most
    // negative int64 cannot overflow here.
    const std::uint64_t magnitude =
        negative ? (~static_cast<std::uint64_t>(v) + 1u)
                 : static_cast<std::uint64_t>(v);
    const std::uint64_t whole = magnitude / SCALE;
    const std::uint64_t frac  = magnitude % SCALE;

    std::string frac_str = std::to_string(frac);
    frac_str.insert(0, 4 - frac_str.size(), '0');
    return (negative ? "-" : "") + std::to_string(whole) + "." + frac_str;
}

// ── Settlements ──────────────────────────────────────────────────────────────

struct MemberBalance {
    std::string id;
    std::string name;
    Scaled      balance = 0;
};

// Repeatedly matches the largest debtor against the largest creditor, in exact
// integer arithmetic. This is *a* valid settlement, not provably the minimal
// one; the contract says so and the UI is expected to say so too.
//
// The largest of each side is re-selected every round rather than walked with
// two sorted pointers, because after a partial match the residual is not
// necessarily still the largest -- and "largest against largest" is the rule
// the contract states.
//
// Balances need not sum to zero (rounding residue, and the unallocated
// remainder that nobody is on the hook for), so the loop stops as soon as one
// side runs out. Whatever is left over is left visible in the member rows
// instead of being absorbed into an invented transfer.
json settlements_for(const std::vector<MemberBalance>& members) {
    std::vector<Scaled> remaining;
    remaining.reserve(members.size());
    for (const auto& m : members) remaining.push_back(m.balance);

    json out = json::array();

    // Each round zeroes at least one side, so there can be no more rounds than
    // there are members.
    for (std::size_t round = 0; round < members.size(); ++round) {
        std::size_t debtor = 0, creditor = 0;
        bool have_debtor = false, have_creditor = false;

        for (std::size_t i = 0; i < remaining.size(); ++i) {
            if (remaining[i] < 0 &&
                (!have_debtor || remaining[i] < remaining[debtor])) {
                debtor = i;
                have_debtor = true;
            }
            if (remaining[i] > 0 &&
                (!have_creditor || remaining[i] > remaining[creditor])) {
                creditor = i;
                have_creditor = true;
            }
        }
        if (!have_debtor || !have_creditor) break;

        const Scaled owed   = -remaining[debtor];
        const Scaled due    = remaining[creditor];
        const Scaled amount = owed < due ? owed : due;
        if (amount <= 0) break;

        out.push_back({
            {"from_member", members[debtor].id},
            {"from_name",   members[debtor].name},
            {"to_member",   members[creditor].id},
            {"to_name",     members[creditor].name},
            {"amount",      format_scaled(amount)},
        });

        remaining[debtor]   += amount;
        remaining[creditor] -= amount;
    }
    return out;
}

}  // namespace

void register_summary_routes(BsApp& app, DbPool& pool) {

    // ── GET /api/splits/<id>/summary ─────────────────────────────────────────
    // Every balance in the split, grouped by the currency of the bills that
    // produced it, plus a suggested set of transfers that clears each group.
    CROW_ROUTE(app, "/api/splits/<string>/summary")
        .methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req, const std::string& id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id)) return json_error(404, "Split not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto split = txn.exec(SPLIT_SQL, pqxx::params{id, user->id});
            if (split.empty()) {
                txn.commit();
                return json_error(404, "Split not found");
            }

            // Safe without repeating the ownership check: both queries carry
            // the same sp CTE, and all three run in one transaction, so they
            // see one snapshot of one user's split.
            auto currencies = txn.exec(currency_sql(), pqxx::params{id, user->id});
            auto members    = txn.exec(member_sql(),   pqxx::params{id, user->id});
            txn.commit();

            // The member rows arrive ordered by currency, so they are walked
            // once alongside the currency rows rather than re-scanned per
            // group. pqxx's own index type is signed, so it is used here rather
            // than size_t, which would compare across signs.
            pqxx::result::size_type next_member = 0;

            json groups = json::array();
            for (const auto& c : currencies) {
                const std::string currency = c[0].as<std::string>();

                json group;
                group["currency"]    = currency;
                group["bill_count"]  = c[1].as<long long>();
                group["total"]       = c[2].as<std::string>();
                group["unallocated"] = {
                    {"items", c[3].as<std::string>()},
                    {"tax",   c[4].as<std::string>()},
                    {"tip",   c[5].as<std::string>()},
                    {"fees",  c[6].as<std::string>()},
                    {"total", c[7].as<std::string>()},
                };

                std::vector<MemberBalance> balances;
                json member_array = json::array();
                while (next_member < members.size() &&
                       members[next_member][0].as<std::string>() == currency) {
                    const pqxx::row& m = members[next_member];
                    ++next_member;

                    json row;
                    row["member_id"]         = m[1].as<std::string>();
                    row["name"]              = m[2].as<std::string>();
                    row["owes"]              = m[3].as<std::string>();
                    row["fronted"]           = m[4].as<std::string>();
                    row["payments_made"]     = m[5].as<std::string>();
                    row["payments_received"] = m[6].as<std::string>();
                    row["balance"]           = m[7].as<std::string>();
                    member_array.push_back(row);

                    balances.push_back({m[1].as<std::string>(),
                                        m[2].as<std::string>(),
                                        parse_scaled(m[7].as<std::string>())});
                }
                group["members"]     = member_array;
                group["settlements"] = settlements_for(balances);

                groups.push_back(group);
            }

            json out;
            out["split_id"]       = split[0][0].as<std::string>();
            out["name"]           = split[0][1].as<std::string>();
            out["base_currency"]  = split[0][2].as<std::string>();
            out["mixed_currency"] = groups.size() > 1;
            // Empty when the split has no bills: no currency has been used
            // yet, and inventing a zero row for the split's own currency would
            // claim a set of balances that does not exist.
            out["by_currency"]    = groups;

            res.code = 200;
            res.body = out.dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "summary_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });
}
