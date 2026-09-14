# Browser tests

Each file drives the real app in Chrome:

- `splits.e2e.mjs` — Phase 2: login, splits list, detail, adding a member,
  not-found, creating a split.
- `bills.e2e.mjs` — Phase 3: bills list, bill detail, adding an item, and that
  the server-derived subtotal moves.
- `allocations.e2e.mjs` — Phase 4: the allocator, even split, and the per-bill
  share panel.
- `receipt-parse.e2e.mjs` — Phase 5: the receipt uploader, an HTML receipt
  whose items deliberately do not match its printed total (so the cross-check
  fires), the unmatched coupon line, and a refused PDF.
- `summary-payments-share.e2e.mjs` — Phase 6: the split summary, recording a
  payment, and the public share view **in a session-free browser context** —
  which is how it proves the share page needs no account, sets no cookies, and
  leaks no email address.

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
browser. The suites create fixture data and only partly clean it up — `DELETE
/api/splits/:id` archives rather than removes, so `ZZ*`-named splits accumulate.
Run these against a development database, and clear leftovers with:

```sql
DELETE FROM splits WHERE name LIKE 'ZZ%';
```

Assertions are written against whatever data is present rather than fixed names,
so it does not break when the database has different splits in it.
