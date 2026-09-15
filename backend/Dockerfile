# syntax=docker/dockerfile:1
#
# Build from the REPOSITORY ROOT, not from backend/:
#     docker build -f backend/Dockerfile -t billsplitter-backend .
#
# The context is the root because the unit-test target globs ../tests/unit
# relative to backend/CMakeLists.txt, and a Dockerfile cannot COPY from outside
# its context.

# ── Build stage ───────────────────────────────────────────────────────────────
# Crow, nlohmann/json and doctest arrive via FetchContent, so the builder needs
# git and network access. Everything else is a system package.
# trixie, not bookworm or Ubuntu 24.04: this code calls the two-argument
# txn.exec(sql, params) added in libpqxx 7.10. bookworm has 6.4 (no
# pqxx::params at all) and Ubuntu 24.04 has 7.8 (params, but not that
# overload). Check `pkg-config --modversion libpqxx` before changing this.
FROM debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates pkg-config \
        libpqxx-dev libpq-dev libssl-dev libcurl4-openssl-dev libasio-dev \
        libtesseract-dev libleptonica-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY backend/CMakeLists.txt ./
COPY backend/include ./include
COPY backend/src ./src
# ../tests/unit relative to /src — the path backend/CMakeLists.txt globs.
COPY tests/unit /tests/unit

# Warnings fail the image. They are how the small mistakes in this codebase have
# surfaced, and a container build is the last place to start tolerating them.
# NB: the build output is redirected, not piped. `cmake --build ... | tee` would
# report tee's exit status, and /bin/sh here is dash, which has no pipefail — so
# a failing build silently satisfied the `&&` chain and only surfaced later as a
# missing binary.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && { cmake --build build -j"$(nproc)" > /tmp/build.log 2>&1 \
      || { echo "BUILD FAILED:"; cat /tmp/build.log; exit 1; }; } \
 && if grep "warning:" /tmp/build.log | grep -vE "/build/_deps/" | grep .; then \
      echo "build produced warnings in first-party code"; exit 1; \
    fi

# Run the C++ unit tests during the build, so a broken parser cannot be shipped.
# The API integration tests need a database and run in CI, not here.
RUN ./build/unit-tests

# ── Runtime stage ─────────────────────────────────────────────────────────────
# Same base as the builder so shared-library ABIs match. Runtime libraries only:
# no compiler, no headers, no git.
FROM debian:trixie-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl \
        libpqxx-7.10 libpq5 libssl3t64 libcurl4t64 \
        libtesseract5 libleptonica6 tesseract-ocr-eng \
    && rm -rf /var/lib/apt/lists/*

# Unprivileged: the process writes nothing to disk.
RUN useradd --system --create-home --shell /usr/sbin/nologin billsplitter
USER billsplitter
WORKDIR /app

COPY --from=build /src/build/billsplitter-backend /usr/local/bin/billsplitter-backend

# Where Debian puts the English language data. The OCR pool reads
# BILLSPLITTER_TESSDATA first, so this is what makes receipt parsing work in a
# container with no TESSDATA_PREFIX set.
ENV BILLSPLITTER_TESSDATA=/usr/share/tesseract-ocr/5/tessdata
ENV PORT=8080
EXPOSE 8080

# /api/health touches no database, so this reports "the process is serving",
# not "the database is reachable" — which is what a restart policy should key
# on. A database outage must not cycle an otherwise healthy process.
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -fsS "http://127.0.0.1:${PORT}/api/health" || exit 1

ENTRYPOINT ["/usr/local/bin/billsplitter-backend"]
