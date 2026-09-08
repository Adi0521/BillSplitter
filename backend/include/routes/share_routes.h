#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers GET /api/splits/<id>/bills/<bid>/shares on the given Crow app.
//
// The endpoint is read-only: it aggregates item_allocations into what each
// member of the split owes on one bill, plus their proportional part of tax,
// tip and fees. Every number is computed by Postgres in NUMERIC and returned as
// a string, so no monetary value ever passes through a C++ double.
//
// Ownership is proven inside the SQL by joining bills -> splits and requiring
// splits.owner_id = <session user>; another user's bill is a 404.
void register_share_routes(BsApp& app, DbPool& pool);
