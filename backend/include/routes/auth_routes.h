#pragma once
#include <crow.h>
#include "db/db_pool.h"

// Registers all /api/auth/* routes on the given Crow app.
void register_auth_routes(crow::SimpleApp& app, DbPool& pool);
