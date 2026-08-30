#!/usr/bin/env bash
# Applies pending migrations from migrations/ in filename order.
#
# Each file is applied exactly once, inside a transaction, and recorded in the
# schema_migrations ledger. This matters: relying on "CREATE TABLE IF NOT
# EXISTS" alone silently skips a table whose definition has since changed,
# leaving the database quietly out of sync with the migration that claims to
# have created it. The ledger makes an applied migration a fact, not a guess.
#
# Usage:  ./scripts/migrate.sh                  # uses DATABASE_URL from backend/.env
#         DATABASE_URL=... ./scripts/migrate.sh
#         ./scripts/migrate.sh --status         # list applied / pending, apply nothing
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -z "${DATABASE_URL:-}" ]]; then
    env_file="$repo_root/backend/.env"
    [[ -f "$env_file" ]] || { echo "No DATABASE_URL set and $env_file not found" >&2; exit 1; }
    set -a; . "$env_file"; set +a
fi
: "${DATABASE_URL:?DATABASE_URL is not set}"

# Suppress "already exists, skipping" NOTICEs so real warnings stand out.
export PGOPTIONS='-c client_min_messages=warning'

psql_bin="$(command -v psql || echo /opt/homebrew/opt/libpq/bin/psql)"
[[ -x "$psql_bin" ]] || { echo "psql not found (brew install libpq)" >&2; exit 1; }
psql() { "$psql_bin" "$DATABASE_URL" -v ON_ERROR_STOP=1 -q "$@"; }

psql -c "CREATE TABLE IF NOT EXISTS schema_migrations (
             version     TEXT PRIMARY KEY,
             applied_at  TIMESTAMPTZ NOT NULL DEFAULT now()
         )" >/dev/null

shopt -s nullglob
migrations=("$repo_root"/migrations/*.sql)
(( ${#migrations[@]} )) || { echo "No migrations found" >&2; exit 1; }

applied=0
for f in "${migrations[@]}"; do
    version="$(basename "$f" .sql)"
    if [[ "$(psql -tAc "SELECT 1 FROM schema_migrations WHERE version = '$version'")" == "1" ]]; then
        [[ "${1:-}" == "--status" ]] && echo "  applied  $version"
        continue
    fi
    if [[ "${1:-}" == "--status" ]]; then
        echo "  PENDING  $version"
        continue
    fi
    echo "==> applying $version"
    # Single transaction: the migration and its ledger entry commit together,
    # so a failure halfway through leaves no partial state and no false record.
    psql --single-transaction \
         -f "$f" \
         -c "INSERT INTO schema_migrations (version) VALUES ('$version')"
    applied=$(( applied + 1 ))
done

[[ "${1:-}" == "--status" ]] && exit 0
if (( applied == 0 )); then echo "==> already up to date"; else echo "==> applied $applied migration(s)"; fi
