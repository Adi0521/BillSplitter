# BillSplitter

Split bills item by item, with per-person ratio or dollar allocations, multi-currency
support, and payer/payment tracking.

- **Backend** — C++17 REST API (Crow 1.3.3, libpqxx, nlohmann/json) on port 8080
- **Frontend** — Vue 3 SPA (Vite, Pinia, Tailwind) on port 5173
- **Database** — PostgreSQL 16

See [plan.md](plan.md) for the full design and phase breakdown.

## Status

Phases 1 and 2 are complete and verified end to end.

- **Phase 1** — register, login, logout, session cookie, `Authorization: Bearer`,
  and `GET /api/auth/me`.
- **Phase 2** — splits CRUD, member add/remove, and the three views for them.
  Member *invite* is deferred: it sends email and no mail service is configured.

- **Phase 3** — bills CRUD, line-item CRUD, and the views for them. Money is
  handled as strings end to end and every total is computed server-side; see
  the "Money representation" section of the contract before touching an amount.

- **Phase 4** — per-item allocations (ratio or fixed amount), even split, and a
  per-bill "who owes what" breakdown with proportional tax/tip/fees. Read the
  "Phase 4 — Allocations" section of the contract before changing any of this
  arithmetic: over-allocation is blocked, under-allocation is legitimate, and
  the rounding remainder is reported rather than absorbed into someone's share.

Phases 5–8 — receipt parsing, the split-wide summary, payments, sharing, export,
and multi-currency — are not yet implemented.

The full request/response contract lives in [docs/api.md](docs/api.md).

## Prerequisites

macOS with Homebrew:

```bash
brew install cmake libpqxx openssl@3 libpq asio
brew install --cask docker    # or have Postgres 16 available some other way
```

CMake ≥ 3.20 and a C++17 compiler. Node 18+ for the frontend.

## Setup

```bash
# 1. Database (host port 5433, so it does not collide with a Postgres on 5432)
docker compose up -d
./scripts/migrate.sh

# 2. Backend
cp backend/.env.example backend/.env      # already points at the local database
cd backend && cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j8
set -a && . .env && set +a && ./build/billsplitter-backend

# 3. Frontend (separate shell)
cd frontend && npm install && npm run dev
```

Then open http://localhost:5173.

## Tests

```bash
./scripts/run-api-tests.sh            # the whole suite
./scripts/run-api-tests.sh test_shares  # one module
ctest --test-dir backend/build        # same suite, via CTest
```

242 integration tests drive the real HTTP API against a throwaway
`billsplitter_test` database, rebuilt and migrated from zero on every run. They
are deliberately not unit tests: every validation helper lives in an anonymous
namespace inside its `.cpp`, and the logic worth testing is in NUMERIC
expressions, join-based authorization and handler validation — none of which a
linked unit test reaches. Standard library only, so CI needs nothing but Python.

Tests assert the **contract** in [docs/api.md](docs/api.md), not current
behaviour. A test that fails because the code disagrees with the contract is
doing its job; fix the code, or change the contract deliberately.

## Migrations

`scripts/migrate.sh` is the only thing that applies schema, to local and hosted
databases alike. Each file in `migrations/` is applied once, inside a
transaction, and recorded in a `schema_migrations` ledger.

```bash
./scripts/migrate.sh              # apply pending migrations
./scripts/migrate.sh --status     # show applied / pending, change nothing
```

Add a migration by creating `migrations/00N_description.sql`. Do not edit a
migration that has already been applied — the ledger will skip it and the
database will silently diverge from the file. Write a new one instead.

There is deliberately no `docker-entrypoint-initdb.d` mount: it only fires on an
empty volume and never for a hosted database, which is exactly how a database
drifts from the migration that claims to have created it.

## Using Supabase instead of local Postgres

Put the Transaction Pooler URI in `backend/.env` as `DATABASE_URL`, then run
`./scripts/migrate.sh` to create the schema there.

## Configuration

`backend/.env` (gitignored; template in `backend/.env.example`):

| Variable | Required | Default | Purpose |
|---|---|---|---|
| `DATABASE_URL` | yes | — | PostgreSQL connection string |
| `APP_BASE_URL` | no | `http://localhost:5173` | Allowed CORS origin |
| `PORT` | no | `8080` | Backend listen port |
| `LOGIN_MAX_FAILURES` | no | `10` | Failed sign-ins per email+IP before a 429 |
| `LOGIN_THROTTLE_WINDOW_SECONDS` | no | `900` | Window those failures are counted over |

Only *failed* sign-ins count toward the throttle and a success clears the
history, so ordinary use — including a test suite that logs in constantly — is
never throttled. The counter is in-process, so behind several instances the
effective limit multiplies by the instance count.

Expired sessions are swept hourly by a background thread; nothing else deletes
them.

The backend starts even when the database is unreachable and logs a warning;
`/api/health` still responds, and connections are opened lazily per request.

## API

Implemented today:

| Method | Path | Description |
|---|---|---|
| GET | `/api/health` | Liveness check |
| POST | `/api/auth/register` | Create account; sets session cookie |
| POST | `/api/auth/login` | Log in; sets session cookie |
| POST | `/api/auth/logout` | Invalidate session |
| GET | `/api/auth/me` | Current user |
| GET | `/api/splits` | List splits (`?archived=true` to include archived) |
| POST | `/api/splits` | Create a split; adds the owner as first member |
| GET | `/api/splits/:id` | Split detail with embedded members |
| PUT | `/api/splits/:id` | Partial update |
| DELETE | `/api/splits/:id` | Archive (does not delete) |
| GET | `/api/splits/:id/members` | List members |
| POST | `/api/splits/:id/members` | Add a member |
| DELETE | `/api/splits/:id/members/:mid` | Remove a member |
| GET | `/api/splits/:id/bills` | List bills |
| POST | `/api/splits/:id/bills` | Create a bill |
| GET | `/api/splits/:id/bills/:bid` | Bill detail with embedded items |
| PUT | `/api/splits/:id/bills/:bid` | Partial update |
| DELETE | `/api/splits/:id/bills/:bid` | Delete a bill (items cascade) |
| GET | `/api/bills/:bid/items` | List line items |
| POST | `/api/bills/:bid/items` | Add a line item |
| PUT | `/api/bills/:bid/items/:iid` | Update a line item |
| DELETE | `/api/bills/:bid/items/:iid` | Delete a line item |
| GET | `/api/bills/:bid/items/:iid/allocations` | Who is on this line item |
| PUT | `/api/bills/:bid/items/:iid/allocations` | Replace an item's allocations |
| POST | `/api/bills/:bid/items/:iid/even-split` | Split a line evenly (floors to cents) |
| GET | `/api/splits/:id/bills/:bid/shares` | Per-member breakdown for one bill |

Sessions are opaque 32-byte tokens in the `sessions` table, sent as an HttpOnly
`session` cookie or an `Authorization: Bearer` header. Passwords are PBKDF2-
SHA256, 100k iterations, with a per-user salt.

Errors are always `{"error":"..."}` with an appropriate status code.

## Repository layout

```
backend/           C++ API — include/ headers, src/ implementation
  include/app.h    BsApp, the shared Crow app type (App<CORSHandler>)
  include/db/      Connection pool
  include/auth/    Password hashing, sessions, require_auth middleware
  src/routes/      One module per resource: register_<name>_routes(BsApp&, DbPool&)
frontend/src/      Vue SPA — views/, components/, stores/, router/
migrations/        Versioned SQL, applied by scripts/migrate.sh
splititems.{h,cpp} Pre-rewrite prototype, not built; see plan.md for the migration
```

Adding a backend route module needs no CMake change — `CMakeLists.txt` globs
`src/*.cpp`. It does need a `register_*_routes(app, pool)` call in `main.cpp`.
