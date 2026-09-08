#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers all /api/bills/<bill_id>/items/<item_id> allocation routes:
//
//   GET  /api/bills/<bid>/items/<iid>/allocations
//   PUT  /api/bills/<bid>/items/<iid>/allocations
//   POST /api/bills/<bid>/items/<iid>/even-split
//
// Like the item routes, these URLs carry no split id and no owner, so ownership
// is established in SQL by joining bill_items -> bills -> splits and requiring
// splits.owner_id = <session user>. An item on someone else's split — or an
// item id that exists under a different bill — is a 404, never a 403, so the
// API never confirms which ids exist.
//
// Every monetary and ratio value is computed by Postgres in NUMERIC and crosses
// the wire as a string. No share, sum or remainder is ever calculated in C++:
// a share is ROUND(line_total * ratio / 100, 4) in ratio mode and the stored
// amount in amount mode, and the rounded shares are deliberately NOT forced to
// tie to line_total. The difference comes back as `unallocated` rather than
// being handed to whichever member happens to sort first.
void register_allocation_routes(BsApp& app, DbPool& pool);
