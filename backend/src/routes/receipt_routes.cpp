#include "routes/receipt_routes.h"

#include "auth/middleware.h"
#include "services/receipt/line_parser.h"
#include "services/receipt/ocr.h"

#include <crow.h>
#include <crow/multipart.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// ── Limits ───────────────────────────────────────────────────────────────────

// 10 MB, per docs/api.md. A phone photo of a receipt is well under 5 MB; the
// cap exists because this is the only endpoint that accepts a file, and an
// unbounded upload is a memory-exhaustion vector against a server that buffers
// request bodies.
constexpr std::size_t kMaxUploadBytes = 10 * 1024 * 1024;

// How much of the file is examined to decide what it is. Every signature this
// looks for lives in the first few bytes; the window is only this wide so that
// an HTML document with a long comment or BOM preamble is still recognized.
constexpr std::size_t kSniffWindow = 1024;

// ── Response helpers ─────────────────────────────────────────────────────────

crow::response json_error(int code, const std::string& message) {
    crow::response res(code, json({{"error", message}}).dump());
    res.add_header("Content-Type", "application/json");
    return res;
}

// ── Input helpers ────────────────────────────────────────────────────────────

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

// Content-Length as sent by the client. Returns false when the header is
// absent or not a plain decimal number, in which case the actual buffered body
// size is the only thing to go on.
bool declared_length(const crow::request& req, unsigned long long& out) {
    const std::string& raw = req.get_header_value("Content-Length");
    if (raw.empty()) return false;
    out = 0;
    for (const char c : raw) {
        if (c < '0' || c > '9') return false;
        if (out > (kMaxUploadBytes + 1)) return true;   // already over; stop early
        out = out * 10 + static_cast<unsigned long long>(c - '0');
    }
    return true;
}

// ── Content sniffing ─────────────────────────────────────────────────────────
//
// The part's declared Content-Type is not consulted anywhere. It is chosen by
// the client, so `receipt.png` claiming `image/png` while containing something
// else must not be handed to the decoder on the strength of the claim. Magic
// bytes are the only evidence used.

enum class FileKind { Image, Html, Pdf, Unsupported, Unknown };

bool has_prefix(const std::string& b, const char* sig, std::size_t n) {
    return b.size() >= n && std::memcmp(b.data(), sig, n) == 0;
}

bool contains_ci(const std::string& hay, std::size_t limit, const char* needle) {
    const std::size_t n = std::strlen(needle);
    const std::size_t end = std::min(hay.size(), limit);
    if (n > end) return false;
    for (std::size_t i = 0; i + n <= end; ++i) {
        std::size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(hay[i + k])) ==
                            std::tolower(static_cast<unsigned char>(needle[k]))) {
            ++k;
        }
        if (k == n) return true;
    }
    return false;
}

// `label` is filled for kinds that need to name the format in an error.
FileKind sniff(const std::string& b, std::string& label) {
    // PDF first, and by name: tesseract cannot read one and leptonica returns
    // a bare decode failure, so without this check the user would be told
    // their perfectly good receipt was corrupt.
    if (has_prefix(b, "%PDF-", 5) || contains_ci(b, 64, "%PDF-")) {
        label = "PDF";
        return FileKind::Pdf;
    }

    if (has_prefix(b, "\x89PNG\r\n\x1a\n", 8)) { label = "PNG";  return FileKind::Image; }
    if (has_prefix(b, "\xFF\xD8\xFF", 3))      { label = "JPEG"; return FileKind::Image; }
    if (has_prefix(b, "II*\0", 4) ||
        has_prefix(b, "MM\0*", 4))             { label = "TIFF"; return FileKind::Image; }
    if (b.size() >= 12 && std::memcmp(b.data(), "RIFF", 4) == 0 &&
        std::memcmp(b.data() + 8, "WEBP", 4) == 0) { label = "WebP"; return FileKind::Image; }

    // Recognized, but not on the accepted list. Named explicitly so the user
    // is told to convert rather than left guessing why their image failed.
    if (has_prefix(b, "GIF87a", 6) || has_prefix(b, "GIF89a", 6)) {
        label = "GIF";
        return FileKind::Unsupported;
    }
    if (has_prefix(b, "BM", 2)) { label = "BMP"; return FileKind::Unsupported; }

    // HTML: a text document whose first non-space character opens a tag. The
    // NUL check keeps a binary file that happens to start with '<' out.
    std::size_t i = 0;
    if (has_prefix(b, "\xEF\xBB\xBF", 3)) i = 3;   // UTF-8 BOM
    while (i < b.size() && std::isspace(static_cast<unsigned char>(b[i]))) ++i;
    if (i < b.size() && b[i] == '<') {
        const std::size_t end = std::min(b.size(), kSniffWindow);
        if (b.find('\0', 0) >= end) {
            label = "HTML";
            return FileKind::Html;
        }
    }
    return FileKind::Unknown;
}

// ── Serialization ────────────────────────────────────────────────────────────

// Confidence is the one non-money number in this payload, and it is a hint
// about legibility rather than a quantity, so it is rounded to two places: a
// UI threshold does not care about the 14th digit and "0.9599999999999999" in
// the response body invites someone to think it does.
double round2(double v) {
    if (!std::isfinite(v)) return 0.0;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return std::round(v * 100.0) / 100.0;
}

// A string field the receipt did not carry is JSON null, not "": the contract
// distinguishes "the receipt printed no tax line" from "the tax was zero", and
// only null can say the first.
json or_null(const std::string& s) {
    return s.empty() ? json(nullptr) : json(s);
}

json draft_to_json(const receipt::Draft& d,
                   const char*           source,
                   const std::string&    currency) {
    json items = json::array();
    for (const auto& item : d.items) {
        items.push_back(json{
            {"name",       item.name},
            {"price",      item.price},          // 4dp string, never a float
            {"quantity",   item.quantity},
            {"confidence", round2(item.confidence)},
        });
    }

    json j;
    j["source"]          = source;
    j["store_name"]      = d.store_name;         // "" when unknown, never null
    j["date"]            = or_null(d.date);
    // The split's currency, read from the database. OCR does not guess it: a
    // receipt's "$" says nothing about USD vs CAD vs AUD, and the split already
    // knows the answer.
    j["currency"]        = currency;
    j["items"]           = items;
    j["tax"]             = or_null(d.tax);
    j["tip"]             = or_null(d.tip);
    j["total_read"]      = or_null(d.total_read);
    j["items_total"]     = d.items_total;
    // null, not true, when the receipt printed no total. "The totals agree" and
    // "there was nothing to compare against" are different facts, and a client
    // reading `true` for the second would report a check that never ran.
    j["totals_agree"]    = d.total_read.empty() ? json(nullptr) : json(d.totals_agree);
    j["unmatched_lines"] = d.unmatched_lines;
    j["warnings"]        = d.warnings;
    return j;
}

}  // namespace

void register_receipt_routes(BsApp& app, DbPool& pool) {

    // ── POST /api/splits/<id>/bills/parse ────────────────────────────────────
    // multipart/form-data with a `file` field → 200 ReceiptDraft.
    //
    // Persists nothing. Not a single row is written by this handler; the draft
    // is confirmed through POST /bills and POST /bills/:bid/items, which run
    // the same validation a hand-typed bill runs. There is no path by which an
    // OCR'd number reaches the database without being seen by a human first.
    CROW_ROUTE(app, "/api/splits/<string>/bills/parse")
        .methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req, const std::string& split_id) {
        // The size check runs before authentication, and deliberately so: it
        // is the cheapest possible answer, needs no database round-trip, and
        // the whole point of the cap is to refuse a huge body before spending
        // anything on it. Telling an anonymous caller "too large" leaks
        // nothing — the limit is documented.
        //
        // Crow buffers a request body before dispatching to the handler, so
        // this is the earliest point in *this* module at which the request can
        // be seen. Rejecting on Content-Length still short-circuits the OCR
        // pipeline and the multipart copy, but a truly pre-buffer cap would
        // have to live in the connection layer (app.h / main.cpp), which this
        // module does not own. Flagged in the handoff notes.
        unsigned long long declared = 0;
        if (declared_length(req, declared) && declared > kMaxUploadBytes) {
            return json_error(413, "The uploaded file is too large. The limit is 10 MB.");
        }
        // Chunked uploads arrive without a Content-Length, so the buffered
        // body is checked too.
        if (req.body.size() > kMaxUploadBytes) {
            return json_error(413, "The uploaded file is too large. The limit is 10 MB.");
        }

        try {
            // current_user() rather than require_auth(): it has no output
            // parameter, so there is no way to fill a response and then
            // accidentally return a different one. It touches the database,
            // which is why it sits inside the try — an outage here is a 500
            // with the API's error envelope, not an escaped exception that
            // Crow answers with a bare page.
            auto user = current_user(req, pool);
            if (!user) return unauthorized();

            if (!is_uuid(split_id)) return json_error(404, "Split not found");

            // Access and the split's currency come out of one query, and access
            // is scoped into the WHERE clause via split_role(): a split the caller
            // neither owns nor is a member of yields no rows, so it is
            // indistinguishable from one that does not exist. Nesting under
            // /api/splits/<id>/ proves nothing.
            std::string currency;
            {
                auto conn = pool.acquire();
                pqxx::work txn(*conn);
                auto rows = txn.exec(
                    "SELECT currency FROM splits "
                    " WHERE id = $1::uuid AND split_role(id, $2::uuid) IS NOT NULL",
                    pqxx::params{split_id, user->id});
                txn.commit();
                if (rows.empty()) return json_error(404, "Split not found");
                currency = rows[0][0].as<std::string>();
            }
            // The connection is back in the pool before OCR starts. Recognition
            // takes seconds; holding one of eight connections for that long
            // would let a handful of uploads starve every other request.

            // Crow throws crow::bad_request when the Content-Type carries no
            // multipart boundary, and its parser throws on a truncated body.
            // Both are malformed client input: a 400, not a 500.
            std::string file_body;
            try {
                crow::multipart::message form(req);
                if (form.part_map.count("file") == 0) {
                    return json_error(400,
                        "Attach the receipt as a multipart form field named \"file\".");
                }
                file_body = form.get_part_by_name("file").body;
            } catch (const std::exception&) {
                return json_error(400,
                    "The upload could not be read. Send multipart/form-data with a "
                    "\"file\" field.");
            }

            if (file_body.empty()) {
                return json_error(400, "The uploaded file is empty.");
            }
            if (file_body.size() > kMaxUploadBytes) {
                return json_error(413, "The uploaded file is too large. The limit is 10 MB.");
            }

            std::string label;
            const FileKind kind = sniff(file_body, label);

            if (kind == FileKind::Pdf) {
                return json_error(415,
                    "PDF receipts are not supported yet. Tesseract cannot read a PDF "
                    "directly. Please upload a photo or screenshot (PNG, JPEG, WebP or "
                    "TIFF), or the receipt's HTML export.");
            }
            if (kind == FileKind::Unsupported) {
                return json_error(415,
                    label + " files are not supported. Please upload a PNG, JPEG, WebP "
                    "or TIFF image, or an HTML receipt.");
            }
            if (kind == FileKind::Unknown) {
                return json_error(400,
                    "This file is not a receipt image or an HTML receipt. Accepted "
                    "types are PNG, JPEG, WebP, TIFF and HTML.");
            }

            const char*              source = "image";
            std::vector<std::string> lines;
            std::vector<double>      confidences;

            if (kind == FileKind::Html) {
                // HTML is already text. Running it through OCR would rasterize
                // nothing and invent errors in a document that has none, so
                // the tags are simply stripped and the same parser is used.
                // Confidences are left empty: nothing here was guessed, and
                // parse_lines reads an empty vector as "no confidence data".
                source = "html";
                lines  = receipt::split_text_lines(receipt::html_to_text(file_body));
            } else {
                try {
                    const auto ocr_lines = receipt::recognize_image(file_body);
                    lines.reserve(ocr_lines.size());
                    confidences.reserve(ocr_lines.size());
                    for (const auto& line : ocr_lines) {
                        lines.push_back(line.text);
                        confidences.push_back(line.confidence);
                    }
                } catch (const receipt::ImageDecodeError& e) {
                    // Truncated, corrupt, or a format leptonica cannot open.
                    // The client sent a bad file: 400.
                    return json_error(400, e.what());
                }
                // receipt::OcrUnavailable is deliberately not caught here: a
                // missing tessdata is a server fault and belongs in the 500
                // below, not in a message blaming the user's photo.
            }

            receipt::Draft draft = receipt::parse_lines(lines, confidences);

            // The parser cannot tell "the receipt had no items" from "no text
            // reached me", so the empty-input case is explained here.
            if (lines.empty()) {
                draft.warnings.push_back(
                    kind == FileKind::Html
                        ? "No text could be found in this HTML file."
                        : "No text could be read from this image. Try a sharper, "
                          "straighter photo in better light.");
            }

            crow::response res(200, draft_to_json(draft, source, currency).dump());
            res.add_header("Content-Type", "application/json");
            return res;

        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "receipt parse failed: " << e.what();
            return json_error(500, "Something went wrong reading this receipt");
        }
    });
}
