import { chromium } from 'playwright'
const BASE='http://localhost:5173', shots=process.env.SHOTS
const fails=[], log=(ok,m)=>{console.log(`  ${ok?'PASS':'FAIL'}  ${m}`); if(!ok)fails.push(m)}
const b=await chromium.launch({channel:'chrome'})
const p=await b.newPage({viewport:{width:1280,height:1000}})
const errs=[]; p.on('console',m=>{if(m.type()==='error')errs.push(m.text())}); p.on('pageerror',e=>errs.push('pageerror: '+e.message))

await p.goto(BASE+'/login',{waitUntil:'networkidle'})
await p.fill('input[type=email]','a1@example.com'); await p.fill('input[type=password]','hunter2hunter2')
await p.click('button[type=submit]'); await p.waitForURL(u=>!u.pathname.startsWith('/login'))

// split detail -> bills section
await p.goto(BASE+'/',{waitUntil:'networkidle'})
await p.locator('xpath=//a[contains(@href,"/splits/") and not(contains(@href,"/new"))]').first().click()
await p.waitForURL(/\/splits\/[0-9a-f-]{36}$/)
await p.waitForFunction(() => /Safeway/.test(document.body.innerText), null, {timeout:15000})
let t=await p.textContent('body')
log(/Bills/i.test(t),'SplitDetailView renders a Bills section')
log(/Safeway/.test(t),'bills list shows the fixture bill')
log(!/Not yet implemented/i.test(t),'the "not yet implemented" placeholder is gone')
log(/65\.32/.test(t),'bill total renders 2dp from the server string')
log(!/65\.3200/.test(t),'4-decimal raw value is not shown to the user')
log(!/Invalid Date/.test(t),'no Invalid Date in bills list')
log(/Aug 30, 2026/.test(t),'bill DATE renders (the formatDate bug)')
if(shots) await p.screenshot({path:`${shots}/p3-1-split.png`,fullPage:true})

// bill detail
// "/bills/new" also contains "/bills/", so require a uuid-shaped tail.
await p.locator('xpath=//a[contains(@href,"/bills/") and not(contains(@href,"/new"))]').first().click()
await p.waitForURL(/\/bills\/[0-9a-f-]{36}$/)
await p.waitForFunction(() => /Olive oil/.test(document.body.innerText), null, {timeout:15000})
t=await p.textContent('body')
log(/Safeway/.test(t),'BillView loaded the bill')
log(/Olive oil/.test(t)&&/Coffee beans/.test(t),'line items render')
log(/61\.49/.test(t),'server subtotal renders')
log(/65\.32/.test(t),'server total renders')
log(!/Invalid Date/.test(t),'no Invalid Date on bill detail')
log(/Aug 30, 2026/.test(t),'bill date renders on detail')
log(!/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}/.test(t),'no raw uuid leaked into the UI (payer resolved to a name)')
if(shots) await p.screenshot({path:`${shots}/p3-2-bill.png`,fullPage:true})

// add an item through the UI and confirm the server-derived subtotal moves
const before=(await p.textContent('body')).match(/61\.49/)?'61.49':null
const nameIn=p.locator('input').first()
const inputs=await p.locator('form input').all()
log(inputs.length>=2,`add-item form present (${inputs.length} inputs)`)
await p.locator('xpath=//input[@id="item-name" or @placeholder="Olive oil" or contains(@placeholder,"name")]').first().fill('E2E Widget').catch(()=>{})
await p.locator('xpath=//input[@id="item-price" or contains(@placeholder,"12.5") or contains(@placeholder,"price")]').first().fill('10.00').catch(()=>{})
await p.locator('button[type=submit]').filter({hasText:/add/i}).first().click().catch(()=>{})
await p.waitForTimeout(1500)
t=await p.textContent('body')
log(/E2E Widget/.test(t),'item added through the UI')
log(/71\.49/.test(t),'subtotal recomputed server-side after add (61.49 + 10.00)')
log(/75\.32/.test(t),'total recomputed server-side after add')
if(shots) await p.screenshot({path:`${shots}/p3-3-item-added.png`,fullPage:true})

// 404
await p.goto(BASE+'/splits/'+p.url().split('/splits/')[1].split('/')[0]+'/bills/00000000-0000-0000-0000-000000000000',{waitUntil:'networkidle'})
await p.waitForFunction(() => /not found/i.test(document.body.innerText), null, {timeout:15000}).catch(()=>{})
t=await p.textContent('body')
log(/not found/i.test(t),'nonexistent bill renders a not-found state')
if(shots) await p.screenshot({path:`${shots}/p3-4-notfound.png`,fullPage:true})

console.log('\n  console errors:', errs.length?errs.slice(0,4):'none')
await b.close()
console.log(fails.length?`\nFAILURES (${fails.length}):\n - `+fails.join('\n - '):'\nAll Phase 3 browser checks passed.')
process.exit(fails.length?1:0)
