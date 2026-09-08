#include "routes/allocation_routes.h"
#include "auth/auth.h"
#include "auth/middleware.h"
#include "models/user.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr std::size_t MAX_DECIMALS        = 4;   // NUMERIC(_,4) everywhere
constexpr std::size_t MAX_MONEY_INT_DIGITS = 8;  // 99999999.9999 is the ceiling
constexpr std::size_t MAX_ALLOCATIONS     = 500; // one item, one split's members

// ── Small shared helpers (mirrors item_routes.cpp) ───────────────────────────

std::string trim(const std::string& s) {
    const char* ws = " \t\n\r\f\v";
    auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

// Path parameters are user input and are interpolated into `$n::uuid` casts.
// Postgres raises invalid_text_representation for a malformed uuid, which would
// surface as a 500, so ids are validated here first and a bad one is simply
// "not found" — the same answer an unknown id gets.
bool is_uuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

crow::response json_error(int code, const std::string& message) {
    crow::response res(code, json({{"error", message}}).dump());
    res.add_header("Content-Type", "application/json");
    return res;
}

// ── Decimal handling ─────────────────────────────────────────────────────────
//
// No ratio and no amount ever passes through a double in this module. The wire
// form is a string ("33.3333"); a JSON number is accepted for convenience but
// is immediately turned back into text, and the text is what Postgres receives
// as `$n::numeric`. Every sum, product and rounding happens in the database.

// Recovers the literal text of a JSON numeric value. A JSON float has already
// been through a double by the time nlohmann hands it over, so dump() (the
// shortest round-tripping form) is used and anything outside the 4-decimal
// domain is rejected below rather than silently rounded. Clients that care
// about exactness send a string, which is passed through untouched.
bool decimal_text(const json& v, const std::string& field,
                  std::string& out, std::string& err) {
    if (v.is_string()) {
        out = trim(v.get<std::string>());
        return true;
    }
    if (v.is_number_unsigned()) {
        out = std::to_string(v.get<std::uint64_t>());
        return true;
    }
    if (v.is_number_integer()) {
        out = std::to_string(v.get<std::int64_t>());
        return true;
    }
    if (v.is_number_float()) {
        out = v.dump();
        return true;
    }
    err = field + " must be a numeric string, like \"33.3333\"";
    return false;
}

// Validates a plain decimal and rewrites it in the canonical form Postgres will
// store (leading zeros dropped, fraction padded to 4 places), splitting out the
// integer and fractional digits so the caller can bound-check the value without
// arithmetic. Rejects negatives, exponent notation and anything that is not a
// plain decimal.
bool normalize_decimal(std::string& text, const std::string& field,
                       std::size_t max_int_digits,
                       std::string& int_part, std::string& frac_part,
                       std::string& err) {
    const std::string in = text;
    std::size_t i = 0;

    if (i < in.size() && (in[i] == '-' || in[i] == '+')) {
        if (in[i] == '-') {
            err = field + " must be greater than 0";
            return false;
        }
        ++i;
    }

    int_part.clear();
    frac_part.clear();
    while (i < in.size() && std::isdigit(static_cast<unsigned char>(in[i]))) {
        int_part += in[i++];
    }
    if (i < in.size() && in[i] == '.') {
        ++i;
        while (i < in.size() && std::isdigit(static_cast<unsigned char>(in[i]))) {
            frac_part += in[i++];
        }
    }

    // Anything left over is not a plain decimal: exponent notation, a currency
    // symbol, a thousands separator, "12.5.5", or an empty string.
    if (i != in.size() || (int_part.empty() && frac_part.empty())) {
        err = field + " must be a decimal number, like \"33.3333\"";
        return false;
    }
    if (frac_part.size() > MAX_DECIMALS) {
        err = field + " must have at most 4 decimal places";
        return false;
    }

    auto first_significant = int_part.find_first_not_of('0');
    int_part = (first_significant == std::string::npos)
                   ? "0"
                   : int_part.substr(first_significant);
    if (int_part.size() > max_int_digits) {
        err = field + " is out of range";
        return false;
    }

    frac_part.append(MAX_DECIMALS - frac_part.size(), '0');
    text = int_part + "." + frac_part;
    return true;
}

// "Is this normalized decimal zero?" — a digit test, not arithmetic. Zero and
// negative are both rejected: "allocate nothing to Bob" is expressed by leaving
// Bob out, and having two ways to say it invites them to disagree.
bool is_zero(const std::string& int_part, const std::string& frac_part) {
    return int_part == "0" &&
           frac_part.find_first_not_of('0') == std::string::npos;
}

// A ratio is a percent: > 0 and <= 100.0000. Comparing digit counts rather than
// converting to double keeps the boundary exact — "100.0001" must not pass.
bool check_ratio_bounds(const std::string& int_part, const std::string& frac_part,
                        std::string& err) {
    if (is_zero(int_part, frac_part)) {
        err = "ratio must be greater than 0";
        return false;
    }
    const bool over_100 =
        int_part.size() > 3 ||
        (int_part.size() == 3 &&
         (int_part != "100" || frac_part.find_first_not_of('0') != std::string::npos));
    if (over_100) {
        err = "ratio must be at most 100";
        return false;
    }
    return true;
}

bool check_amount_bounds(const std::string& int_part, const std::string& frac_part,
                         std::string& err) {
    if (is_zero(int_part, frac_part)) {
        err = "amount must be greater than 0";
        return false;
    }
    return true;
}

// ── Reading an AllocationSet back out ────────────────────────────────────────
//
// $1 = item id, $2 = bill id, $3 = owner id, in every statement in this module.
// The CTE is the authorization: the item is only reachable through a bill on a
// split this user owns, and only under the bill named in the URL. Zero rows is
// the answer for "no such item", "not your item" and "wrong bill" alike.
const char* TARGET_CTE =
    "WITH target AS ("
    "    SELECT i.id, b.split_id, (i.price * i.quantity) AS line_total"
    "      FROM bill_items i"
    "      JOIN bills b  ON b.id = i.bill_id"
    "      JOIN splits s ON s.id = b.split_id"
    "     WHERE i.id = $1::uuid"
    "       AND i.bill_id = $2::uuid"
    "       AND s.owner_id = $3::uuid)";

// The share is computed here, in NUMERIC, and nowhere else: ROUND(line_total *
// ratio / 100, 4) in ratio mode, the stored amount in amount mode.
const char* ALLOCATION_ROWS_SQL =
    " SELECT a.id::text          AS id,"
    "        a.bill_item_id::text AS bill_item_id,"
    "        a.member_id::text    AS member_id,"
    "        m.name               AS member_name,"
    "        a.allocation_mode,"
    "        a.ratio::text        AS ratio,"
    "        a.amount::text       AS amount,"
    "        (CASE WHEN a.allocation_mode = 'ratio'"
    "              THEN ROUND(t.line_total * a.ratio / 100, 4)"
    "              ELSE ROUND(a.amount, 4) END)::text AS share"
    "   FROM target t"
    "   JOIN item_allocations a ON a.bill_item_id = t.id"
    "   JOIN split_members m    ON m.id = a.member_id"
    "  ORDER BY m.name, a.member_id";

// allocated is SUM(share) over the same rounded shares the client is shown, so
// the list and the total can never disagree. unallocated is the remainder;
// GREATEST(...,0) keeps it non-negative even for a legacy row set that predates
// a price change, and no part of the remainder is redistributed to anyone.
const char* ALLOCATION_SUMMARY_SQL =
    ", alloc AS ("
    "    SELECT a.allocation_mode,"
    "           CASE WHEN a.allocation_mode = 'ratio'"
    "                THEN ROUND(t.line_total * a.ratio / 100, 4)"
    "                ELSE ROUND(a.amount, 4) END AS share"
    "      FROM target t"
    "      JOIN item_allocations a ON a.bill_item_id = t.id)"
    " SELECT t.id::text                    AS bill_item_id,"
    "        ROUND(t.line_total, 4)::text  AS line_total,"
    "        (SELECT MIN(allocation_mode) FROM alloc) AS mode,"
    "        ROUND(COALESCE((SELECT SUM(share) FROM alloc), 0), 4)::text AS allocated,"
    "        ROUND(GREATEST(t.line_total"
    "                       - COALESCE((SELECT SUM(share) FROM alloc), 0),"
    "                       0::numeric), 4)::text AS unallocated"
    "   FROM target t";

json field_or_null(const pqxx::row& r, const char* column) {
    return r[column].is_null() ? json(nullptr) : json(r[column].as<std::string>());
}

// Builds the AllocationSet for an item, or nullopt when the item is not the
// caller's (which every route reports as 404). Runs inside the caller's
// transaction so a PUT reads back exactly what it just wrote.
std::optional<json> read_allocation_set(pqxx::work& txn,
                                        const std::string& item_id,
                                        const std::string& bill_id,
                                        const std::string& owner_id) {
    auto summary = txn.exec(std::string(TARGET_CTE) + ALLOCATION_SUMMARY_SQL,
                            pqxx::params{item_id, bill_id, owner_id});
    if (summary.empty()) return std::nullopt;

    const auto& s = summary[0];
    json out;
    out["bill_item_id"] = s["bill_item_id"].as<std::string>();
    out["line_total"]   = s["line_total"].as<std::string>();
    out["mode"]         = field_or_null(s, "mode");
    out["allocated"]    = s["allocated"].as<std::string>();
    out["unallocated"]  = s["unallocated"].as<std::string>();

    auto rows = txn.exec(std::string(TARGET_CTE) + ALLOCATION_ROWS_SQL,
                         pqxx::params{item_id, bill_id, owner_id});
    json allocations = json::array();
    for (const auto& r : rows) {
        json a;
        a["id"]              = r["id"].as<std::string>();
        a["bill_item_id"]    = r["bill_item_id"].as<std::string>();
        a["member_id"]       = r["member_id"].as<std::string>();
        a["member_name"]     = r["member_name"].as<std::string>();
        a["allocation_mode"] = r["allocation_mode"].as<std::string>();
        a["ratio"]           = field_or_null(r, "ratio");
        a["amount"]          = field_or_null(r, "amount");
        a["share"]           = r["share"].as<std::string>();
        allocations.push_back(std::move(a));
    }
    out["allocations"] = std::move(allocations);
    return out;
}

// ── Request parsing ──────────────────────────────────────────────────────────

struct AllocInput {
    std::string member_id;
    std::string value;   // normalized to 4 decimals; a ratio or an amount
};

// Reads a member id out of an object field, enforcing uuid shape. A malformed
// uuid here is a 400, not a 404: the item was found, the body is wrong.
bool read_member_id(const json& obj, const char* field,
                    std::string& out, std::string& err) {
    if (!obj.contains(field) || obj[field].is_null()) {
        err = std::string(field) + " is required";
        return false;
    }
    if (!obj[field].is_string()) {
        err = std::string(field) + " must be a uuid string";
        return false;
    }
    out = trim(obj[field].get<std::string>());
    if (!is_uuid(out)) {
        err = std::string(field) + " must be a uuid";
        return false;
    }
    return true;
}

// Parses { mode, allocations: [{member_id, ratio|amount}] }.
//
// The mode is a property of the whole set: a row carrying its own
// allocation_mode must agree with it, and a row carrying the other mode's value
// field is the same mistake spelled differently. Both are a 400.
bool parse_put_body(const json& body, std::string& mode,
                    std::vector<AllocInput>& out, std::string& err) {
    if (!body.is_object()) {
        err = "Request body must be a JSON object";
        return false;
    }
    if (!body.contains("mode") || body["mode"].is_null() || !body["mode"].is_string()) {
        err = "mode is required and must be \"ratio\" or \"amount\"";
        return false;
    }
    mode = trim(body["mode"].get<std::string>());
    if (mode != "ratio" && mode != "amount") {
        err = "mode must be \"ratio\" or \"amount\"";
        return false;
    }
    if (!body.contains("allocations") || !body["allocations"].is_array()) {
        err = "allocations must be an array";
        return false;
    }
    const auto& arr = body["allocations"];
    if (arr.size() > MAX_ALLOCATIONS) {
        err = "allocations may contain at most 500 entries";
        return false;
    }

    const std::string value_field = (mode == "ratio") ? "ratio" : "amount";
    const std::string other_field = (mode == "ratio") ? "amount" : "ratio";

    std::set<std::string> seen;
    for (const auto& entry : arr) {
        if (!entry.is_object()) {
            err = "each allocation must be an object";
            return false;
        }
        AllocInput in;
        if (!read_member_id(entry, "member_id", in.member_id, err)) return false;
        if (!seen.insert(in.member_id).second) {
            err = "member_id " + in.member_id + " appears more than once";
            return false;
        }

        if (entry.contains("allocation_mode") && !entry["allocation_mode"].is_null()) {
            if (!entry["allocation_mode"].is_string() ||
                entry["allocation_mode"].get<std::string>() != mode) {
                err = "every allocation must use the same mode (\"" + mode + "\")";
                return false;
            }
        }
        if (entry.contains(other_field) && !entry[other_field].is_null()) {
            err = "every allocation must use the same mode (\"" + mode +
                  "\"); " + other_field + " is not allowed in " + mode + " mode";
            return false;
        }
        if (!entry.contains(value_field) || entry[value_field].is_null()) {
            err = value_field + " is required for every allocation in " + mode + " mode";
            return false;
        }
        if (entry.contains("share")) {
            err = "share is derived by the server and cannot be set";
            return false;
        }

        if (!decimal_text(entry[value_field], value_field, in.value, err)) return false;

        std::string int_part, frac_part;
        const std::size_t max_int_digits = (mode == "ratio") ? 3 : MAX_MONEY_INT_DIGITS;
        if (!normalize_decimal(in.value, value_field, max_int_digits,
                               int_part, frac_part, err)) {
            return false;
        }
        const bool ok = (mode == "ratio")
                            ? check_ratio_bounds(int_part, frac_part, err)
                            : check_amount_bounds(int_part, frac_part, err);
        if (!ok) return false;

        out.push_back(std::move(in));
    }
    return true;
}

// Parses { member_ids: [uuid] } for the even-split action.
bool parse_member_ids(const json& body, std::vector<std::string>& out,
                      std::string& err) {
    if (!body.is_object()) {
        err = "Request body must be a JSON object";
        return false;
    }
    if (!body.contains("member_ids") || !body["member_ids"].is_array()) {
        err = "member_ids must be a non-empty array of member uuids";
        return false;
    }
    const auto& arr = body["member_ids"];
    if (arr.empty()) {
        err = "member_ids must contain at least one member";
        return false;
    }
    if (arr.size() > MAX_ALLOCATIONS) {
        err = "member_ids may contain at most 500 entries";
        return false;
    }

    std::set<std::string> seen;
    for (const auto& v : arr) {
        if (!v.is_string()) {
            err = "member_ids must contain uuid strings";
            return false;
        }
        std::string id = trim(v.get<std::string>());
        if (!is_uuid(id)) {
            err = "member_ids must contain uuid strings";
            return false;
        }
        if (!seen.insert(id).second) {
            err = "member_id " + id + " appears more than once";
            return false;
        }
        out.push_back(std::move(id));
    }
    return true;
}

// Deletes every allocation on an item, re-proving ownership in the statement so
// the write is safe on its own terms and not only because an earlier SELECT in
// this transaction said so.
void delete_allocations(pqxx::work& txn, const std::string& item_id,
                        const std::string& bill_id, const std::string& owner_id) {
    txn.exec(
        "DELETE FROM item_allocations"
        "  USING bill_items i, bills b, splits s"
        " WHERE item_allocations.bill_item_id = $1::uuid"
        "   AND i.id = item_allocations.bill_item_id"
        "   AND i.bill_id = $2::uuid"
        "   AND b.id = i.bill_id"
        "   AND s.id = b.split_id"
        "   AND s.owner_id = $3::uuid",
        pqxx::params{item_id, bill_id, owner_id});
}

}  // namespace

void register_allocation_routes(BsApp& app, DbPool& pool) {

    // ── GET /api/bills/<bid>/items/<iid>/allocations ──────────────────────────
    // 200 AllocationSet. An item with no allocations is a valid 200 with an
    // empty list, mode null and unallocated == line_total.
    CROW_ROUTE(app, "/api/bills/<string>/items/<string>/allocations")
        .methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req,
             const std::string& bill_id,
             const std::string& item_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return unauthorized();

        if (!is_uuid(bill_id) || !is_uuid(item_id)) {
            return json_error(404, "Item not found");
        }

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto set = read_allocation_set(txn, item_id, bill_id, user->id);
            if (!set) {
                txn.abort();
                return json_error(404, "Item not found");
            }
            txn.commit();

            res.code = 200;
            res.body = set->dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "allocation_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── PUT /api/bills/<bid>/items/<iid>/allocations ──────────────────────────
    // Body: { mode, allocations: [{ member_id, ratio | amount }] } → 200 set.
    //
    // A full replace in one transaction: the old rows are deleted and the new
    // set inserted together, so "remove Bob" is expressible and a failure can
    // never leave half of each set behind. An empty allocations array clears
    // the item.
    CROW_ROUTE(app, "/api/bills/<string>/items/<string>/allocations")
        .methods(crow::HTTPMethod::PUT)
    ([&pool](const crow::request& req,
             const std::string& bill_id,
             const std::string& item_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return unauthorized();

        if (!is_uuid(bill_id) || !is_uuid(item_id)) {
            return json_error(404, "Item not found");
        }

        try {
            auto body = json::parse(req.body);

            std::string mode;
            std::vector<AllocInput> allocs;
            std::string err;
            if (!parse_put_body(body, mode, allocs, err)) return json_error(400, err);

            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // Values only ever travel as parameters; the placeholder list is
            // the only thing built by concatenation.
            pqxx::params params{item_id, bill_id, user->id};
            std::string values;
            int n = 4;
            for (std::size_t k = 0; k < allocs.size(); ++k) {
                if (k > 0) values += ", ";
                values += "($" + std::to_string(n) + "::uuid, $" +
                          std::to_string(n + 1) + "::numeric)";
                n += 2;
                params.append(allocs[k].member_id);
                params.append(allocs[k].value);
            }
            const std::string mode_param = "$" + std::to_string(n++);
            params.append(mode);

            if (!allocs.empty()) {
                // One statement answers all three questions the write depends
                // on: does the caller own this item (zero rows if not), do all
                // the member ids belong to that item's split, and does the set
                // over-allocate. The sums are NUMERIC sums computed here, never
                // accumulated in C++.
                const std::string check_sql =
                    std::string(TARGET_CTE) +
                    ", input(member_id, val) AS (VALUES " + values + ")"
                    " SELECT (SELECT COUNT(*) FROM input i"
                    "           WHERE NOT EXISTS (SELECT 1 FROM split_members m"
                    "                              WHERE m.id = i.member_id"
                    "                                AND m.split_id = t.split_id))::int"
                    "            AS unknown_members,"
                    "        ((SELECT COALESCE(SUM(val), 0) FROM input)"
                    "           > CASE WHEN " + mode_param + "::text = 'ratio'"
                    "                  THEN 100::numeric ELSE t.line_total END)"
                    "            AS over_allocated"
                    "   FROM target t";

                auto check = txn.exec(check_sql, params);
                if (check.empty()) {
                    txn.abort();
                    return json_error(404, "Item not found");
                }
                if (check[0]["unknown_members"].as<int>() > 0) {
                    txn.abort();
                    return json_error(400,
                        "Every member_id must belong to this bill's split");
                }
                if (check[0]["over_allocated"].as<bool>()) {
                    txn.abort();
                    return json_error(400,
                        mode == "ratio"
                            ? "Allocated ratios must not add up to more than 100%"
                            : "Allocated amounts must not add up to more than the "
                              "line total");
                }
            } else {
                // Nothing to insert, but the item must still be proven to be
                // the caller's before its rows are cleared.
                auto found = txn.exec(std::string(TARGET_CTE) +
                                      " SELECT id::text AS id FROM target",
                                      pqxx::params{item_id, bill_id, user->id});
                if (found.empty()) {
                    txn.abort();
                    return json_error(404, "Item not found");
                }
            }

            delete_allocations(txn, item_id, bill_id, user->id);

            if (!allocs.empty()) {
                // chk_one_mode requires exactly one of ratio/amount to be set;
                // the CASE arms without ELSE leave the other NULL.
                const std::string insert_sql =
                    std::string(TARGET_CTE) +
                    ", input(member_id, val) AS (VALUES " + values + ")"
                    " INSERT INTO item_allocations"
                    "        (bill_item_id, member_id, allocation_mode, ratio, amount)"
                    " SELECT t.id, i.member_id, " + mode_param + "::text,"
                    "        CASE WHEN " + mode_param + "::text = 'ratio'  THEN i.val END,"
                    "        CASE WHEN " + mode_param + "::text = 'amount' THEN i.val END"
                    "   FROM target t CROSS JOIN input i";
                txn.exec(insert_sql, params);
            }

            auto set = read_allocation_set(txn, item_id, bill_id, user->id);
            if (!set) {
                txn.abort();
                return json_error(404, "Item not found");
            }
            txn.commit();

            res.code = 200;
            res.body = set->dump();
        } catch (const json::exception& e) {
            CROW_LOG_WARNING << "allocation_routes: bad JSON body: " << e.what();
            return json_error(400, "Invalid JSON body");
        } catch (const pqxx::unique_violation& e) {
            CROW_LOG_WARNING << "allocation_routes: unique violation: " << e.what();
            return json_error(409, "These allocations were modified concurrently; "
                                   "please retry");
        } catch (const pqxx::foreign_key_violation& e) {
            CROW_LOG_WARNING << "allocation_routes: fk violation: " << e.what();
            return json_error(409, "The item or one of its members was removed while "
                                   "saving; please retry");
        } catch (const pqxx::data_exception& e) {
            CROW_LOG_WARNING << "allocation_routes: numeric out of range: " << e.what();
            return json_error(400, "Amount out of range");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "allocation_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── POST /api/bills/<bid>/items/<iid>/even-split ──────────────────────────
    // Body: { member_ids: [uuid] } → 200 AllocationSet in `amount` mode.
    //
    // Each share is FLOOR(line_total * 100 / n) / 100 — floored to cents, not
    // rounded, so the set can never exceed the line — and the leftover cents
    // stay in `unallocated` rather than being pushed onto one member.
    CROW_ROUTE(app, "/api/bills/<string>/items/<string>/even-split")
        .methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req,
             const std::string& bill_id,
             const std::string& item_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return unauthorized();

        if (!is_uuid(bill_id) || !is_uuid(item_id)) {
            return json_error(404, "Item not found");
        }

        try {
            auto body = json::parse(req.body);

            std::vector<std::string> member_ids;
            std::string err;
            if (!parse_member_ids(body, member_ids, err)) return json_error(400, err);

            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            pqxx::params params{item_id, bill_id, user->id};
            std::string values;
            int n = 4;
            for (std::size_t k = 0; k < member_ids.size(); ++k) {
                if (k > 0) values += ", ";
                values += "($" + std::to_string(n++) + "::uuid)";
                params.append(member_ids[k]);
            }
            const std::string count_param = "$" + std::to_string(n++);
            params.append(static_cast<int>(member_ids.size()));

            const std::string share_expr =
                "FLOOR(t.line_total * 100 / " + count_param + "::int) / 100";

            const std::string check_sql =
                std::string(TARGET_CTE) +
                ", input(member_id) AS (VALUES " + values + ")"
                " SELECT (SELECT COUNT(*) FROM input i"
                "           WHERE NOT EXISTS (SELECT 1 FROM split_members m"
                "                              WHERE m.id = i.member_id"
                "                                AND m.split_id = t.split_id))::int"
                "            AS unknown_members,"
                "        (" + share_expr + " <= 0) AS share_too_small"
                "   FROM target t";

            auto check = txn.exec(check_sql, params);
            if (check.empty()) {
                txn.abort();
                return json_error(404, "Item not found");
            }
            if (check[0]["unknown_members"].as<int>() > 0) {
                txn.abort();
                return json_error(400, "Every member_id must belong to this bill's split");
            }
            // A share that floors to zero cents cannot be stored: a zero
            // allocation is rejected everywhere else in this module, and
            // silently writing one would create a row that says nothing.
            if (check[0]["share_too_small"].as<bool>()) {
                txn.abort();
                return json_error(400,
                    "This line total is too small to split evenly across that many "
                    "members");
            }

            delete_allocations(txn, item_id, bill_id, user->id);

            const std::string insert_sql =
                std::string(TARGET_CTE) +
                ", input(member_id) AS (VALUES " + values + ")"
                " INSERT INTO item_allocations"
                "        (bill_item_id, member_id, allocation_mode, amount)"
                " SELECT t.id, i.member_id, 'amount', " + share_expr +
                "   FROM target t CROSS JOIN input i";
            txn.exec(insert_sql, params);

            auto set = read_allocation_set(txn, item_id, bill_id, user->id);
            if (!set) {
                txn.abort();
                return json_error(404, "Item not found");
            }
            txn.commit();

            res.code = 200;
            res.body = set->dump();
        } catch (const json::exception& e) {
            CROW_LOG_WARNING << "allocation_routes: bad JSON body: " << e.what();
            return json_error(400, "Invalid JSON body");
        } catch (const pqxx::unique_violation& e) {
            CROW_LOG_WARNING << "allocation_routes: unique violation: " << e.what();
            return json_error(409, "These allocations were modified concurrently; "
                                   "please retry");
        } catch (const pqxx::foreign_key_violation& e) {
            CROW_LOG_WARNING << "allocation_routes: fk violation: " << e.what();
            return json_error(409, "The item or one of its members was removed while "
                                   "saving; please retry");
        } catch (const pqxx::data_exception& e) {
            CROW_LOG_WARNING << "allocation_routes: numeric out of range: " << e.what();
            return json_error(400, "Amount out of range");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "allocation_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });
}
