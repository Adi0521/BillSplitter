#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers all /api/splits routes (list, create, detail, update, archive)
// on the given Crow app.
void register_split_routes(BsApp& app, DbPool& pool);
