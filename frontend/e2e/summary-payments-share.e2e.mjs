import { chromium } from 'playwright'
const BASE='http://localhost:5173', API='http://localhost:8080', shots=process.env.SHOTS
const fails=[], log=(ok,m)=>{console.log(`  ${ok?'PASS':'FAIL'}  ${m}`); if(!ok)fails.push(m)}
const b=await chromium.launch({channel:'chrome'})
const p=await b.newPage({viewport:{width:1400,height:1400}})
const errs=[]; p.on('console',m=>{if(m.type()==='error')errs.push(m.text())}); p.on('pageerror',e=>errs.push('pageerror: '+e.message))

// ── build a fixture through the API in the browser's own session ────────────
await p.goto(BASE+'/login',{waitUntil:'networkidle'})
await p.fill('input[type=email]','a1@example.com'); await p.fill('input[type=password]','hunter2hunter2')
await p.click('button[type=submit]'); await p.waitForURL(u=>!u.pathname.startsWith('/login'))

const fx = await p.evaluate(async () => {
  const j = async (m,u,b) => (await fetch(u,{method:m,headers:{'Content-Type':'application/json'},
    credentials:'include', body:b?JSON.stringify(b):undefined})).json()
  const split = await j('POST','/api/splits',{name:'ZZ Phase6 E2E',type:'one_time',currency:'USD'})
  const members = await j('GET',`/api/splits/${split.id}/members`)
  const alice = members[0]
  const bob = await j('POST',`/api/splits/${split.id}/members`,{name:'Bob'})
  const carol = await j('POST',`/api/splits/${split.id}/members`,{name:'Carol'})
  // Alice fronts dinner, fully split; Bob fronts the taxi, only partly allocated
  const b1 = await j('POST',`/api/splits/${split.id}/bills`,{store_name:'Dinner',date:'2026-09-01',tax:'10.00',payer_member_id:alice.id})
  const i1 = await j('POST',`/api/bills/${b1.id}/items`,{name:'Mains',price:'100.00'})
  await j('PUT',`/api/bills/${b1.id}/items/${i1.id}/allocations`,{mode:'ratio',allocations:[
    {member_id:alice.id,ratio:'50'},{member_id:bob.id,ratio:'30'},{member_id:carol.id,ratio:'20'}]})
  const b2 = await j('POST',`/api/splits/${split.id}/bills`,{store_name:'Taxi',date:'2026-09-02',payer_member_id:bob.id})
  const i2 = await j('POST',`/api/bills/${b2.id}/items`,{name:'Fare',price:'50.00'})
  await j('PUT',`/api/bills/${b2.id}/items/${i2.id}/allocations`,{mode:'amount',allocations:[{member_id:alice.id,amount:'20.00'}]})
  await j('POST',`/api/splits/${split.id}/payments`,{from_member:carol.id,to_member:alice.id,amount:'5.00',method:'venmo'})
  const full = await j('GET',`/api/splits/${split.id}`)
  return {id:split.id, token:full.share_token}
})

// ── the link I just added ───────────────────────────────────────────────────
await p.goto(`${BASE}/splits/${fx.id}`,{waitUntil:'networkidle'})
await p.waitForFunction(()=>/Members/.test(document.body.innerText),null,{timeout:15000})
const link=p.locator('a',{hasText:/who owes what/i}).first()
log(await link.count()>0,'SplitDetailView links to the summary')
await link.click()
await p.waitForURL(/\/summary$/,{timeout:15000})
log(!p.url().includes('undefined'),'the summary link resolves to a real id (not undefined)')

// ── summary ─────────────────────────────────────────────────────────────────
await p.waitForFunction(()=>/Carol/.test(document.body.innerText)
  && /allocated to nobody|not assigned|unallocated/i.test(document.body.innerText),
  null,{timeout:20000})
let t=await p.textContent('body')
log(/ZZ Phase6 E2E/.test(t),'summary shows the split')
log(/Carol/.test(t)&&/Bob/.test(t),'every member appears')
log(!/one_time|ongoing/.test(t),'no raw enum leaked')
log(!/Invalid Date|NaN|\[object Object\]/.test(t),'no NaN/Invalid Date/object leakage')
log(/owes the split|the split owes|settled up/i.test(t),'balance sign is stated in words, not just a minus')
log(/unallocated|not assigned|nobody/i.test(t),'the unallocated remainder is surfaced')
log(/5\.00/.test(t),'the recorded payment is reflected')
if(shots) await p.screenshot({path:`${shots}/p6-1-summary.png`,fullPage:true})

// record a payment through the embedded tracker
const before=(await p.locator('li').count())
const froms=p.locator('select')
if(await froms.count()>=2){
  await froms.nth(0).selectOption({index:1}).catch(()=>{})
  await froms.nth(1).selectOption({index:2}).catch(()=>{})
}
const amt=p.locator('input').filter({hasText:''}).nth(0)
await p.locator('xpath=//input[contains(@id,"amount") or contains(@placeholder,"0.00")]').first().fill('7.25').catch(()=>{})
await p.locator('button[type=submit]').filter({hasText:/record|save|add/i}).first().click().catch(()=>{})
await p.waitForTimeout(1800)
t=await p.textContent('body')
log(/7\.25/.test(t),'a payment recorded through the UI appears')
log(!/AliceUSD|BobUSD|CarolUSD/.test(t),'no missing space between name and currency (the condensing bug)')
if(shots) await p.screenshot({path:`${shots}/p6-2-payment.png`,fullPage:true})

// ── public share view, in a session-free context ────────────────────────────
const anon = await b.newContext()
const ap = await anon.newPage()
const anonErrs=[]; ap.on('pageerror',e=>anonErrs.push(e.message))
await ap.goto(`${BASE}/share/${fx.token}`,{waitUntil:'networkidle'})
await ap.waitForFunction(()=>/owes|balance|Nothing/i.test(document.body.innerText),null,{timeout:15000}).catch(()=>{})
const at=await ap.textContent('body')
log(!ap.url().includes('/login'),'the share link does not bounce a stranger to /login')
log(/ZZ Phase6 E2E/.test(at),'share view renders the split')
log(!/@/.test(at),'no email address appears anywhere on the public page')
log(!/Log out/.test(at),'no logged-in chrome (AppLayout) on the public page')
log((await anon.cookies()).length===0,'the public page set no cookies')
if(shots) await ap.screenshot({path:`${shots}/p6-3-share.png`,fullPage:true})

// revoked token
await p.evaluate(async (id)=>{ await fetch(`/api/splits/${id}/share/regenerate`,{method:'POST',credentials:'include'}) }, fx.id)
await ap.goto(`${BASE}/share/${fx.token}`,{waitUntil:'networkidle'})
await ap.waitForTimeout(1200)
const rt=await ap.textContent('body')
log(/isn.t valid|not valid|no longer/i.test(rt),'a revoked token shows a friendly dead-link page')
log(!/log ?in/i.test(rt),'the dead-link page does not offer a login as the fix')
if(shots) await ap.screenshot({path:`${shots}/p6-4-revoked.png`,fullPage:true})

// cleanup
await p.evaluate(async (id)=>{ await fetch(`/api/splits/${id}`,{method:'DELETE',credentials:'include'}) }, fx.id)
console.log('\n  console errors (app):', errs.length?errs.slice(0,4):'none')
console.log('  page errors (anon):', anonErrs.length?anonErrs.slice(0,4):'none')
await b.close()
console.log(fails.length?`\nFAILURES (${fails.length}):\n - `+fails.join('\n - '):'\nAll Phase 6 browser checks passed.')
process.exit(fails.length?1:0)
