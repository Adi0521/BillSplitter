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

## Not yet implemented

Bills, items, allocations, summary, payments, share, export, and currency
endpoints are specified in [plan.md](../plan.md) and are not built yet.
