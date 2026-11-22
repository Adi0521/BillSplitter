#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers all /api/auth/google/* routes on the given Crow app.
void register_google_routes(BsApp& app, DbPool& pool);
