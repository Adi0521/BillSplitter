import { chromium } from 'playwright'
const BASE='http://localhost:5173', shots=process.env.SHOTS
const fails=[], log=(ok,m)=>{console.log(`  ${ok?'PASS':'FAIL'}  ${m}`); if(!ok)fails.push(m)}
const SID='e4901405-3a7b-4c23-9063-d9b25d3fed24', BID='86dd3d8a-5ebe-4276-a2f5-ba7b8e12290b'
const b=await chromium.launch({channel:'chrome'})
const p=await b.newPage({viewport:{width:1400,height:1200}})
const errs=[]; p.on('console',m=>{if(m.type()==='error')errs.push(m.text())}); p.on('pageerror',e=>errs.push('pageerror: '+e.message))
const text=()=>p.textContent('body')

await p.goto(BASE+'/login',{waitUntil:'networkidle'})
await p.fill('input[type=email]','a1@example.com'); await p.fill('input[type=password]','hunter2hunter2')
await p.click('button[type=submit]'); await p.waitForURL(u=>!u.pathname.startsWith('/login'))

await p.goto(`${BASE}/splits/${SID}/bills/${BID}`,{waitUntil:'networkidle'})
await p.waitForFunction(()=>/Olive oil/.test(document.body.innerText),null,{timeout:15000})
await p.waitForFunction(()=>/owes|Who owes/i.test(document.body.innerText),null,{timeout:15000}).catch(()=>{})
let t=await text()

log(/who owes/i.test(t),'shares panel replaced the Phase 4 placeholder')
log(!/Not yet implemented/i.test(t),'no "not yet implemented" text remains')
log(/Unallocated/i.test(t),'the unallocated row is shown, not hidden')
log(/27\.73/.test(t),'Bob owes Alice 27.73 (server-computed owes_payer)')
log(/0\.93/.test(t),'Alice tax share 0.93 = 15.00/61.49*3.83 (full-subtotal denominator)')
log(!/1\.96/.test(t),'the wrong-denominator value 1.96 does NOT appear')
log(/25\.43|22\.24/.test(t),'unallocated amounts rendered')
log(!/\[object Object\]|NaN|undefined/.test(t),'no NaN/undefined leakage')
if(shots) await p.screenshot({path:`${shots}/p4-1-shares.png`,fullPage:true})

// open the allocator on a line
const allocBtn=p.locator('button',{hasText:/allocate/i}).first()
log(await allocBtn.count()>0,'per-item Allocate control exists')
await allocBtn.click()
await p.waitForFunction(()=>/ratio|amount/i.test(document.body.innerText),null,{timeout:15000}).catch(()=>{})
t=await text()
log(/ratio/i.test(t)&&/amount/i.test(t),'allocator shows the ratio/amount mode toggle')
log(/even split/i.test(t),'even-split action present')
if(shots) await p.screenshot({path:`${shots}/p4-2-allocator.png`,fullPage:true})

// even-split an unallocated line and confirm the remainder surfaces
await p.goto(`${BASE}/splits/${SID}/bills/${BID}`,{waitUntil:'networkidle'})
await p.waitForFunction(()=>/Coffee beans/.test(document.body.innerText),null,{timeout:15000})
const rows=await p.locator('tr').all()
for(const r of rows){
  if(/Coffee beans/.test(await r.textContent())){
    const btn=r.locator('button',{hasText:/allocate/i}).first()
    if(await btn.count()) await btn.click()
    break
  }
}
await p.waitForTimeout(1200)
// tick every member checkbox in the open allocator, then even-split
const boxes=await p.locator('input[type=checkbox]').all()
for(const c of boxes){ if(await c.isVisible()) await c.check().catch(()=>{}) }
const even=p.locator('button',{hasText:/even split/i}).first()
if(await even.count()) await even.click()
await p.waitForTimeout(2000)
t=await text()
// 18.99 / 3 = 6.33 each, 0.00 remainder; /2 = 9.495 -> 9.49 x2 = 18.98, 0.01 left
log(/6\.33|9\.49/.test(t),'even split produced floored per-member shares')
log(!/Invalid|NaN/.test(t),'no NaN after even split')
if(shots) await p.screenshot({path:`${shots}/p4-3-evensplit.png`,fullPage:true})

console.log('\n  console errors:', errs.length?errs.slice(0,4):'none')
await b.close()
console.log(fails.length?`\nFAILURES (${fails.length}):\n - `+fails.join('\n - '):'\nAll Phase 4 browser checks passed.')
process.exit(fails.length?1:0)
