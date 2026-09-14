#pragma once
#include "app.h"
#include "db/db_pool.h"

// Registers the receipt parsing routes on the given Crow app.
//
// There is one: POST /api/splits/<id>/bills/parse. It reads an uploaded
// receipt, OCRs or de-HTMLs it, and returns a ReceiptDraft. It persists
// nothing — the user reviews the draft and confirms it through the ordinary
// Phase 3 bill and item endpoints, which apply the ordinary validation.
void register_receipt_routes(BsApp& app, DbPool& pool);
