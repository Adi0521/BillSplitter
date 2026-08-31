#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers all /api/splits/<id>/bills routes (list, create, detail, update,
// delete) on the given Crow app. Bill *items* live under /api/bills/<bid>/items
// and are registered separately by register_item_routes().
void register_bill_routes(BsApp& app, DbPool& pool);
