#include "routes/export_routes.h"
#include "auth/middleware.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <cctype>
#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// ── Response helpers ─────────────────────────────────────────────────────────

// Failures carry the same {"error": "..."} envelope as every other endpoint,
// even though the success path is CSV. A client that asked for a download and
// got a 404 should still be able to parse the reason.
crow::response json_error(int code, const std::string& message) {
    crow::response res(code, json({{"error", message}}).dump());
    res.add_header("Content-Type", "application/json");
    return res;
}

// Path parameters are raw user input. Postgres raises an error (which would
// otherwise surface as a 500) when a non-UUID string is cast to uuid, so the
// shape is checked here and anything malformed is treated as "not found".
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

// ── CSV writing ──────────────────────────────────────────────────────────────

// Formula injection is the reason this function exists.
//
// Excel, LibreOffice and Google Sheets evaluate any cell whose text begins with
// '=', '+', '-', '@', a tab or a carriage return as a formula the moment the
// file is opened. Item names come from receipts and from whatever a user typed,
// so `=cmd|' /C calc'!A0` is reachable from the database, and the entire point
// of this endpoint is that somebody opens the result in a spreadsheet.
//
// A leading single quote is the spreadsheet convention for "this cell is text",
// and it is applied to EVERY field rather than only to the ones expected to
// carry user input. Deciding per-column which fields are trustworthy is exactly
// the kind of judgement that goes stale when a column is added later; the money
// and date columns never start with one of these characters anyway, so the
// blanket rule costs nothing.
//
// The prefix goes on before RFC 4180 quoting, so it lands inside the quotes and
// is part of the cell's value rather than part of the file's framing.
std::string csv_field(const std::string& raw) {
    std::string value = raw;

    if (!value.empty()) {
        const char lead = value[0];
        if (lead == '=' || lead == '+' || lead == '-' || lead == '@' ||
            lead == '\t' || lead == '\r') {
            value.insert(value.begin(), '\'');
        }
    }

    // RFC 4180: a field holding a comma, a double quote or a line break is
    // wrapped in double quotes, and each embedded quote is doubled.
    const bool needs_quotes =
        value.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs_quotes) return value;

    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (const char c : value) {
        if (c == '"') quoted.push_back('"');
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
}

void append_row(std::string& out, const std::vector<std::string>& fields) {
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) out.push_back(',');
        out += csv_field(fields[i]);
    }
    out += "\r\n";   // RFC 4180 record separator
}

// ── Content-Disposition ──────────────────────────────────────────────────────

// Header injection is the second way this feature becomes a security bug.
//
// The split name is user input and it is interpolated into a response header.
// A split called `x"` + CRLF + `Set-Cookie: session=...` would, without this,
// terminate the Content-Disposition header and start one of the attacker's own
// — the response splitting bug in its classic form.
//
// So the filename is built from an allow-list rather than by removing the
// characters currently known to be dangerous: letters, digits, space, '-' and
// '_' survive, everything else (CR, LF, quotes, ';', backslashes, control
// characters and every non-ASCII byte) becomes '_'. An allow-list cannot be
// outflanked by an encoding trick the way a deny-list can.
//
// The result is length-capped, and a name that contributes nothing usable falls
// back to "split" so the header is never left with an empty or all-underscore
// filename.
std::string safe_filename_stem(const std::string& split_name) {
    constexpr std::size_t MAX_STEM = 60;

    std::string stem;
    stem.reserve(MAX_STEM);
    bool last_was_underscore = false;

    for (const char raw : split_name) {
        if (stem.size() >= MAX_STEM) break;
        const unsigned char c = static_cast<unsigned char>(raw);

        const bool allowed =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';

        if (allowed) {
            stem.push_back(static_cast<char>(c));
            last_was_underscore = false;
        } else if (!last_was_underscore) {
            // Runs of rejected characters collapse to a single '_' so that a
            // name written in a non-Latin script does not become 60 underscores.
            stem.push_back('_');
            last_was_underscore = true;
        }
    }

    // Trim the padding the substitution can leave at either end.
    const auto first = stem.find_first_not_of(" _-");
    if (first == std::string::npos) return "split";
    const auto last = stem.find_last_not_of(" _-");
    stem = stem.substr(first, last - first + 1);

    return stem.empty() ? "split" : stem;
}

// The date the export was taken, for the filename only. UTC so that two servers
// in different time zones name the same export the same way.
std::string today_utc() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
    if (gmtime_r(&now, &tm_utc) == nullptr) return "export";
    char buf[11];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_utc) == 0) return "export";
    return std::string(buf);
}

// ── SQL ──────────────────────────────────────────────────────────────────────

// One denormalized, rectangular result: a row per allocation, and a row per
// item or bill that has none.
//
// The LEFT JOINs are the contract, not an optimization. An item nobody has been
// assigned still produces a row with an empty member and share, and a bill with
// no items still produces a row with the item columns empty. Inner joins here
// would silently drop exactly the rows the export exists to surface — what
// nobody is on the hook for.
//
// Access is scoped into the query via split_role(): a split the caller neither
// owns nor is a member of matches no rows, and the caller cannot tell it apart
// from one that does not exist.
//
// `share` is the same expression share_routes.cpp uses, evaluated by Postgres
// in NUMERIC: ROUND(line_total * ratio / 100, 4) in ratio mode, the stored
// amount in amount mode. Every monetary column is cast to text in the database
// and concatenated as a string here; no value in this file is ever a C++
// double.
//
// The ORDER BY is what makes two exports of an unchanged split byte-identical:
// bill date, then the bill itself, then item insertion order, then member name.
// `id` follows `created_at` and `name` at every level because timestamps can
// collide on a bulk insert and two members of one split are allowed to share a
// name — without the id, either would leave the row order up to the planner.
const char* const EXPORT_SQL =
    "SELECT s.name, "
    "       to_char(b.date, 'YYYY-MM-DD'), "
    "       b.store_name, "
    "       b.currency, "
    "       COALESCE(i.name, ''), "
    "       COALESCE(i.price::text, ''), "
    "       COALESCE(i.quantity::text, ''), "
    "       COALESCE((i.price * i.quantity)::text, ''), "
    "       COALESCE(m.name, ''), "
    "       COALESCE(CASE WHEN a.allocation_mode = 'ratio' "
    "                     THEN ROUND(i.price * i.quantity * a.ratio / 100, 4) "
    "                     ELSE ROUND(a.amount, 4) END::text, '') "
    "  FROM splits s "
    "  JOIN bills b                 ON b.split_id = s.id "
    "  LEFT JOIN bill_items i       ON i.bill_id = b.id "
    "  LEFT JOIN item_allocations a ON a.bill_item_id = i.id "
    "  LEFT JOIN split_members m    ON m.id = a.member_id "
    " WHERE s.id = $1::uuid AND split_role(s.id, $2::uuid) IS NOT NULL "
    " ORDER BY b.date, b.created_at, b.id, i.created_at, i.id, m.name, m.id";

const char* const CSV_HEADER =
    "split,bill_date,store,currency,item,unit_price,quantity,line_total,"
    "member,share\r\n";

}  // namespace

void register_export_routes(BsApp& app, DbPool& pool) {

    // ── GET /api/splits/<id>/export/csv ──────────────────────────────────────
    // The whole split as one flat table: every line item, every allocation,
    // every share. Balances and settlements are deliberately absent — they come
    // from GET /api/splits/:id/summary, and a second copy of that SQL here
    // could drift from the first.
    CROW_ROUTE(app, "/api/splits/<string>/export/csv")
        .methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req, const std::string& id) {
        auto user = current_user(req, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id)) return json_error(404, "Split not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // Resolved separately from the export query because a split that is
            // accessible but empty must return a header-only CSV, while a split
            // that is missing or not the caller's (split_role NULL) must return 404.
            // The export query alone returns no rows in both cases.
            auto owned = txn.exec(
                "SELECT name FROM splits "
                " WHERE id = $1::uuid AND split_role(id, $2::uuid) IS NOT NULL",
                pqxx::params{id, user->id});
            if (owned.empty()) {
                txn.commit();
                return json_error(404, "Split not found");
            }
            const std::string split_name = owned[0][0].as<std::string>();

            auto rows = txn.exec(EXPORT_SQL, pqxx::params{id, user->id});
            txn.commit();

            std::string csv = CSV_HEADER;
            for (const auto& row : rows) {
                std::vector<std::string> fields;
                fields.reserve(row.size());
                for (const auto& field : row) fields.push_back(field.as<std::string>());
                append_row(csv, fields);
            }

            crow::response res(200, csv);
            res.set_header("Content-Type", "text/csv; charset=utf-8");
            res.set_header("Content-Disposition",
                           "attachment; filename=\"" +
                               safe_filename_stem(split_name) + "-" +
                               today_utc() + ".csv\"");
            return res;
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "export_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
    });
}
