-- `bills.subtotal` was a denormalized copy of SUM(price * quantity) over the
-- bill's items. Nothing reads it: every API response derives the value in the
-- same query that returns the bill, so the stored column could only ever agree
-- with the truth or silently disagree with it.
--
-- Keeping it would be a trap for Phase 5, where receipt parsing bulk-inserts
-- items: any path that writes bill_items without also rewriting this column
-- leaves a wrong number sitting in a field that looks authoritative, and the
-- error would surface much later as a few cents in someone's share.
--
-- Deriving on read makes the invariant structural rather than something each
-- writer has to remember.

-- The view selects b.* and depends on the column, so it goes first. It was
-- never used: bill_routes computes subtotal and total itself.
DROP VIEW IF EXISTS bills_with_total;

ALTER TABLE bills DROP COLUMN IF EXISTS subtotal;
