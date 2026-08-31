-- Bill items had no stable ordering column: `id` is a random UUID, so listing
-- items ordered by id would shuffle them on every insert. Items must come back
-- in the order they were entered, which is how a receipt reads.
ALTER TABLE bill_items
    ADD COLUMN IF NOT EXISTS created_at TIMESTAMPTZ NOT NULL DEFAULT now();

CREATE INDEX IF NOT EXISTS idx_bill_items_bill_created
    ON bill_items(bill_id, created_at);

-- Bills are listed newest first within a split; this supports that ordering.
CREATE INDEX IF NOT EXISTS idx_bills_split_date
    ON bills(split_id, date DESC, created_at DESC);
