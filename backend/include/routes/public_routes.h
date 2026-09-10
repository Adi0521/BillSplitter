#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers the public share-link routes:
//
//   GET  /api/splits/share/<token>            unauthenticated, read-only
//   POST /api/splits/<id>/share/regenerate    owner only
//
// The GET is the only endpoint in the API that returns user data without a
// session, so its response is assembled field by field in the .cpp rather than
// by serializing whatever a query happened to return.
void register_public_routes(BsApp& app, DbPool& pool);
