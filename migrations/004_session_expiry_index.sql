-- Sessions accumulate forever: every login writes a row and nothing ever
-- removed one. A 30-day-old expired session is dead weight that still gets
-- scanned on every authenticated request.
--
-- The sweeper (backend/include/auth/session_sweeper.h) deletes expired rows
-- periodically; this index is what makes both that delete and the per-request
-- lookup cheap once the table is large.
CREATE INDEX IF NOT EXISTS idx_sessions_expires_at ON sessions(expires_at);
