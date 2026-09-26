# Deploying BillSplitter

Two pieces, and they must go to different kinds of host:

| Piece | What it is | Where it goes |
|---|---|---|
| `frontend/` | A Vite-built static SPA | Vercel, Netlify, Cloudflare Pages, any static host |
| `backend/` | A long-running C++ process | A **container** host — this repo is set up for **Render** (`render.yaml`) |
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

## Backend on Render

[render.yaml](render.yaml) declares the web service. The database is Supabase
(any Postgres works; see below), so `DATABASE_URL` is entered by hand:

1. Push the repo to GitHub (Render deploys from it).
2. Render dashboard → **New → Blueprint** → select the repo. It reads
   `render.yaml` and creates `billsplitter-backend`.
3. It will prompt for the two values it cannot infer:
   - **`DATABASE_URL`** — Supabase → Project Settings → Database → Connection
     string → **Transaction pooler** (port 6543), with your password filled in
     and `?sslmode=require` appended. **Not** the direct connection: on the free
     tier that is IPv6-only and Render does not guarantee IPv6 egress. And not
     the `https://<ref>.supabase.co` URL — that is Supabase's REST API, which
     this backend never uses.
   - **`APP_BASE_URL`** — your Vercel URL (`https://...`). Guess it now and
     correct it after the frontend is up; it only needs to be https so the
     cookie is `Secure`.
4. Wait for the first build. It compiles C++ with FetchContent, so expect
   several minutes; later builds cache the dependency clones.
5. Run migrations once. From the service's **Shell** tab, or from your machine
   using the database's *external* connection string from the dashboard:
   ```bash
   DATABASE_URL="postgresql://...?sslmode=require" ./scripts/migrate.sh
   ```
6. Confirm `https://<your-service>.onrender.com/api/health` returns
   `{"status":"ok"}`.

### What the free tier means for this app

- **Spin-down.** A free service stops after ~15 minutes idle and cold-starts on
  the next request, which takes tens of seconds. This app builds its database
  pool and OCR engines lazily, so a cold start is one slow first request, not
  an error — but the login throttle and the hourly session sweep restart with
  the process. Fine for personal use; not for something people rely on.
- **512 MB.** `render.yaml` sets `BILLSPLITTER_OCR_POOL=1`. Each Tesseract
  engine holds a language model; four would not fit. Two receipts uploaded at
  the same instant queue behind one engine rather than running in parallel.
- **Supabase free tier pauses inactive projects** after a period without
  activity (check the current policy); the dashboard shows a resume button.
  The backend's lazy connection pool copes — a paused database surfaces as a
  failed request, not a crashed process — but it is worth knowing.
- Render's proxy caps request bodies well above our 10 MB, so the
  "request body size" limitation in the README still applies.

The Docker `HEALTHCHECK` is ignored by Render; `healthCheckPath: /api/health`
in the Blueprint is what it uses. Render injects `PORT=10000`, which the app
reads — the Dockerfile's `PORT=8080` is only a default.

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

Have the frontend host proxy `/api/*` to the backend. On Vercel, this lives in
**`frontend/vercel.json`** — it must be inside the project's Root Directory, not
at the repo root. With Root Directory set to `frontend`, Vercel reads config
from there and silently ignores a `vercel.json` one level up. The symptom is
that the site deploys fine but `/api/*` returns Vercel's own NOT_FOUND page and
deep links 404 instead of falling back to the SPA.

```json
{
  "rewrites": [
    { "source": "/api/:path*", "destination": "https://billsplitter-backend.onrender.com/api/:path*" }
  ]
}
```
That file already exists at `frontend/vercel.json` with an SPA fallback rule as
well — just replace the host with the one Render assigned.

Two things about it that cannot be written inside the file itself, because
Vercel validates `vercel.json` against a strict schema and rejects any unknown
key — so no comments, not even `//` keys:

- **Root Directory must be set to `frontend`** in the Vercel project settings.
  That is a dashboard setting, not something `vercel.json` can express.
- The `/(.*)` → `/index.html` rule is the SPA fallback. Static files are matched
  before rewrites, so it does not shadow the built assets; it only catches
  client-side routes like `/splits/<id>/summary` on a hard refresh.

Leave `VITE_API_BASE_URL` unset. The browser sees one origin, so:

- the session cookie works under `SameSite=Lax`, the safer setting
- there is no CORS involved at all
- no backend configuration is needed beyond `APP_BASE_URL`

### The alternative: cross-origin

Only if the API really must live on another domain. **Three things must change
together, and getting one wrong fails silently** — login appears to succeed and
every request afterwards looks logged out, because the browser quietly declines
to send the cookie:

1. Frontend build: `VITE_API_BASE_URL=https://<your-service>.onrender.com/api`
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

## Deploying a change that includes a migration

**Run the migration against the production database before the new backend
goes live.** The code and the schema ship separately, and the code assumes the
schema. Migration 005, for example, defines the `split_role()` function every
route now calls — a backend deployed ahead of it fails on every authenticated
request.

```bash
DATABASE_URL="<pooler connection string>?sslmode=require" ./scripts/migrate.sh
```

The ledger applies only what is pending, so this is always safe to run.

## Checklist

- [ ] Database created; `./scripts/migrate.sh` run against it; `pgcrypto` allowed
- [ ] Backend deployed with `DATABASE_URL` (+ `sslmode=require`) and `APP_BASE_URL`
- [ ] `/api/health` returns `{"status":"ok"}` over HTTPS
- [ ] Frontend deployed with the `/api/*` rewrite (or the three cross-origin settings)
- [ ] Register, log in, reload the page — still logged in. This is the check that
      catches a misconfigured cookie, and nothing else will
- [ ] Upload a receipt — confirms OCR language data resolved inside the container
- [ ] Request body limit enforced by the proxy
