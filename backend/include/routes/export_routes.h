#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers the /api/splits/<id>/export/csv route on the given Crow app.
void register_export_routes(BsApp& app, DbPool& pool);
