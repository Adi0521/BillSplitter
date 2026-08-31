# Browser tests

`splits.e2e.mjs` drives the real app in Chrome and checks the Phase 2 flows:
login, the splits list, split detail, adding a member, the not-found state, and
creating a split.

Playwright is deliberately **not** in `package.json` — it is a heavy dependency
and nothing in CI runs these yet. Install it where you want to run them:

```bash
npm i -D playwright          # or: npm i -g playwright
```

Then, with Postgres, the backend, and `npm run dev` all running:

```bash
node e2e/splits.e2e.mjs
SHOTS=/tmp/shots node e2e/splits.e2e.mjs   # also write screenshots
```

It uses the system Chrome (`channel: 'chrome'`) rather than a downloaded
browser. The suite creates a split and a member and does not clean them up, so
run it against a development database.

Assertions are written against whatever data is present rather than fixed names,
so it does not break when the database has different splits in it.
