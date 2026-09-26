#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers the invite-link routes that bind an account to a member seat:
//
//   POST   /api/splits/<id>/members/<mid>/invite   owner only — mint a token
//   DELETE /api/splits/<id>/members/<mid>/invite   owner only — revoke it
//   GET    /api/invites/<token>                    any signed-in user — preview
//   POST   /api/invites/<token>/claim              any signed-in user — claim
//   POST   /api/splits/<id>/leave                  member only — unlink self
//
// The token is the secret (emails are never verified in this system), so it is
// returned exactly once, from the endpoint that mints it, and never listed.
// Access is decided by the split_role() SQL function inside each statement.
void register_invite_routes(BsApp& app, DbPool& pool);
