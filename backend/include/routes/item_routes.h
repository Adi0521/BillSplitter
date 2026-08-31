#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers all /api/bills/<bill_id>/items routes on the given Crow app.
//
// These URLs carry no split id and no owner, so ownership cannot be inferred
// from the path: every route proves it by joining bill_items -> bills -> splits
// and requiring splits.owner_id = <session user>, inside the SQL. A bill on
// someone else's split is indistinguishable from one that does not exist (404).
//
// Every mutation also recomputes the parent bill's subtotal in the same
// transaction as the item write, so the stored subtotal can never disagree with
// the items it summarizes.
void register_item_routes(BsApp& app, DbPool& pool);
