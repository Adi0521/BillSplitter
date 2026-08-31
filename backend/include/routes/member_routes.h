#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers all /api/splits/<split_id>/members routes on the given Crow app.
//
// Every route here is split-scoped: the split must exist AND be owned by the
// session user, and a member id must belong to that split. Both facts are
// proven inside the SQL, never by fetching a row and comparing in C++.
void register_member_routes(BsApp& app, DbPool& pool);
