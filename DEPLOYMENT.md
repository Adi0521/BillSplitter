# Deploying BillSplitter

Two pieces, and they must go to different kinds of host:

| Piece | What it is | Where it goes |
|---|---|---|
| `frontend/` | A Vite-built static SPA | Vercel, Netlify, Cloudflare Pages, any static host |
| `backend/` | A long-running C++ process | A **container** host — Fly.io, Railway, Render, a VPS |
| database | PostgreSQL 13+ | Managed Postgres, ideally beside the backend |

**The backend cannot run on Vercel.** It is not a serverless function: it keeps a
pool of database connections, an hourly session-sweeper thread, an in-process
login throttle, and a pool of initialized Tesseract OCR engines. It also links
against libpqxx, OpenSSL, libcurl, tesseract and leptonica. Vercel has no C++
runtime and no persistent process to hold any of that.

## You do not need Supabase

Nothing in this codebase uses a Supabase feature — no Auth, no RLS, no Realtime,
no Storage, no client library. The data layer is plain libpqxx over TCP, and the
schema uses only stock PostgreSQL (`pgcrypto`, CTEs, `LATERAL`, `NUMERIC`).
Supabase works fine, but so does any Postgres, and switching later is a new
`DATABASE_URL` plus `./scripts/migrate.sh`.

Prefer **managed Postgres from the same provider as the backend**. The summary
and shares endpoints issue several queries per request over long-lived
connections, so a database at a different provider pays a cross-network round
trip on every one of them.

Whichever you pick, confirm it allows `CREATE EXTENSION pgcrypto` — migration
001 needs `gen_random_uuid()` and `gen_random_bytes()`.

## libpqxx >= 7.10 is required

This code calls the two-argument `txn.exec(sql, pqxx::params{...})`, which
arrived in libpqxx **7.10**. The floor is not "7":

| Distribution | libpqxx | Builds? |
|---|---|---|
| Debian bookworm | 6.4 | No — `pqxx::params` does not exist |
| Ubuntu 24.04 | 7.8 | No — has `pqxx::params`, but not `exec(sql, params)` |
| Debian trixie | 7.10 | Yes |
| Homebrew (macOS) | 7.10+ | Yes |

Both the Dockerfile and the CI backend job run on `debian:trixie-slim` for this
reason, so CI builds the same toolchain that ships. **Check the libpqxx version
before changing either base image** — the 7.8 failure in particular is easy to
misread, because `pqxx::params` resolves fine and only the overload is missing.

## Backend

Build from the **repository root**, not from `backend/`:

```bash
docker build -f backend/Dockerfile -t billsplitter-backend .
docker run -p 8080:8080 \
  -e DATABASE_URL="postgresql://user:pass@host:5432/db?sslmode=require" \
  -e APP_BASE_URL="https://your-frontend.example.com" \
  billsplitter-backend
```

The image is ~139 MB, runs as an unprivileged `billsplitter` user, and carries a
`HEALTHCHECK` on `/api/health`. That endpoint touches no database on purpose:
it reports "this process is serving", so a database outage does not cause your
platform to cycle an otherwise healthy container.

Run migrations once per environment, from anywhere with `psql` and network
access to the database:

```bash
DATABASE_URL="postgresql://..." ./scripts/migrate.sh
```

### Environment

| Variable | Required | Default | Notes |
|---|---|---|---|
| `DATABASE_URL` | yes | — | Add `?sslmode=require` for any hosted database |
| `APP_BASE_URL` | yes in production | `http://localhost:5173` | The frontend's origin. Drives CORS **and** whether the session cookie is `Secure` |
| `PORT` | no | `8080` | Most platforms inject this |
| `SESSION_COOKIE_SAMESITE` | no | `Lax` | Set `None` **only** for a cross-origin frontend |
| `SESSION_COOKIE_SECURE` | no | `auto` | `auto` = on when `APP_BASE_URL` is https |
| `LOGIN_MAX_FAILURES` | no | `10` | Failed sign-ins per email+IP before a 429 |
| `LOGIN_THROTTLE_WINDOW_SECONDS` | no | `900` | Window those failures are counted over |
| `BILLSPLITTER_OCR_POOL` | no | `4` | Tesseract engines held in memory |
| `BILLSPLITTER_TESSDATA` | no | set in image | Language data path |

## Frontend, and the one decision that matters

The SPA calls the API at `VITE_API_BASE_URL`, defaulting to the **relative**
path `/api`. That default is not an accident, and there are two ways to deploy:

### Recommended: same-origin, via a rewrite

Have the frontend host proxy `/api/*` to the backend. On Vercel, `vercel.json`:

```json
{
  "rewrites": [
    { "source": "/api/:path*", "destination": "https://your-backend.fly.dev/api/:path*" }
  ]
}
```

Leave `VITE_API_BASE_URL` unset. The browser sees one origin, so:

- the session cookie works under `SameSite=Lax`, the safer setting
- there is no CORS involved at all
- no backend configuration is needed beyond `APP_BASE_URL`

### The alternative: cross-origin

Only if the API really must live on another domain. **Three things must change
together, and getting one wrong fails silently** — login appears to succeed and
every request afterwards looks logged out, because the browser quietly declines
to send the cookie:

1. Frontend build: `VITE_API_BASE_URL=https://your-backend.fly.dev/api`
2. Backend: `SESSION_COOKIE_SAMESITE=None` (this forces `Secure`, so HTTPS only)
3. Backend: `APP_BASE_URL` set to the frontend's exact origin — CORS with
   credentials is never honoured against a wildcard

`VITE_*` variables are inlined at **build** time, so changing this needs a
rebuild, and nothing secret may go in one: it ships in the bundle.

## Put a reverse proxy in front

Crow buffers an entire request body before dispatching it and exposes no
application-level size cap. The receipt upload endpoint rejects anything over
10 MB from `Content-Length` before doing any work, but the bytes are already in
memory by then. Cap it upstream:

```nginx
client_max_body_size 10m;
```

Most PaaS routers do this by default; confirm yours does. See the README section
"Known limitation: request body size".

## Checklist

- [ ] Database created; `./scripts/migrate.sh` run against it; `pgcrypto` allowed
- [ ] Backend deployed with `DATABASE_URL` (+ `sslmode=require`) and `APP_BASE_URL`
- [ ] `/api/health` returns `{"status":"ok"}` over HTTPS
- [ ] Frontend deployed with the `/api/*` rewrite (or the three cross-origin settings)
- [ ] Register, log in, reload the page — still logged in. This is the check that
      catches a misconfigured cookie, and nothing else will
- [ ] Upload a receipt — confirms OCR language data resolved inside the container
- [ ] Request body limit enforced by the proxy
