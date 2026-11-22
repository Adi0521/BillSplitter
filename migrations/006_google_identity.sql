-- Sign in with Google, and the verified-email flag that makes seat matching safe.
--
-- The point of this migration is `email_verified`. Until now an address was
-- whatever someone typed at registration — never checked — so matching a split
-- seat to an account by email would have let anyone register as
-- someone@gmail.com and inherit their splits. Google tells us whether it has
-- verified the address, so a seat may be matched to an account only when that
-- flag is true.

-- A Google-only account has no password. Password-only accounts keep theirs.
ALTER TABLE users ALTER COLUMN password_hash DROP NOT NULL;
ALTER TABLE users ALTER COLUMN password_salt DROP NOT NULL;

-- Google's stable subject id. Not the email: a Google account can change its
-- address, and `sub` is what identifies the same person across that change.
ALTER TABLE users ADD COLUMN IF NOT EXISTS google_sub     TEXT UNIQUE;
ALTER TABLE users ADD COLUMN IF NOT EXISTS email_verified BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE users ADD COLUMN IF NOT EXISTS verified_at    TIMESTAMPTZ;

-- Every existing account was created with a password and an address nobody
-- checked, so none of them are verified. This is deliberate: those accounts
-- must not start matching seats by email.
UPDATE users SET email_verified = FALSE WHERE email_verified IS NULL;

-- An account must be reachable by at least one means. Without this, a bug that
-- cleared a password on a non-Google account would silently orphan it.
ALTER TABLE users DROP CONSTRAINT IF EXISTS chk_users_has_credential;
ALTER TABLE users ADD CONSTRAINT chk_users_has_credential CHECK (
    google_sub IS NOT NULL
    OR (password_hash IS NOT NULL AND password_salt IS NOT NULL)
);

-- Password columns travel together or not at all.
ALTER TABLE users DROP CONSTRAINT IF EXISTS chk_users_password_pair;
ALTER TABLE users ADD CONSTRAINT chk_users_password_pair CHECK (
    (password_hash IS NULL) = (password_salt IS NULL)
);

-- Seat matching looks up unlinked seats by address on every verified sign-in.
CREATE INDEX IF NOT EXISTS idx_split_members_email_unlinked
    ON split_members(lower(email)) WHERE user_id IS NULL AND email IS NOT NULL;

-- Links every unlinked seat carrying this address to this user, and reports how
-- many it took. Defined here rather than in C++ because it is one statement and
-- because the rules that keep it safe are all SQL:
--
--   * the caller must pass a user whose email_verified is true — enforced here,
--     not trusted from the call site. An unverified address is an unproven
--     claim, and matching on it is the account-takeover hole this whole
--     migration exists to close.
--   * a seat already linked to someone is never reassigned.
--   * a split where this user already holds a seat is skipped, so the partial
--     unique index on (split_id, user_id) cannot fire.
--   * addresses compare case-insensitively; people capitalise inconsistently.
CREATE OR REPLACE FUNCTION link_seats_by_verified_email(p_user UUID)
RETURNS INTEGER
LANGUAGE plpgsql
AS $$
DECLARE
    v_email    TEXT;
    v_verified BOOLEAN;
    v_count    INTEGER;
BEGIN
    SELECT email, email_verified INTO v_email, v_verified
      FROM users WHERE id = p_user;

    IF v_email IS NULL OR v_verified IS NOT TRUE THEN
        RETURN 0;
    END IF;

    WITH linked AS (
        UPDATE split_members m
           SET user_id = p_user, linked_at = now(), invite_token = NULL
         WHERE m.user_id IS NULL
           AND m.email IS NOT NULL
           AND lower(m.email) = lower(v_email)
           AND NOT EXISTS (SELECT 1 FROM split_members other
                            WHERE other.split_id = m.split_id
                              AND other.user_id  = p_user)
        RETURNING 1
    )
    SELECT COUNT(*) INTO v_count FROM linked;

    RETURN v_count;
END
$$;
