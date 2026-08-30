# BillSplitter

Split bills item by item, with per-person ratio or dollar allocations, multi-currency
support, and payer/payment tracking.

- **Backend** — C++17 REST API (Crow 1.3.3, libpqxx, nlohmann/json) on port 8080
- **Frontend** — Vue 3 SPA (Vite, Pinia, Tailwind) on port 5173
- **Database** — PostgreSQL 16

See [plan.md](plan.md) for the full design and phase breakdown.

## Status

Phase 1 (auth foundation) is complete and verified end to end: register, login,
logout, session cookie, `Authorization: Bearer`, and `GET /api/auth/me`.

Phases 2–8 — splits, members, bills, the allocation engine, receipt parsing,
summaries, payments, sharing, export, and currency — are not yet implemented.

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
