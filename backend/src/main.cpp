#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "app.h"
#include "auth/session_sweeper.h"
#include "db/db_pool.h"
#include "routes/auth_routes.h"
#include "routes/member_routes.h"
#include "routes/split_routes.h"
#include "routes/bill_routes.h"
#include "routes/item_routes.h"
#include "routes/allocation_routes.h"
#include "routes/share_routes.h"
#include "routes/payment_routes.h"
#include "routes/summary_routes.h"
#include "routes/public_routes.h"
// Phase 5/7 route headers will be #include'd here as they are implemented:
// #include "routes/payment_routes.h"
// #include "routes/export_routes.h"
// #include "routes/currency_routes.h"

static std::string require_env(const char* name) {
    const char* val = std::getenv(name);
    if (!val || std::string(val).empty()) {
        throw std::runtime_error(
            std::string("Missing required environment variable: ") + name);
    }
    return val;
}

static std::string env_or(const char* name, const std::string& fallback) {
    const char* val = std::getenv(name);
    return (val && *val) ? std::string(val) : fallback;
}

int main() {
    try {
        const std::string db_conn_str  = require_env("DATABASE_URL");
        const std::string app_base_url = env_or("APP_BASE_URL", "http://localhost:5173");
        const int         port         = std::stoi(env_or("PORT", "8080"));

        DbPool pool(db_conn_str, 8);
        if (!pool.ping()) {
            std::cerr << "Warning: database unreachable at startup; "
                         "requests needing it will fail until it recovers.\n";
        }

        // Nothing else deletes expired sessions; without this the table grows
        // for the life of the deployment.
        SessionSweeper sweeper(pool);
        sweeper.start();

        BsApp app;

        app.get_middleware<crow::CORSHandler>()
            .global()
            .headers("Content-Type", "Authorization", "Cookie")
            .methods(crow::HTTPMethod::GET, crow::HTTPMethod::POST,
                     crow::HTTPMethod::PUT, crow::HTTPMethod::DELETE,
                     crow::HTTPMethod::OPTIONS)
            .origin(app_base_url);

        register_auth_routes(app, pool);
        register_split_routes(app, pool);
        register_member_routes(app, pool);
        register_bill_routes(app, pool);
        register_item_routes(app, pool);
        register_allocation_routes(app, pool);
        register_share_routes(app, pool);
        register_payment_routes(app, pool);
        register_summary_routes(app, pool);
        register_public_routes(app, pool);
        // register_payment_routes(app, pool);
        // register_export_routes(app, pool);
        // register_currency_routes(app, pool);

        // Crow answers an unmatched URL with a plain-text "404 Not Found",
        // which breaks the API's own rule that every failure carries an
        // {"error": "..."} body. A client parsing responses as JSON would hit
        // a parse error instead of a message it can show.
        CROW_CATCHALL_ROUTE(app)([](crow::response& res) {
            if (res.code == 404) {
                res.body = R"({"error":"Not found"})";
            } else if (res.code == 405) {
                res.body = R"({"error":"Method not allowed"})";
            } else if (res.body.empty()) {
                res.body = R"({"error":"Request failed"})";
            }
            res.add_header("Content-Type", "application/json");
            res.end();
        });

        CROW_ROUTE(app, "/api/health")([] {
            crow::response res(200, R"({"status":"ok"})");
            res.add_header("Content-Type", "application/json");
            return res;
        });

        std::cout << "BillSplitter backend listening on port " << port << "\n";
        app.port(static_cast<uint16_t>(port)).multithreaded().run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
}
