#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers GET /api/splits/<id>/summary on the given Crow app.
//
// The endpoint is read-only. It generalizes the per-bill shares endpoint to the
// whole split: every member's share of every bill (items plus proportional
// tax/tip/fees, over the bill's FULL subtotal), netted against what they
// fronted as payer and against the payments already recorded, giving
//
//     balance = fronted + payments_made - owes - payments_received
//
// Results are grouped by the currency of the bills they came from and are never
// summed across currencies: FX conversion is Phase 7, so a split holding a EUR
// bill and a USD bill has two independent sets of balances.
//
// Every monetary value is derived by Postgres in NUMERIC and returned as a
// string; no amount passes through a double. The one exception is the suggested
// `settlements` list, which only *pairs* balances that Postgres already
// computed and does so in integer arithmetic on 1/10000ths.
//
// Ownership is proven inside the SQL by requiring splits.owner_id = <session
// user>; another user's split, and a malformed id, are both a 404.
void register_summary_routes(BsApp& app, DbPool& pool);
