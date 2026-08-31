# BillSplitter — Full-Stack Implementation Plan

## Context

The user wants a bill-splitting web app. Early C++ scaffolding exists (`splititems.cpp`, `splititems.h`) with basic data structures and a PostgreSQL connection via libpqxx. The goal is to build a complete, production-quality app around that foundation: a C++ REST API backend (Crow), a Vue.js SPA frontend, PostgreSQL persistence, email magic-link auth (SendGrid), AI receipt parsing (pluggable), multi-currency support, and per-item ratio/dollar splitting with full payer and payment tracking.

---

## Architecture Overview

```
┌─────────────────────┐      REST/JSON       ┌─────────────────────────┐
│   Vue.js SPA        │ ◄──────────────────► │  C++ Crow REST Backend  │
│  (Vite + Vue 3)     │                      │  (port 8080)            │
└─────────────────────┘                      └────────────┬────────────┘
                                                          │ libpqxx
                                                 ┌────────▼────────┐
                                                 │   PostgreSQL    │
                                                 └─────────────────┘
```

External services (called from C++ backend only):
- **exchangerate.host** (or compatible) — daily FX rate fetch
- **AI Provider (pluggable)** — receipt OCR (Claude, OpenAI, etc.)

---

## Directory Structure

```
BillSplitter/
├── backend/
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── db/           # DB connection pool, migrations
│   │   ├── auth/         # Magic link, session token logic
│   │   ├── routes/       # Route handlers (splits, bills, users, etc.)
│   │   ├── models/       # Structs: User, Split, Bill, BillItem, etc.
│   │   ├── services/
│   │   │   ├── receipt_parser/   # Pluggable OCR interface
│   │   │   ├── email/            # SendGrid adapter
│   │   │   ├── currency/         # FX rate fetcher + cache
│   │   │   └── export/           # PDF/CSV export
│   │   └── splititems.h   # (existing — migrate/refactor into models)
│   └── src/
│       ├── main.cpp
│       ├── db/
│       ├── auth/
│       ├── routes/
│       ├── services/
│       └── splititems.cpp  # (existing — refactor)
├── frontend/
│   ├── package.json
│   ├── vite.config.js
│   └── src/
│       ├── main.js
│       ├── router/
│       ├── stores/         # Pinia stores
│       ├── views/          # Page-level components
│       │   ├── LoginView.vue
│       │   ├── SplitsView.vue       # List of user's splits
│       │   ├── SplitDetailView.vue  # Single split, all bills
│       │   ├── BillView.vue         # Item-level allocation UI
│       │   ├── ShareView.vue        # Public shareable summary
│       │   └── SettingsView.vue
│       └── components/
│           ├── ItemAllocator.vue    # Per-item ratio/dollar toggle
│           ├── ReceiptUploader.vue
│           └── PaymentTracker.vue
├── migrations/
│   └── 001_initial_schema.sql
└── docker-compose.yml    # Postgres local dev
```

---

## Database Schema

### Tables

```sql
-- Users
CREATE TABLE users (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email        TEXT UNIQUE NOT NULL,
    display_name TEXT,
    created_at   TIMESTAMPTZ DEFAULT now()
);

-- Magic link tokens
CREATE TABLE auth_tokens (
    token      TEXT PRIMARY KEY,
    user_id    UUID REFERENCES users(id) ON DELETE CASCADE,
    expires_at TIMESTAMPTZ NOT NULL,
    used       BOOLEAN DEFAULT FALSE
);

-- Session tokens (JWT or opaque)
CREATE TABLE sessions (
    token      TEXT PRIMARY KEY,
    user_id    UUID REFERENCES users(id) ON DELETE CASCADE,
    expires_at TIMESTAMPTZ NOT NULL
);

-- Splits (groups/events)
CREATE TABLE splits (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    owner_id    UUID REFERENCES users(id) ON DELETE CASCADE,
    name        TEXT NOT NULL,
    description TEXT,
    type        TEXT CHECK (type IN ('one_time', 'ongoing')) NOT NULL,
    currency    TEXT DEFAULT 'USD',
    share_token TEXT UNIQUE,          -- for shareable link
    created_at  TIMESTAMPTZ DEFAULT now(),
    archived_at TIMESTAMPTZ
);

-- Members of a split (can be registered users or name-only guests)
CREATE TABLE split_members (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    split_id   UUID REFERENCES splits(id) ON DELETE CASCADE,
    user_id    UUID REFERENCES users(id),   -- NULL if guest
    name       TEXT NOT NULL,
    email      TEXT,                        -- for invite emails
    is_payer   BOOLEAN DEFAULT FALSE,       -- who fronted the bill
    joined_at  TIMESTAMPTZ DEFAULT now()
);

-- Bills within a split
CREATE TABLE bills (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    split_id    UUID REFERENCES splits(id) ON DELETE CASCADE,
    store_name  TEXT NOT NULL,
    date        DATE NOT NULL,
    currency    TEXT DEFAULT 'USD',
    subtotal    NUMERIC(12,4) DEFAULT 0,
    tax         NUMERIC(12,4) DEFAULT 0,
    tip         NUMERIC(12,4) DEFAULT 0,
    fees        NUMERIC(12,4) DEFAULT 0,
    total       NUMERIC(12,4) GENERATED ALWAYS AS (subtotal + tax + tip + fees) STORED,
    payer_member_id UUID REFERENCES split_members(id),
    created_at  TIMESTAMPTZ DEFAULT now()
);

-- Line items on a bill
CREATE TABLE bill_items (
    id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    bill_id  UUID REFERENCES bills(id) ON DELETE CASCADE,
    name     TEXT NOT NULL,
    price    NUMERIC(12,4) NOT NULL,
    quantity INT DEFAULT 1,
    currency TEXT DEFAULT 'USD'
);

-- Per-item allocations (the core split logic)
CREATE TABLE item_allocations (
    id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    bill_item_id   UUID REFERENCES bill_items(id) ON DELETE CASCADE,
    member_id      UUID REFERENCES split_members(id) ON DELETE CASCADE,
    -- Exactly one of ratio or amount must be set:
    ratio          NUMERIC(7,4),    -- e.g. 33.33 (percent)
    amount         NUMERIC(12,4),   -- e.g. 5.00 (dollars)
    allocation_mode TEXT CHECK (allocation_mode IN ('ratio', 'amount')) NOT NULL,
    UNIQUE (bill_item_id, member_id)
);

-- Tax/tip proportional split is computed at query time, not stored

-- Payment records
CREATE TABLE payments (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    split_id    UUID REFERENCES splits(id) ON DELETE CASCADE,
    from_member UUID REFERENCES split_members(id),
    to_member   UUID REFERENCES split_members(id),
    amount      NUMERIC(12,4) NOT NULL,
    currency    TEXT DEFAULT 'USD',
    method      TEXT,     -- 'venmo', 'cash', 'zelle', etc.
    notes       TEXT,
    paid_at     TIMESTAMPTZ DEFAULT now()
);

-- FX rate cache
CREATE TABLE fx_rates (
    base_currency TEXT NOT NULL,
    target_currency TEXT NOT NULL,
    rate          NUMERIC(16,8) NOT NULL,
    fetched_at    TIMESTAMPTZ DEFAULT now(),
    PRIMARY KEY (base_currency, target_currency)
);
```

---

## Backend — C++ with Crow

### Key Dependencies (CMake)
- **Crow** — HTTP framework
- **libpqxx** — PostgreSQL client (already in use)
- **nlohmann/json** — JSON serialization
- **libcurl** — HTTP calls to FX API and AI provider
- **OpenSSL** — password hashing (PBKDF2) + token generation
- **libmagic** or custom — MIME detection for uploads
- **libpoppler or pdfium** — PDF text extraction (for receipt parsing)

### REST API Endpoints

#### Auth
| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/auth/register` | Create account with email + password |
| POST | `/api/auth/login` | Login with email + password, set session cookie |
| POST | `/api/auth/logout` | Invalidate session |
| GET  | `/api/auth/me` | Get current user |

#### Splits
| Method | Path | Description |
|--------|------|-------------|
| GET    | `/api/splits` | List user's splits |
| POST   | `/api/splits` | Create new split |
| GET    | `/api/splits/:id` | Get split detail |
| PUT    | `/api/splits/:id` | Update split |
| DELETE | `/api/splits/:id` | Archive split |
| GET    | `/api/splits/:id/summary` | Full cost summary per member |
| GET    | `/api/splits/share/:token` | Public shareable summary (no auth) |

#### Members
| Method | Path | Description |
|--------|------|-------------|
| GET    | `/api/splits/:id/members` | List members |
| POST   | `/api/splits/:id/members` | Add member (by name or email) |
| DELETE | `/api/splits/:id/members/:mid` | Remove member |
| POST   | `/api/splits/:id/members/:mid/invite` | Send invite email |

#### Bills
| Method | Path | Description |
|--------|------|-------------|
| GET    | `/api/splits/:id/bills` | List bills in split |
| POST   | `/api/splits/:id/bills` | Create bill (manual) |
| GET    | `/api/splits/:id/bills/:bid` | Get bill with items |
| PUT    | `/api/splits/:id/bills/:bid` | Update bill metadata |
| DELETE | `/api/splits/:id/bills/:bid` | Delete bill |
| POST   | `/api/splits/:id/bills/upload` | Upload receipt (HTML/PDF) → AI parse |

#### Items & Allocations
| Method | Path | Description |
|--------|------|-------------|
| POST   | `/api/bills/:bid/items` | Add item to bill |
| PUT    | `/api/bills/:bid/items/:iid` | Update item |
| DELETE | `/api/bills/:bid/items/:iid` | Remove item |
| GET    | `/api/bills/:bid/items/:iid/allocations` | Get allocations for item |
| PUT    | `/api/bills/:bid/items/:iid/allocations` | Set allocations (batch upsert) |
| POST   | `/api/bills/:bid/items/:iid/even-split` | Auto even-split among members |

#### Payments
| Method | Path | Description |
|--------|------|-------------|
| GET    | `/api/splits/:id/payments` | List payments |
| POST   | `/api/splits/:id/payments` | Record a payment |
| DELETE | `/api/splits/:id/payments/:pid` | Delete payment record |

#### Export
| Method | Path | Description |
|--------|------|-------------|
| GET    | `/api/splits/:id/export/pdf` | Export split summary as PDF |
| GET    | `/api/splits/:id/export/csv` | Export as CSV |

#### Currency
| Method | Path | Description |
|--------|------|-------------|
| GET    | `/api/currencies` | List supported currencies |
| GET    | `/api/currencies/rates?base=USD` | Get current rates |

---

## Core Business Logic

### Allocation Validation
- **Ratio mode**: Sum of all member ratios for an item ≤ 100.00%. Saving ≥ 100.01% is blocked (error response). Under-allocation is allowed — UI shows unassigned %.
- **Amount mode**: Sum of member amounts ≤ item price. Over-allocation is blocked. Under-allocation shows remainder.
- **Toggle**: Each item stores `allocation_mode` ('ratio' or 'amount'). Switching modes converts existing allocations.

### Even Split
- Divide item price by N members.
- Round each share down to 4 decimal places.
- Compute remainder = price − (rounded_share × N).
- Remainder is tracked as "unassigned" (shown in UI), NOT assigned to anyone.

### Tax / Tip / Fees (Proportional)
- For each member, their share of tax/tip/fees = (their_item_subtotal / bill_subtotal) × tax_or_tip.
- Computed at query time in the summary endpoint, not stored.

### Net Amounts (Payer Tracking)
- One member per bill is marked `is_payer` (fronted the money).
- Summary endpoint returns: for each non-payer member, `amount_owed_to_payer`.
- If no payer is set, just shows raw shares.

### Multi-Currency
- Each bill stores a `currency`. Each item can override currency.
- FX rates are fetched from `exchangerate.host` once daily and stored in `fx_rates` table.
- All summary computations normalize to the split's base currency using stored rates.
- A background thread in the C++ backend refreshes rates at startup and every 24h.

### Shareable Link
- `splits.share_token` is a random UUID generated on split creation.
- `GET /api/splits/share/:token` returns the full public summary without auth.
- Token can be regenerated (invalidating the old link).

---

## Receipt Parsing (Pluggable AI)

### Interface (C++ abstract class)
```cpp
class ReceiptParser {
public:
    virtual ~ReceiptParser() = default;
    virtual std::vector<ParsedItem> parse(
        const std::string& content,   // raw text or base64 image
        const std::string& mime_type  // "text/html", "application/pdf", "image/png", etc.
    ) = 0;
};

struct ParsedItem {
    std::string name;
    double price;
    int quantity;
};
```

### Concrete implementations
1. **ClaudeReceiptParser** — calls Anthropic API with vision
2. **OpenAIReceiptParser** — calls OpenAI GPT-4V
3. **HtmlReceiptParser** — regex/DOM parsing of Walmart/Amazon/Costco HTML exports (no AI needed, cheaper)

### Upload flow
1. Frontend POSTs multipart form to `/api/splits/:id/bills/upload` with file.
2. Backend detects MIME type.
3. If HTML → `HtmlReceiptParser` (store-specific patterns).
4. If PDF → extract text with pdfium → `ClaudeReceiptParser` (or configured provider).
5. If image → `ClaudeReceiptParser`.
6. Returns JSON list of parsed items → user reviews/edits before confirming.
7. On confirm, bill and items are saved to DB.

---

## Auth Flow (Email + Password)

No email sending required. Simple credential-based auth with server-side sessions.

1. **Register**: `POST /api/auth/register` — `{ email, password, display_name? }` → hashes password with PBKDF2-SHA256 (salt per user), stores in `users`, creates session, returns HttpOnly cookie.
2. **Login**: `POST /api/auth/login` — `{ email, password }` → verifies hash, creates session, returns HttpOnly cookie.
3. **Logout**: `POST /api/auth/logout` — deletes session row, clears cookie.
4. **Me**: `GET /api/auth/me` — validates session cookie, returns user JSON.
5. All subsequent API calls validated by session token middleware (cookie or `Authorization: Bearer` header).

---

## Frontend — Vue 3 + Vite

### Key Libraries
- **Vue Router 4** — client-side routing
- **Pinia** — state management
- **Axios** — HTTP client
- **Tailwind CSS** — styling
- **Vue Draggable** — drag-to-reorder items (optional)
- **jsPDF** — client-side PDF export fallback

### Key Views & User Flows

#### Flow 1: Create Split
1. `/splits/new` → enter name, type (one-time/ongoing), currency
2. Add members by name or email
3. Optionally designate a payer

#### Flow 2: Add Bill
1. `/splits/:id/bills/new` → choose: manual entry OR upload receipt
2. Upload: drag-drop file → preview parsed items → edit/confirm
3. Manual: type store name, date, add line items
4. Set tax, tip, fees (optional)
5. Set currency for the bill

#### Flow 3: Allocate Items (`BillView.vue`)
**Step 1 — Who's splitting this item?**
Checkbox list of members. "Select all" = even split.

**Step 2 — Ratios or Amounts?**
Toggle (per item or global): `[ % Ratio ]  [ $ Amount ]`

**Step 3 — Enter values**
- Even split button: fills equal values automatically, shows remainder if not divisible.
- Manual: input per person. Running total shown. Red if > 100% / base price (blocked). Yellow if under-allocated (shows unassigned amount/%).

#### Flow 4: Summary & Payment Tracking
- `/splits/:id/summary` — table: member | items owed | tax share | tip share | total | paid? | method | notes
- Mark payment: amount, method (Venmo/Cash/Zelle/Other), notes
- If payer designated: shows "Alice is owed $X from Bob, $Y from Carol"

#### Shareable View (`ShareView.vue`)
- No auth required, read-only
- Shows split name, bill list, per-member totals, payment status

---

## Existing Code — Migration Plan

| Existing | Action |
|----------|--------|
| `splititems.h` — `BillItem`, `Bill` structs | Migrate to `backend/include/models/bill.h` — extend with UUID, currency, etc. |
| `splititems.h` — `BillSplitter` class | Break apart: DB queries → `BillRepository`, split logic → `SplitCalculator` |
| `splititems.cpp` — `getUserBills()` | Rewrite for new schema (UUID keys, split scoping) |
| `splititems.cpp` — `loadBill()` | Rewrite |
| `splititems.cpp` — `setPeople()` / `splitItem()` | Replace with `item_allocations` DB-backed logic |
| `splititems.cpp` — `calculateTotals()` | Move to `SplitCalculator` service, extend for tax/tip/FX/payer |

---

## Build System

### CMakeLists.txt (backend)
- Targets: `billsplitter-backend` executable
- Dependencies via `find_package` or `FetchContent`: Crow, libpqxx, nlohmann_json, libcurl, OpenSSL
- Build type: Debug (local dev), Release (prod)

### Frontend
- `npm create vue@latest frontend`
- `vite.config.js` proxies `/api` to `http://localhost:8080`

### Local Dev Setup
```bash
docker-compose up -d   # starts Postgres on 5432
cd backend && cmake -B build && cmake --build build
./build/billsplitter-backend
cd frontend && npm install && npm run dev
```

---

## Implementation Phases

### Phase 1 — Foundation
- [x] Write `migrations/001_initial_schema.sql`
- [x] Set up Crow server with middleware skeleton (`main.cpp`)
- [x] DB connection pool (thread-safe libpqxx wrapper)
- [x] Session middleware (validate session token on each request)
- [x] `POST /api/auth/register` + `POST /api/auth/login` + `POST /api/auth/logout` + `GET /api/auth/me`
- [x] Basic Vue app scaffold (Vite + Vue Router + Pinia + Tailwind)
- [x] Login/Register page (email + password)

### Phase 2 — Splits & Members
- [x] Full CRUD for splits
- [x] Member add/remove
- [ ] Member invite — deferred: sends email, and no mail service is configured
      (Phase 1 dropped SendGrid when auth moved to passwords). Member emails are
      stored, nothing is sent. Revisit alongside Phase 6's "notify all".
- [x] SplitsView.vue + SplitDetailView.vue + NewSplitView.vue

### Phase 3 — Bills & Items
- [x] Manual bill creation + item CRUD
- [x] BillView.vue with item list
- [x] Bills section on SplitDetailView.vue + NewBillView.vue
- Note: `bills.subtotal` is derived on read, not stored — the column was dropped
  in `migrations/003_drop_stored_bill_subtotal.sql`. See docs/api.md.

### Phase 4 — Allocation Engine
- [ ] `ItemAllocator.vue` — member checkboxes, ratio/amount toggle, even split button
- [ ] Allocation validation (block > 100% / base price, allow under, show remainder)
- [ ] `PUT /api/bills/:bid/items/:iid/allocations` bulk upsert
- [ ] `SplitCalculator` service — totals with tax/tip proportional split, FX normalization

### Phase 5 — Receipt Parsing
- [ ] `ReceiptParser` abstract interface + `ClaudeReceiptParser` impl
- [ ] `HtmlReceiptParser` for Walmart/Amazon/Costco HTML
- [ ] PDF text extraction pipeline
- [ ] `ReceiptUploader.vue` drag-drop + preview/edit flow

### Phase 6 — Summary, Payments, Sharing
- [ ] Summary endpoint with net payer amounts
- [ ] `PaymentTracker.vue` — record payments, method/notes
- [ ] Share token generation + `ShareView.vue` (public, no auth)
- [ ] Email "notify all" after finalization

### Phase 7 — Export & Currency
- [ ] PDF export endpoint (server-side)
- [ ] CSV export endpoint
- [ ] FX rate fetcher service + daily refresh background thread
- [ ] Currency selector in split/bill creation

### Phase 8 — Polish
- [ ] CMakeLists.txt finalized
- [ ] Input validation everywhere
- [ ] Error handling (DB failures, SendGrid failures, AI timeouts)
- [ ] Basic responsive mobile layout

---

## Verification

- **Auth**: register with email+password → login → confirm session cookie set → `GET /api/auth/me` returns user
- **Split flow**: create split → add members → add bill → add items → allocate per item → view summary → verify totals = sum(item prices) + proportional tax/tip
- **Even split rounding**: add item at $10, split 3 ways → verify $3.33 + $3.33 + $3.33 + $0.01 unassigned
- **Over-allocation block**: try to assign > 100% ratio → API returns 400, UI shows error
- **Multi-currency**: create bill in EUR, split has USD base → verify amounts converted correctly using stored rate
- **Shareable link**: copy share URL → open in incognito → confirm summary visible without login
- **Receipt upload**: upload Walmart HTML export → confirm items parsed → edit → save
- **PDF/CSV export**: hit export endpoint → verify file downloads with correct data
- **Payment tracking**: record payment → verify net amounts update on summary page
