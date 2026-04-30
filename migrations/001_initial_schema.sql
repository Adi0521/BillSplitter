-- Enable UUID generation
CREATE EXTENSION IF NOT EXISTS "pgcrypto";

-- ─── Users ────────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS users (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email         TEXT UNIQUE NOT NULL,
    display_name  TEXT,
    password_hash TEXT NOT NULL,   -- PBKDF2-SHA256 hex
    password_salt TEXT NOT NULL,   -- random 16-byte hex salt
    created_at    TIMESTAMPTZ DEFAULT now()
);

-- ─── Auth: session tokens ─────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS sessions (
    token      TEXT PRIMARY KEY,          -- random 32-byte hex
    user_id    UUID REFERENCES users(id) ON DELETE CASCADE,
    expires_at TIMESTAMPTZ NOT NULL
);

-- ─── Splits ───────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS splits (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    owner_id    UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name        TEXT NOT NULL,
    description TEXT,
    type        TEXT NOT NULL CHECK (type IN ('one_time', 'ongoing')),
    currency    TEXT NOT NULL DEFAULT 'USD',
    share_token TEXT UNIQUE DEFAULT encode(gen_random_bytes(16), 'hex'),
    created_at  TIMESTAMPTZ DEFAULT now(),
    archived_at TIMESTAMPTZ
);

-- ─── Split members ────────────────────────────────────────────────────────────
-- A member can be a registered user (user_id set) or a guest (user_id NULL).
CREATE TABLE IF NOT EXISTS split_members (
    id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    split_id  UUID NOT NULL REFERENCES splits(id) ON DELETE CASCADE,
    user_id   UUID REFERENCES users(id) ON DELETE SET NULL,
    name      TEXT NOT NULL,
    email     TEXT,
    joined_at TIMESTAMPTZ DEFAULT now()
);

-- ─── Bills ────────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS bills (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    split_id        UUID NOT NULL REFERENCES splits(id) ON DELETE CASCADE,
    store_name      TEXT NOT NULL,
    date            DATE NOT NULL,
    currency        TEXT NOT NULL DEFAULT 'USD',
    subtotal        NUMERIC(12,4) NOT NULL DEFAULT 0,
    tax             NUMERIC(12,4) NOT NULL DEFAULT 0,
    tip             NUMERIC(12,4) NOT NULL DEFAULT 0,
    fees            NUMERIC(12,4) NOT NULL DEFAULT 0,
    payer_member_id UUID REFERENCES split_members(id) ON DELETE SET NULL,
    created_at      TIMESTAMPTZ DEFAULT now()
);

-- Computed total view (avoids GENERATED ALWAYS AS for wider pg compat)
CREATE OR REPLACE VIEW bills_with_total AS
    SELECT *, (subtotal + tax + tip + fees) AS total
    FROM bills;

-- ─── Bill line items ──────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS bill_items (
    id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    bill_id  UUID NOT NULL REFERENCES bills(id) ON DELETE CASCADE,
    name     TEXT NOT NULL,
    price    NUMERIC(12,4) NOT NULL CHECK (price >= 0),
    quantity INT NOT NULL DEFAULT 1 CHECK (quantity > 0),
    currency TEXT NOT NULL DEFAULT 'USD'
);

-- ─── Per-item allocations ─────────────────────────────────────────────────────
-- For each item, each assigned member has either a ratio (%) or a fixed amount.
-- Over-allocation is prevented at the API layer, not with a DB constraint
-- (because individual rows may be inserted before all siblings exist).
CREATE TABLE IF NOT EXISTS item_allocations (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    bill_item_id    UUID NOT NULL REFERENCES bill_items(id) ON DELETE CASCADE,
    member_id       UUID NOT NULL REFERENCES split_members(id) ON DELETE CASCADE,
    allocation_mode TEXT NOT NULL CHECK (allocation_mode IN ('ratio', 'amount')),
    ratio           NUMERIC(7,4),    -- percent, e.g. 33.3333
    amount          NUMERIC(12,4),   -- dollars/cents, e.g. 5.00
    UNIQUE (bill_item_id, member_id),
    CONSTRAINT chk_one_mode CHECK (
        (allocation_mode = 'ratio'  AND ratio  IS NOT NULL AND amount IS NULL) OR
        (allocation_mode = 'amount' AND amount IS NOT NULL AND ratio  IS NULL)
    )
);

-- ─── Payments ─────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS payments (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    split_id    UUID NOT NULL REFERENCES splits(id) ON DELETE CASCADE,
    from_member UUID REFERENCES split_members(id) ON DELETE SET NULL,
    to_member   UUID REFERENCES split_members(id) ON DELETE SET NULL,
    amount      NUMERIC(12,4) NOT NULL CHECK (amount > 0),
    currency    TEXT NOT NULL DEFAULT 'USD',
    method      TEXT,   -- 'venmo', 'cash', 'zelle', 'other', etc.
    notes       TEXT,
    paid_at     TIMESTAMPTZ DEFAULT now()
);

-- ─── FX rate cache ────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS fx_rates (
    base_currency   TEXT NOT NULL,
    target_currency TEXT NOT NULL,
    rate            NUMERIC(16,8) NOT NULL CHECK (rate > 0),
    fetched_at      TIMESTAMPTZ DEFAULT now(),
    PRIMARY KEY (base_currency, target_currency)
);

-- ─── Indexes ──────────────────────────────────────────────────────────────────
CREATE INDEX IF NOT EXISTS idx_splits_owner       ON splits(owner_id);
CREATE INDEX IF NOT EXISTS idx_split_members_split ON split_members(split_id);
CREATE INDEX IF NOT EXISTS idx_bills_split         ON bills(split_id);
CREATE INDEX IF NOT EXISTS idx_bill_items_bill     ON bill_items(bill_id);
CREATE INDEX IF NOT EXISTS idx_allocations_item    ON item_allocations(bill_item_id);
CREATE INDEX IF NOT EXISTS idx_payments_split      ON payments(split_id);
CREATE INDEX IF NOT EXISTS idx_sessions_user       ON sessions(user_id);
