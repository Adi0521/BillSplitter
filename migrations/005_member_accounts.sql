-- Members become people: an invite token binds a member row to an account.
--
-- Emails are never verified in this system (there is no mail service), so
-- matching members to accounts by email would let anyone register as
-- "alice@herwork.com" and see every split Alice was ever added to. The token IS
-- the secret instead — the owner hands the link to the person it is for, by any
-- channel they trust — which is the same pattern splits.share_token already uses.

ALTER TABLE split_members ADD COLUMN IF NOT EXISTS invite_token TEXT UNIQUE;
ALTER TABLE split_members ADD COLUMN IF NOT EXISTS invited_at   TIMESTAMPTZ;
ALTER TABLE split_members ADD COLUMN IF NOT EXISTS linked_at    TIMESTAMPTZ;

-- One account holds at most one seat in a split. Partial, so the many NULL
-- (unlinked) rows do not collide.
CREATE UNIQUE INDEX IF NOT EXISTS uq_split_members_split_user
    ON split_members(split_id, user_id) WHERE user_id IS NOT NULL;

-- "Which splits am I in?" — the Invited tab.
CREATE INDEX IF NOT EXISTS idx_split_members_user
    ON split_members(user_id) WHERE user_id IS NOT NULL;

-- Existing owner rows were linked at creation; record that so linked_at is
-- never NULL for a linked row.
UPDATE split_members SET linked_at = joined_at
 WHERE user_id IS NOT NULL AND linked_at IS NULL;

-- THE access rule, defined once.
--
-- Every route that touches a split asks this function instead of comparing
-- owner_id itself. Before this migration the rule "who may touch this split"
-- was written out 39 times across 11 files; a rule that must be changed in 39
-- places is a rule that will be wrong in one of them.
--
--   'owner'  — created the split; may do everything, including the four
--              administrative actions (archive, remove members, manage
--              invites, regenerate the share link)
--   'member' — an account bound to a member row; may do everything else
--   NULL     — no access at all; routes answer 404, never 403, so the split's
--              existence is not revealed
CREATE OR REPLACE FUNCTION split_role(p_split UUID, p_user UUID)
RETURNS TEXT
LANGUAGE sql
STABLE
AS $$
    SELECT CASE
        WHEN EXISTS (SELECT 1 FROM splits s
                      WHERE s.id = p_split AND s.owner_id = p_user)
            THEN 'owner'
        WHEN EXISTS (SELECT 1 FROM split_members m
                      WHERE m.split_id = p_split AND m.user_id = p_user)
            THEN 'member'
    END
$$;
