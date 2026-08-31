import { chromium } from 'playwright'

const BASE = 'http://localhost:5173'
const shots = process.env.SHOTS
const fails = []
const log = (ok, msg) => { console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${msg}`); if (!ok) fails.push(msg) }

const browser = await chromium.launch({ channel: 'chrome' })
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } })

// Surface anything the app logs or throws — a view can compile and still crash at runtime.
const consoleErrors = []
page.on('console', m => { if (m.type() === 'error') consoleErrors.push(m.text()) })
page.on('pageerror', e => consoleErrors.push('pageerror: ' + e.message))

// ── login ────────────────────────────────────────────────────────────────────
await page.goto(BASE + '/login', { waitUntil: 'networkidle' })
log(await page.locator('h1', { hasText: 'BillSplitter' }).isVisible(), 'login page renders')
await page.fill('input[type=email]', 'a1@example.com')
await page.fill('input[type=password]', 'hunter2hunter2')
await page.click('button[type=submit]')
await page.waitForURL(u => !u.pathname.startsWith('/login'), { timeout: 10000 })
log(true, 'login redirects away from /login')

// ── splits list ──────────────────────────────────────────────────────────────
await page.goto(BASE + '/', { waitUntil: 'networkidle' })
const body = await page.textContent('body')
log(/My Splits/.test(body), 'SplitsView shows heading')
log(/Tahoe 2026/.test(body), 'SplitsView lists a real split from the API')
log(/member/.test(body), 'SplitsView shows member count')
log(!/one_time|ongoing/.test(body), 'SplitsView never leaks the raw type enum')
log(!/Invalid Date/.test(body), 'SplitsView has no Invalid Date')
log(!/\[object Object\]|undefined|NaN/.test(body), 'SplitsView has no undefined/NaN leakage')
if (shots) await page.screenshot({ path: `${shots}/1-splits.png`, fullPage: true })

// ── detail ───────────────────────────────────────────────────────────────────
// "/splits/new" also matches a prefix selector, so match the id shape instead.
// Assert against whatever split we actually clicked rather than a hardcoded
// name, so the suite survives its own test data.
const firstCard = page.locator('xpath=//a[contains(@href,"/splits/") and not(contains(@href,"/new"))]').first()
const clickedName = (await firstCard.textContent()).trim().split('\n')[0].trim()
await firstCard.click()
await page.waitForURL(/\/splits\/[0-9a-f-]{36}$/, { timeout: 10000 })
await page.waitForLoadState('networkidle')
const d = await page.textContent('body')
log(d.includes(clickedName), `SplitDetailView loaded the split we clicked (${clickedName})`)
log(/Members/i.test(d), 'SplitDetailView renders a members section')
log(!/one_time|ongoing/.test(d), 'SplitDetailView never leaks the raw enum')
log(!/Invalid Date/.test(d), 'SplitDetailView has no Invalid Date')
if (shots) await page.screenshot({ path: `${shots}/2-detail.png`, fullPage: true })

// add a member through the real UI
await page.fill('#member-name', 'E2E Tester')
await page.fill('#member-email', 'e2e@example.com')
await page.locator('button[type=submit]', { hasText: /add member/i }).click()
await page.waitForFunction(() => document.body.innerText.includes('E2E Tester'), null, { timeout: 8000 }).catch(() => {})
const after = await page.textContent('body')
log(/E2E Tester/.test(after), 'adding a member through the UI works')
log(/e2e@example.com/.test(after), 'the new member\'s email renders')
log(await page.inputValue('#member-name') === '', 'add-member form clears on success')
if (shots) await page.screenshot({ path: `${shots}/3-member-added.png`, fullPage: true })

// ── 404 path ─────────────────────────────────────────────────────────────────
await page.goto(BASE + '/splits/00000000-0000-0000-0000-000000000000', { waitUntil: 'networkidle' })
const nf = await page.textContent('body')
log(/not found/i.test(nf), 'nonexistent split renders a not-found state')
log(!/\[object Object\]|Request failed/.test(nf), '404 state is human-readable, not a raw error')
if (shots) await page.screenshot({ path: `${shots}/4-notfound.png`, fullPage: true })

// ── new split form ───────────────────────────────────────────────────────────
await page.goto(BASE + '/splits/new', { waitUntil: 'networkidle' })
const nsBody = await page.textContent('body')
log(/One-time/.test(nsBody) && /Ongoing/.test(nsBody), 'NewSplitView offers both types, humanized')
await page.click('button[type=submit]')          // submit empty -> client validation
await page.waitForTimeout(400)
log(/name/i.test(await page.textContent('body')), 'NewSplitView blocks empty submit with a message')
log(page.url().includes('/splits/new'), 'NewSplitView did not navigate on invalid submit')
if (shots) await page.screenshot({ path: `${shots}/5-newsplit.png`, fullPage: true })

// create one for real
await page.fill('input[type=text]', 'E2E Ongoing Split')
await page.locator('text=Ongoing').first().click()
await page.click('button[type=submit]')
await page.waitForURL(/\/splits\/[0-9a-f-]{36}$/, { timeout: 10000 })
await page.waitForLoadState('networkidle')
const created = await page.textContent('body')
log(/E2E Ongoing Split/.test(created), 'creating a split navigates to its detail page')
log(/Ongoing/.test(created), 'the Ongoing badge renders for a real ongoing split')
if (shots) await page.screenshot({ path: `${shots}/6-created.png`, fullPage: true })

console.log('\n  console errors:', consoleErrors.length ? consoleErrors.slice(0,5) : 'none')
await browser.close()
console.log(fails.length ? `\nFAILURES (${fails.length}):\n - ` + fails.join('\n - ') : '\nAll browser checks passed.')
process.exit(fails.length ? 1 : 0)
