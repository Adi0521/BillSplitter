# API Contract

The authoritative request/response shapes. Backend and frontend are built
against this document, so change it here first, then change the code.

Conventions that hold everywhere:

- All responses are `Content-Type: application/json`.
- Errors are always `{"error": "<human readable message>"}`.
- List endpoints return a **bare JSON array**. Single resources return an object.
- Timestamps are Postgres `TIMESTAMPTZ` rendered as strings, passed through as-is.
- Money is `NUMERIC(12,4)`, serialized as a **string** to avoid float rounding
  (e.g. `"12.5000"`). Not applicable in Phase 2; stated here so it is settled.
- Every split-scoped resource is authorized by `splits.owner_id = <session user>`.
  A split that exists but is not owned by the caller returns **404, not 403**, so
  the API does not leak which split ids exist.
- Status codes: 200 OK, 201 Created (body = created object), 400 invalid input,
  401 unauthenticated, 404 not found or not owned, 409 conflict, 500 unexpected.

---

## Phase 1 — Auth (implemented)

| Method | Path | Body | Response |
|---|---|---|---|
| POST | `/api/auth/register` | `{email, password, display_name?}` | 201 `User` + `Set-Cookie: session=` |
| POST | `/api/auth/login` | `{email, password}` | 200 `User` + `Set-Cookie: session=` |
| POST | `/api/auth/logout` | — | 200 `{"message":"Logged out"}` |
| GET | `/api/auth/me` | — | 200 `User` |

```jsonc
// User
{ "id": "uuid", "email": "a@b.com", "display_name": "Alice", "created_at": "..." }
```

---

## Phase 2 — Splits

```jsonc
// Split
{
  "id":           "uuid",
  "name":         "Tahoe trip",
  "description":  "",              // "" when null, never null
  "type":         "one_time",      // "one_time" | "ongoing"
  "currency":     "USD",
  "share_token":  "hex32",
  "created_at":   "2026-08-30 22:51:45.947108+00",
  "archived_at":  null,            // string when archived
  "member_count": 3                // computed, present on list and detail
}
```

| Method | Path | Body | Response |
|---|---|---|---|
| GET | `/api/splits` | — | 200 `[Split]` |
| POST | `/api/splits` | `{name, description?, type, currency?}` | 201 `Split` |
| GET | `/api/splits/:id` | — | 200 `SplitDetail` |
| PUT | `/api/splits/:id` | `{name?, description?, type?, currency?}` | 200 `Split` |
| DELETE | `/api/splits/:id` | — | 200 `{"message":"Archived"}` |

- `GET /api/splits` excludes archived splits and orders by `created_at DESC`.
  Pass `?archived=true` to include them.
- `SplitDetail` is a `Split` plus an embedded `"members": [Member]` array.
- `DELETE` **archives** (sets `archived_at = now()`), it does not delete rows.
  Archiving an already-archived split is a no-op that still returns 200.
- `POST` automatically adds the owner as the first member, named after their
  `display_name`, falling back to the part of their email before the `@`.
- `currency` defaults to `"USD"` when omitted.

### Validation

| Field | Rule |
|---|---|
| `name` | required, trimmed, 1–200 chars |
| `description` | optional, ≤ 2000 chars |
| `type` | required on create, one of `one_time` \| `ongoing` |
| `currency` | optional, exactly 3 ASCII letters (ISO 4217), normalized to uppercase |

`PUT` accepts any subset of the fields; omitted fields are left unchanged. An
empty body is a 400, not a silent no-op.

---

## Phase 2 — Members

```jsonc
// Member
{
  "id":        "uuid",
  "split_id":  "uuid",
  "user_id":   null,          // uuid when the member is a registered user
  "name":      "Alice",
  "email":     null,          // string when provided
  "joined_at": "2026-08-30 22:51:45.947108+00"
}
```

| Method | Path | Body | Response |
|---|---|---|---|
| GET | `/api/splits/:id/members` | — | 200 `[Member]` |
| POST | `/api/splits/:id/members` | `{name, email?}` | 201 `Member` |
| DELETE | `/api/splits/:id/members/:mid` | — | 200 `{"message":"Removed"}` |

- Ordered by `joined_at ASC`.
- A member is a plain name by default. `email` is stored for future invites but
  **no email is sent** — there is no mail service configured. The `invite`
  endpoint in plan.md is deferred until one exists.
- `name` required, trimmed, 1–100 chars. Duplicate names within a split are
  allowed (two people really can both be "Alex"); the id is the identity.
- `email`, when present, must contain `@` and be ≤ 320 chars.
- Deleting a member that does not belong to `:id` is a 404.
- Removing the last remaining member is allowed; a split with no members is a
  valid, if useless, state.

---

## Phase 3 — Bills

### Money representation

Every monetary value crosses the wire as a **string**, never a JSON number:
`"12.5000"`, not `12.5`. Postgres stores `NUMERIC(12,4)`; JSON numbers are IEEE
doubles, and `0.1 + 0.2` is the canonical reason not to let a float decide what
someone owes.

Rules that follow from this, and that both sides must honour:

- The client parses these strings **for display only**.
- **Any total shown to a user comes from the server.** The client never sums
  line items, tax and tip itself and renders the result. If a total is needed,
  the server computes it and sends it.
- Amounts are sent by the client as strings too. `"12.50"` and `"12.5"` are both
  accepted; the server normalizes to 4 decimal places.
- Negative values are rejected everywhere in Phase 3. Discounts are a later
  feature and will need their own design.
- `""` is **rejected** for an amount but **accepted** for `currency` and
  `payer_member_id`. That asymmetry is deliberate: those two fields have a
  meaningful "unset" (inherit the split's currency; nobody fronted the money),
  which is exactly what an unselected `<select>` submits. An empty *amount* has
  no such meaning — silently reading it as zero would turn a cleared field or a
  typo into a real number on someone's bill. Clients should send `"0"`.

### Derived fields

`subtotal` is **derived on every read**, never stored: it is always
`SUM(price * quantity)` over the bill's items, computed by the same query that
returns the bill. A client that sends `subtotal` gets a 400.

There is deliberately no `bills.subtotal` column — it was dropped in
`migrations/003_drop_stored_bill_subtotal.sql`. A stored copy can only ever
agree with the items or silently disagree with them, and a disagreement would
make Phase 4's proportional tax/tip split wrong in a way that surfaces as a few
unexplained cents in someone's share. Deriving on read makes the invariant
structural instead of something every writer has to remember — which matters
most in Phase 5, where receipt parsing bulk-inserts items. `tax`, `tip` and `fees` are user-entered; `total` is
`subtotal + tax + tip + fees`, computed by the server.

`price` is the **unit** price. A line's contribution is `price * quantity`.

```jsonc
// Bill
{
  "id":              "uuid",
  "split_id":        "uuid",
  "store_name":      "Safeway",
  "date":            "2026-08-30",     // ISO date, no time
  "currency":        "USD",
  "subtotal":        "42.5000",        // derived, read-only
  "tax":             "3.8300",
  "tip":             "0.0000",
  "fees":            "0.0000",
  "total":           "46.3300",        // derived, read-only
  "payer_member_id": null,             // uuid of the member who fronted the money
  "item_count":      3,                // derived, on list and detail
  "created_at":      "2026-08-30 23:46:44.364639+00"
}

// BillItem
{
  "id":       "uuid",
  "bill_id":  "uuid",
  "name":     "Olive oil",
  "price":    "12.5000",   // unit price
  "quantity": 2,
  "currency": "USD",
  "line_total": "25.0000"  // derived: price * quantity
}
```

| Method | Path | Body | Response |
|---|---|---|---|
| GET | `/api/splits/:id/bills` | — | 200 `[Bill]` |
| POST | `/api/splits/:id/bills` | `{store_name, date, currency?, tax?, tip?, fees?, payer_member_id?}` | 201 `Bill` |
| GET | `/api/splits/:id/bills/:bid` | — | 200 `BillDetail` |
| PUT | `/api/splits/:id/bills/:bid` | any subset of the POST fields | 200 `Bill` |
| DELETE | `/api/splits/:id/bills/:bid` | — | 200 `{"message":"Deleted"}` |

- `BillDetail` is a `Bill` plus an embedded `"items": [BillItem]` array.
- Bills list orders by `date DESC, created_at DESC`.
- `DELETE` really deletes (unlike splits, which archive). Items cascade.
- `payer_member_id`, when set, **must be a member of that same split** — a member
  id from another split is a 400, not a 404, because the split itself was found.

### Validation

| Field | Rule |
|---|---|
| `store_name` | required, trimmed, 1–200 chars |
| `date` | required, `YYYY-MM-DD` |
| `currency` | optional, 3 ASCII letters, uppercased; defaults to the split's currency |
| `tax`, `tip`, `fees` | optional, ≥ 0, ≤ 99999999.9999; default `"0"`. An empty string is **rejected**, not coerced to zero — see below |
| `payer_member_id` | optional; a uuid belonging to this split, or `null`/`""` for "no payer" |
| `subtotal`, `total` | rejected with 400 if supplied — they are derived |

---

## Phase 3 — Bill items

Items hang off `/api/bills/:bid/...`, **not** under `/api/splits/:id/`, matching
plan.md and keeping the Phase 4 allocation paths from nesting five levels deep.

**This makes authorization the crux of the slice.** The URL contains no split
and no owner, so ownership must be established by joining
`bill_items → bills → splits` and checking `splits.owner_id`. Every one of these
endpoints must do it. A bill id belonging to another user's split must behave
exactly as if it did not exist.

| Method | Path | Body | Response |
|---|---|---|---|
| GET | `/api/bills/:bid/items` | — | 200 `[BillItem]` |
| POST | `/api/bills/:bid/items` | `{name, price, quantity?, currency?}` | 201 `BillItem` |
| PUT | `/api/bills/:bid/items/:iid` | any subset of the POST fields | 200 `BillItem` |
| DELETE | `/api/bills/:bid/items/:iid` | — | 200 `{"message":"Deleted"}` |

- Ordered by insertion: `ORDER BY created_at, id`. The `created_at` column was
  added in `migrations/002_bill_items_created_at.sql`; `id` is a random UUID and
  is not an ordering.
- Every mutation must recompute the parent bill's `subtotal` **in the same
  transaction**. A response that reports a new item while the bill's subtotal
  still reflects the old set is a bug.
- An `:iid` that exists but belongs to a different bill is a 404.

### Validation

| Field | Rule |
|---|---|
| `name` | required, trimmed, 1–200 chars |
| `price` | required, numeric string, ≥ 0, ≤ 99999999.9999, max 4 decimal places |
| `quantity` | optional int, ≥ 1, ≤ 100000; defaults to 1 |
| `currency` | optional, 3 ASCII letters; defaults to the bill's currency |

---

## Phase 4 — Allocations

Who owes what for each line item. This is the part of the system where a
rounding decision made carelessly becomes a few cents that nobody can account
for, so the rules below are exact and the server is the only thing allowed to
compute a share.

### What an allocation divides

An allocation splits a line's **`line_total`** (`price * quantity`), not the
unit `price`. Two people sharing a $12.50 item bought twice are splitting
$25.00.

### Modes

Each allocation row carries `allocation_mode`, and **every row for one item must
use the same mode** — a mixed set is a 400. The mode is effectively a property
of the item; it lives on the row because that is how the table was built.

| Mode | Field | Meaning |
|---|---|---|
| `ratio` | `ratio` | percent of the line, e.g. `"33.3333"` |
| `amount` | `amount` | a fixed sum, e.g. `"5.0000"` |

### The rounding rule — read this before writing any arithmetic

A member's share in ratio mode is `round(line_total * ratio / 100, 4)`,
**computed by Postgres in NUMERIC**, never in C++ `double` or JavaScript.

Rounded shares need not sum to `line_total`, and the server does not force them
to. Three people splitting $10.00 evenly by ratio each get `"3.3333"`, totalling
`"9.9999"`, and the remaining `"0.0001"` is reported as `unallocated`. It is
**not** silently handed to whoever sorts first. Under-allocation is a legitimate
state that the UI surfaces; a hidden cent is a bug someone finds three months
later.

### Over-allocation is blocked, under-allocation is allowed

- Ratio mode: `SUM(ratio) <= 100.0000`. Above that → 400.
- Amount mode: `SUM(amount) <= line_total`. Above that → 400.
- Below either bound is fine; the difference comes back as `unallocated`.

```jsonc
// Allocation
{
  "id":              "uuid",
  "bill_item_id":    "uuid",
  "member_id":       "uuid",
  "member_name":     "Alice",        // joined, so the UI never shows a bare uuid
  "allocation_mode": "ratio",
  "ratio":           "33.3333",      // string; null in amount mode
  "amount":          null,           // string in amount mode; null in ratio mode
  "share":           "3.3333"        // derived: what this member actually owes
}

// AllocationSet — what GET and PUT both return
{
  "bill_item_id": "uuid",
  "line_total":   "10.0000",
  "mode":         "ratio",           // null when there are no allocations
  "allocated":    "9.9999",          // SUM(share)
  "unallocated":  "0.0001",          // line_total - allocated, never negative
  "allocations":  [ /* Allocation */ ]
}
```

| Method | Path | Body | Response |
|---|---|---|---|
| GET | `/api/bills/:bid/items/:iid/allocations` | — | 200 `AllocationSet` |
| PUT | `/api/bills/:bid/items/:iid/allocations` | `{mode, allocations:[{member_id, ratio?\|amount?}]}` | 200 `AllocationSet` |
| POST | `/api/bills/:bid/items/:iid/even-split` | `{member_ids:[uuid]}` | 200 `AllocationSet` |

- **PUT is a full replace**, in one transaction: the item's existing rows are
  deleted and the supplied set inserted. Sending `{"mode":"ratio","allocations":[]}`
  clears the item. A partial update would make "remove Bob" indistinguishable
  from "leave Bob alone".
- Every `member_id` must belong to **the same split as the bill**. One that does
  not is a 400 (the item was found; the body is wrong). Duplicated `member_id`
  in one request is a 400.
- Ownership, as everywhere: the bill is reached by joining
  `bill_items → bills → splits` and checking `splits.owner_id`. Another user's
  item is a 404.

### Even split

`POST .../even-split` with the members to include. It produces **`amount`** mode,
with each share floored to **2 decimal places** and the remainder left
unallocated:

```
$10.00 across 3 members → 3.33, 3.33, 3.33, unallocated 0.01
```

Floored to cents rather than to the stored 4 decimal places because an even
split exists to produce a number someone can actually pay. A 4-decimal floor
would leave `"3.3333"` each and a `"0.0001"` remainder that displays as `0.00` —
technically precise, and useless. Manually entered amounts may still carry 4
decimals; this rule is specific to the convenience action.

An empty `member_ids` is a 400. A member id not in the split is a 400. A line
whose share floors to zero (e.g. `$0.01` across 3 members) is a 400 — writing
`0.00` rows would contradict the rule that zero allocations are rejected.

### Validation

| Field | Rule |
|---|---|
| `mode` | required, `ratio` \| `amount` |
| `ratio` | required in ratio mode, `> 0`, `<= 100`, max 4 decimals |
| `amount` | required in amount mode, `> 0`, max 4 decimals |
| `member_id` | required, uuid, a member of the bill's split, no duplicates |

A zero or negative share is rejected: "allocate nothing to Bob" is expressed by
leaving Bob out, and having two ways to say it invites them to disagree.

---

## Phase 4 — Per-bill shares

What each member owes on one bill, including their proportional part of tax,
tip and fees.

| Method | Path | Response |
|---|---|---|
| GET | `/api/splits/:id/bills/:bid/shares` | 200 `BillShares` |

```jsonc
// BillShares
{
  "bill_id":   "uuid",
  "currency":  "USD",
  "subtotal":  "61.4900",
  "tax":       "3.8300",
  "tip":       "0.0000",
  "fees":      "0.0000",
  "total":     "65.3200",
  "allocated_subtotal": "51.4900",   // SUM of every allocated share
  "unallocated_subtotal": "10.0000", // items nobody has been assigned
  "payer_member_id": null,
  "members": [
    {
      "member_id": "uuid",
      "name":      "Alice",
      "items":     "25.0000",   // their share of line items
      "tax":       "1.5580",    // proportional
      "tip":       "0.0000",
      "fees":      "0.0000",
      "total":     "26.5580",
      "owes_payer": "26.5580"   // 0 for the payer; equals total when a payer is set
    }
  ],
  "unallocated": {              // the part of the bill nobody is on the hook for
    "items": "10.0000", "tax": "0.6230", "tip": "0.0000", "fees": "0.0000",
    "total": "10.6230"
  }
}
```

### Proportional tax, tip and fees

A member's share of each is `their_items / subtotal * amount`, rounded to 4
decimals in NUMERIC.

The denominator is the bill's **full** `subtotal`, not the allocated portion.
This matters: if only half the items are assigned to anybody, only half the tax
is distributed, and the rest lands in `unallocated`. Dividing by the allocated
subtotal instead would silently make two people cover tax on a third person's
unassigned lunch.

`unallocated.tax` (and tip, fees) is the **proportional share of the unassigned
items**, computed the same way a member's is — not the leftover
`tax - SUM(members.tax)`. The leftover definition would make the columns tie
exactly and always, which would hide the rounding residual described above.

**When `subtotal` is `"0.0000"`, every proportional share is `"0.0000"` and the
whole of tax/tip/fees is `unallocated`.** This is the one place the symmetry
above is deliberately broken: proportional arithmetic on a zero subtotal would
put tax in no bucket at all, so the unallocated fallback is the full amount
while each member's is zero. There is no meaningful proportion of
nothing, and this is the division-by-zero that must not reach Postgres.

Because of rounding and under-allocation, `SUM(members[].total) + unallocated.total`
equals `total` only up to the residual the rounding rule already describes. The
response reports what is true rather than forcing the columns to tie.

### Multi-currency

Deferred to Phase 7. Every amount in this response is in the bill's own
`currency`; there is no conversion, and a split whose bills use different
currencies will produce shares that must not be added together. The FX fetcher
and normalization land with the rest of Phase 7.

---

## Not yet implemented

The split-wide summary, payments, share links, export, and currency endpoints
are specified in [plan.md](../plan.md) and are not built yet.
