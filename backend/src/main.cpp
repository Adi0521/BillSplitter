#include <crow.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include "db/db_pool.h"
#include "routes/auth_routes.h"
// Phase 2+ route headers will be #include'd here as they are implemented:
// #include "routes/split_routes.h"
// #include "routes/bill_routes.h"
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

int main() {
    std::string db_conn_str;
    std::string app_base_url;
    int         port = 8080;

    try {
        db_conn_str  = require_env("DATABASE_URL");
        app_base_url = std::getenv("APP_BASE_URL")
                           ? std::getenv("APP_BASE_URL")
                           : "http://localhost:5173";
        if (auto* p = std::getenv("PORT")) port = std::stoi(p);
    } catch (const std::exception& e) {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }

    DbPool pool(db_conn_str, 8);

    crow::SimpleApp app;

    app.get_middleware<crow::CORSHandler>()
        .global()
        .headers("Content-Type, Authorization, Cookie")
        .methods("GET, POST, PUT, DELETE, OPTIONS")
        .origin(app_base_url);

    register_auth_routes(app, pool);
    // register_split_routes(app, pool);
    // register_bill_routes(app, pool);
    // register_payment_routes(app, pool);
    // register_export_routes(app, pool);
    // register_currency_routes(app, pool);

    CROW_ROUTE(app, "/api/health")([] {
        return crow::response(200, R"({"status":"ok"})");
    });

    std::cout << "BillSplitter backend starting on port " << port << "\n";
    app.port(port).multithreaded().run();
    return 0;
}
