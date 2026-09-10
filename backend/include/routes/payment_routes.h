#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers all /api/splits/<split_id>/payments routes on the given Crow app.
//
// A payment records that money actually moved. It never changes what anyone
// owes, so nothing here is validated against a balance: overpaying, paying
// early, and settling someone else's debt are all real things people do.
//
// Every route is split-scoped. The split must exist AND be owned by the session
// user, and both member ids must belong to that split — all proven inside the
// SQL, never by fetching a row and comparing in C++.
void register_payment_routes(BsApp& app, DbPool& pool);
