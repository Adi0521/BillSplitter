#include "routes/invite_routes.h"
#include "auth/middleware.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <cctype>
#include <string>

using json = nlohmann::json;

namespace {

// ── Response helpers ─────────────────────────────────────────────────────────

crow::response json_error(int code, const std::string& message) {
    crow::response res(code, json({{"error", message}}).dump());
    res.add_header("Content-Type", "application/json");
    return res;
}

// The one message an unresolvable invite ever produces. A token that never
// existed, one that was revoked or replaced, one that was already claimed, and
// one that is not even the right shape must all be indistinguishable — anything
// else tells a caller *why* their link failed, which is an oracle for which
// links used to be valid.
const char* const INVITE_NOT_FOUND = "Invite not found";

// ── Input helpers ────────────────────────────────────────────────────────────

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

// invite_token is encode(gen_random_bytes(16), 'hex') — exactly 32 lowercase
// hex characters. Anything else cannot be a token, so it is rejected before it
// reaches the database rather than after. (The column is TEXT, so a malformed
// token would not error in SQL; the check is for the constant-shape guarantee
// and so the not-found path never depends on what the database does with
// arbitrary strings.)
bool is_invite_token(const std::string& s) {
    if (s.size() != 32) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

}  // namespace

void register_invite_routes(BsApp& app, DbPool& pool) {

    // ── POST /api/splits/<id>/members/<mid>/invite ───────────────────────────
    //
    // Mints a fresh token for a seat, replacing any previous one so the old
    // link stops working. Owner only: a member gets 403, no role gets 404, as
    // does a seat that is not in this split. A seat that already has an
    // account is a 400 — there is nothing left to invite.
    CROW_ROUTE(app, "/api/splits/<string>/members/<string>/invite")
        .methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req,
             const std::string& split_id,
             const std::string& member_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_uuid(split_id) || !is_uuid(member_id)) {
            return json_error(404, "Member not found");
        }

        try {
            auto conn = pool.acquire();

            // invite_token is UNIQUE. A collision between two 128-bit random
            // values is not a real event, but the constraint can still fire,
            // and a 500 for something a retry fixes would be wrong. Each
            // attempt is its own transaction because a failed statement
            // poisons the one it ran in.
            for (int attempt = 0; attempt < 3; ++attempt) {
                try {
                    pqxx::work txn(*conn);

                    // Role, seat lookup and the write are one statement. The
                    // UPDATE fires only when the role is 'owner' and the seat
                    // is unlinked; the flags come back alongside so the
                    // handler can pick 404 / 400 / 403 without a second query.
                    auto rows = txn.exec(
                        "WITH target AS ("
                        "    SELECT s.id, split_role(s.id, $3::uuid) AS role"
                        "      FROM splits s WHERE s.id = $2::uuid"
                        "), seat AS ("
                        "    SELECT m.id, (m.user_id IS NOT NULL) AS linked"
                        "      FROM split_members m JOIN target t ON m.split_id = t.id"
                        "     WHERE m.id = $1::uuid"
                        "), minted AS ("
                        "    UPDATE split_members m"
                        "       SET invite_token = encode(gen_random_bytes(16), 'hex'),"
                        "           invited_at   = now()"
                        "      FROM target t, seat"
                        "     WHERE m.id = seat.id AND t.role = 'owner' AND NOT seat.linked"
                        "    RETURNING m.invite_token"
                        ") SELECT t.role,"
                        "         seat.id IS NOT NULL            AS seat_exists,"
                        "         COALESCE(seat.linked, false)   AS linked,"
                        "         (SELECT invite_token FROM minted) AS token"
                        "    FROM target t LEFT JOIN seat ON true",
                        pqxx::params{member_id, split_id, user->id});

                    if (rows.empty() || rows[0]["role"].is_null()) {
                        txn.abort();
                        return json_error(404, "Member not found");
                    }
                    const pqxx::row& r = rows[0];
                    if (!r["seat_exists"].as<bool>()) {
                        txn.abort();
                        return json_error(404, "Member not found");
                    }
                    if (r["role"].as<std::string>() != "owner") {
                        txn.abort();
                        return json_error(403, "Only the owner can invite members");
                    }
                    if (r["linked"].as<bool>()) {
                        txn.abort();
                        return json_error(400, "This member already has an account linked");
                    }
                    txn.commit();

                    const std::string token = r["token"].as<std::string>();
                    res.code = 200;
                    res.body = json({
                        {"invite_token", token},
                        {"invite_path",  "/invite/" + token},
                    }).dump();
                    return res;
                } catch (const pqxx::unique_violation& e) {
                    CROW_LOG_WARNING << "invite_routes: invite_token collision, retrying: "
                                     << e.what();
                }
            }
            return json_error(500, "Could not generate a unique invite token");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "invite_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
    });

    // ── DELETE /api/splits/<id>/members/<mid>/invite ─────────────────────────
    //
    // Revokes the seat's token so the link stops resolving. Owner only, same
    // 403 / 404 split as minting. Revoking a seat with no pending invite is a
    // no-op that still returns 200: the state the caller asked for holds.
    CROW_ROUTE(app, "/api/splits/<string>/members/<string>/invite")
        .methods(crow::HTTPMethod::DELETE)
    ([&pool](const crow::request& req,
             const std::string& split_id,
             const std::string& member_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_uuid(split_id) || !is_uuid(member_id)) {
            return json_error(404, "Member not found");
        }

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto rows = txn.exec(
                "WITH target AS ("
                "    SELECT s.id, split_role(s.id, $3::uuid) AS role"
                "      FROM splits s WHERE s.id = $2::uuid"
                "), seat AS ("
                "    SELECT m.id"
                "      FROM split_members m JOIN target t ON m.split_id = t.id"
                "     WHERE m.id = $1::uuid"
                "), revoked AS ("
                "    UPDATE split_members m SET invite_token = NULL"
                "      FROM target t, seat"
                "     WHERE m.id = seat.id AND t.role = 'owner'"
                "    RETURNING m.id"
                ") SELECT t.role, seat.id IS NOT NULL AS seat_exists"
                "    FROM target t LEFT JOIN seat ON true",
                pqxx::params{member_id, split_id, user->id});

            if (rows.empty() || rows[0]["role"].is_null()) {
                txn.abort();
                return json_error(404, "Member not found");
            }
            if (!rows[0]["seat_exists"].as<bool>()) {
                txn.abort();
                return json_error(404, "Member not found");
            }
            if (rows[0]["role"].as<std::string>() != "owner") {
                txn.abort();
                return json_error(403, "Only the owner can revoke an invite");
            }
            txn.commit();

            res.code = 200;
            res.body = R"({"message":"Invite revoked"})";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "invite_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── GET /api/invites/<token> ─────────────────────────────────────────────
    //
    // Enough to render "You've been invited to join X as Bob, by Alice". Any
    // signed-in user may look, because holding the link IS the credential.
    // invited_by is the owner's display name (or their email's local part) —
    // never the email itself, which the invitee has no business learning here.
    CROW_ROUTE(app, "/api/invites/<string>").methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req, const std::string& token) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_invite_token(token)) return json_error(404, INVITE_NOT_FOUND);

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // `m.user_id IS NULL` is belt-and-braces: claiming clears the token
            // in the same statement that sets user_id, so a linked seat should
            // never still carry one. If it somehow did, the link must be dead.
            auto rows = txn.exec(
                "SELECT s.name, m.name,"
                "       COALESCE(NULLIF(btrim(u.display_name), ''),"
                "                split_part(u.email, '@', 1))          AS invited_by,"
                "       split_role(s.id, $2::uuid) IS NOT NULL          AS already_member,"
                "       s.id                                             AS split_id"
                "  FROM split_members m"
                "  JOIN splits s ON s.id = m.split_id"
                "  JOIN users  u ON u.id = s.owner_id"
                " WHERE m.invite_token = $1 AND m.user_id IS NULL",
                pqxx::params{token, user->id});
            txn.commit();

            if (rows.empty()) return json_error(404, INVITE_NOT_FOUND);

            const pqxx::row& r = rows[0];
            res.code = 200;
            // split_id lets the UI deep-link when the caller is already a
            // member, instead of guessing by name (names are not unique).
            res.body = json({
                {"split_name",     r[0].as<std::string>()},
                {"member_name",    r[1].as<std::string>()},
                {"invited_by",     r[2].as<std::string>()},
                {"already_member", r[3].as<bool>()},
                {"split_id",       r[4].as<std::string>()},
            }).dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "invite_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── POST /api/invites/<token>/claim ──────────────────────────────────────
    //
    // Binds the caller's account to the seat: user_id, linked_at and the token
    // are all written in one statement, so a link works exactly once. A caller
    // who already holds a seat in that split (the owner included) gets 409; the
    // partial unique index (split_id, user_id) enforces the same rule under a
    // race, and that violation is also reported as 409, never 500.
    CROW_ROUTE(app, "/api/invites/<string>/claim").methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req, const std::string& token) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_invite_token(token)) return json_error(404, INVITE_NOT_FOUND);

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // The UPDATE repeats the token / unlinked predicate rather than
            // joining on inv.id alone: under READ COMMITTED, a second claim
            // that blocks on the row lock re-evaluates the UPDATE's WHERE
            // against the committed row, whose token is now NULL, and so
            // updates nothing. Without that repeat both racers could win.
            auto rows = txn.exec(
                "WITH inv AS ("
                "    SELECT m.id, m.split_id, split_role(m.split_id, $2::uuid) AS role"
                "      FROM split_members m"
                "     WHERE m.invite_token = $1 AND m.user_id IS NULL"
                "), claimed AS ("
                "    UPDATE split_members m"
                "       SET user_id = $2::uuid, linked_at = now(), invite_token = NULL"
                "      FROM inv"
                "     WHERE m.id = inv.id AND inv.role IS NULL"
                "       AND m.invite_token = $1 AND m.user_id IS NULL"
                "    RETURNING m.id"
                ") SELECT inv.split_id,"
                "         inv.role IS NOT NULL               AS already_member,"
                "         (SELECT COUNT(*) FROM claimed) > 0  AS claimed"
                "    FROM inv",
                pqxx::params{token, user->id});

            if (rows.empty()) {
                txn.abort();
                return json_error(404, INVITE_NOT_FOUND);
            }
            const pqxx::row& r = rows[0];
            if (r["already_member"].as<bool>()) {
                txn.abort();
                crow::response conflict(409, json({
                    {"error",    "You already have a seat in this split"},
                    {"split_id", r["split_id"].as<std::string>()},
                }).dump());
                conflict.add_header("Content-Type", "application/json");
                return conflict;
            }
            if (!r["claimed"].as<bool>()) {
                // Lost a race: the row was claimed between snapshot and lock.
                txn.abort();
                return json_error(404, INVITE_NOT_FOUND);
            }
            txn.commit();

            res.code = 200;
            res.body = json({{"split_id", r["split_id"].as<std::string>()}}).dump();
        } catch (const pqxx::unique_violation& e) {
            CROW_LOG_WARNING << "invite_routes: duplicate seat on claim: " << e.what();
            return json_error(409, "You already have a seat in this split");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "invite_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── POST /api/splits/<id>/leave ──────────────────────────────────────────
    //
    // Unlinks the caller's account from their seat. The seat itself, and every
    // allocation and payment recorded against it, stays: they are part of the
    // ledger and must survive the person's departure. The owner cannot leave
    // (400 — archive instead); no role is the usual 404.
    CROW_ROUTE(app, "/api/splits/<string>/leave").methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req, const std::string& split_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_uuid(split_id)) return json_error(404, "Split not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto rows = txn.exec(
                "WITH target AS ("
                "    SELECT s.id, split_role(s.id, $2::uuid) AS role"
                "      FROM splits s WHERE s.id = $1::uuid"
                "), left_seat AS ("
                "    UPDATE split_members m SET user_id = NULL, linked_at = NULL"
                "      FROM target t"
                "     WHERE m.split_id = t.id AND m.user_id = $2::uuid AND t.role = 'member'"
                "    RETURNING m.id"
                ") SELECT t.role FROM target t",
                pqxx::params{split_id, user->id});

            if (rows.empty() || rows[0][0].is_null()) {
                txn.abort();
                return json_error(404, "Split not found");
            }
            if (rows[0][0].as<std::string>() == "owner") {
                txn.abort();
                return json_error(400, "The owner cannot leave a split; archive it instead");
            }
            txn.commit();

            res.code = 200;
            res.body = R"({"message":"Left"})";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "invite_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });
}
