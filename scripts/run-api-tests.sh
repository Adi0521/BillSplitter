#!/usr/bin/env bash
# Runs the API integration suite against a throwaway database.
#
# The tests create and delete rows freely, so they must never touch the
# development database. This script builds `billsplitter_test` from scratch on
# every run: a fresh database is also the only way to be sure the migrations
# themselves still work from zero, which is a thing worth testing on every run.
#
# Usage:  ./scripts/run-api-tests.sh            # all suites
#         ./scripts/run-api-tests.sh test_auth  # one module
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

PORT="${BILLSPLITTER_TEST_PORT:-8099}"
ADMIN_URL="${BILLSPLITTER_ADMIN_URL:-postgresql://billsplitter:billsplitter@localhost:5433/postgres}"
TEST_DB="${BILLSPLITTER_TEST_DB:-billsplitter_test}"
TEST_URL="${ADMIN_URL%/*}/$TEST_DB"
BIN="$repo_root/backend/build/billsplitter-backend"

psql_bin="$(command -v psql || echo /opt/homebrew/opt/libpq/bin/psql)"
[[ -x "$psql_bin" ]] || { echo "psql not found (brew install libpq)" >&2; exit 1; }
[[ -x "$BIN" ]] || { echo "backend not built — run: cd backend && cmake --build build -j8" >&2; exit 1; }

server_pid=""
cleanup() {
    [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
}
trap cleanup EXIT

echo "==> rebuilding $TEST_DB"
# FORCE drops other sessions' connections; without it a leftover connection from
# a crashed run makes DROP DATABASE hang forever.
PGOPTIONS='-c client_min_messages=warning' "$psql_bin" "$ADMIN_URL" -v ON_ERROR_STOP=1 -q \
    -c "DROP DATABASE IF EXISTS $TEST_DB WITH (FORCE)" \
    -c "CREATE DATABASE $TEST_DB"

echo "==> applying migrations"
DATABASE_URL="$TEST_URL" ./scripts/migrate.sh >/dev/null

echo "==> starting backend on :$PORT"
log="$(mktemp -t billsplitter-test-server)"
# A low failure ceiling makes the throttle reachable in a test without
# hundreds of requests. Everything else runs at its production default.
DATABASE_URL="$TEST_URL" PORT="$PORT" APP_BASE_URL="http://localhost:5173" \
    LOGIN_MAX_FAILURES="${LOGIN_MAX_FAILURES:-5}" \
    "$BIN" >"$log" 2>&1 &
server_pid=$!

for _ in $(seq 1 60); do
    curl -sf -m 1 "http://localhost:$PORT/api/health" >/dev/null 2>&1 && break
    kill -0 "$server_pid" 2>/dev/null || { echo "server died:"; cat "$log"; exit 1; }
    sleep 0.25
done
curl -sf -m 2 "http://localhost:$PORT/api/health" >/dev/null || {
    echo "server never became healthy:"; cat "$log"; exit 1; }

echo "==> running tests"
set +e
# -t must equal -s so that tests/api lands on sys.path and `from harness
# import ...` resolves without the package needing an __init__.py.
pattern="test_*.py"
[[ $# -ge 1 ]] && pattern="$1.py"
BILLSPLITTER_TEST_URL="http://localhost:$PORT" \
    LOGIN_MAX_FAILURES="${LOGIN_MAX_FAILURES:-5}" \
    python3 -m unittest discover -s tests/api -t tests/api -p "$pattern" -v
status=$?
set -e

# A 500 means a handler threw where it should have validated; surface it even
# when every assertion passed, because it is a latent bug either way.
if grep -qE "\[ERROR\s*\]" "$log"; then
    echo
    echo "==> server logged errors during the run:"
    grep -E "\[ERROR\s*\]" "$log" | sort | uniq -c | sort -rn | head -20
fi

exit $status
