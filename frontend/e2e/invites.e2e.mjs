import { chromium } from 'playwright'
const BASE='http://localhost:5173', shots=process.env.SHOTS
const fails=[], log=(ok,m)=>{console.log(`  ${ok?'PASS':'FAIL'}  ${m}`); if(!ok)fails.push(m)}
const b=await chromium.launch({channel:'chrome'})
const errs=[]
const j=(p)=>async(m,u,body)=>(await p.evaluate(async([m,u,body])=>{const r=await fetch(u,{method:m,headers:{'Content-Type':'application/json'},credentials:'include',body:body?JSON.stringify(body):undefined});return {s:r.status,j:await r.json().catch(()=>null)}},[m,u,body]))

// ── OWNER: create split, add a seat, invite via the real UI ──────────────────
const ownerCtx=await b.newContext(); const o=await ownerCtx.newPage({viewport:{width:1280,height:900}})
o.on('pageerror',e=>errs.push('owner: '+e.message))
await o.goto(BASE+'/login',{waitUntil:'networkidle'})
await o.click('button:has-text("Sign up")')
const ownerEmail=`zz-owner-${Date.now()}@example.invalid`
await o.fill('input[type=text]','Alice').catch(()=>{})
await o.fill('input[type=email]',ownerEmail); await o.fill('input[type=password]','hunter2hunter2')
await o.click('button[type=submit]'); await o.waitForURL(u=>!u.pathname.startsWith('/login'))
const split=(await j(o)('POST','/api/splits',{name:'ZZ Invite E2E',type:'one_time',currency:'USD'})).j
const seat=(await j(o)('POST',`/api/splits/${split.id}/members`,{name:'Bob'})).j
await o.goto(`${BASE}/splits/${split.id}`,{waitUntil:'networkidle'})
await o.waitForFunction(()=>/Members/.test(document.body.innerText),null,{timeout:15000})
let t=await o.textContent('body')
log(/Invite/.test(t),'owner sees an Invite action on the unlinked seat')
await o.locator('button',{hasText:/^Invite$/}).first().click()
await o.waitForFunction(()=>/\/invite\/[0-9a-f]{32}/.test(document.body.innerText)||!!document.querySelector('input[readonly]'),null,{timeout:15000})
const linkInput=o.locator('input[readonly]').first()
const inviteUrl=await linkInput.inputValue()
log(/\/invite\/[0-9a-f]{32}$/.test(inviteUrl),`owner gets a full invite URL (${inviteUrl.slice(-14)})`)
t=await o.textContent('body')
log(/shown once|send it only/i.test(t),'the UI warns the link is shown once and to send it only to that person')
log(/Invite pending/i.test(t),'the seat now shows Invite pending')
log(!/[0-9a-f]{32}.*[0-9a-f]{32}/.test(t.replace(inviteUrl,'')),'the token appears only in the link box, not elsewhere in the member list')
if(shots) await o.screenshot({path:`${shots}/inv-1-owner-invite.png`,fullPage:true})

// ── NEW USER: opens the link SIGNED OUT, must sign up and land back on it ────
const bobCtx=await b.newContext(); const p=await bobCtx.newPage({viewport:{width:390,height:844}})
p.on('pageerror',e=>errs.push('bob: '+e.message))
await p.goto(inviteUrl,{waitUntil:'networkidle'})
log(p.url().includes('/login'),'a signed-out visitor is sent to /login')
log(/redirect=%2Finvite%2F|redirect=\/invite\//.test(p.url()),'…with ?redirect back to the invite')
await p.click('button:has-text("Sign up")')
await p.fill('input[type=text]','Bob').catch(()=>{})
await p.fill('input[type=email]',`zz-bob-${Date.now()}@example.invalid`); await p.fill('input[type=password]','hunter2hunter2')
await p.click('button[type=submit]')
await p.waitForURL(/\/invite\/[0-9a-f]{32}/,{timeout:20000})
log(true,'after signing up, Bob lands back on the invite page')
await p.waitForFunction(()=>/invited you|Join as/i.test(document.body.innerText),null,{timeout:15000})
t=await p.textContent('body')
log(/Alice/.test(t)&&/ZZ Invite E2E/.test(t)&&/Bob/.test(t),'preview names the inviter, the split, and the seat')
// The nav bar shows Bob's OWN email (his logged-in identity). The leak to guard
// against is the INVITER's email, which the API must never send.
log(!t.includes(ownerEmail) && !t.includes(ownerEmail.split('@')[0]),'the inviter email does not appear on the invite page')
if(shots) await p.screenshot({path:`${shots}/inv-2-bob-preview.png`,fullPage:true})
await p.locator('button',{hasText:/join as/i}).first().click()
await p.waitForURL(/\/splits\/[0-9a-f-]{36}$/,{timeout:20000})
log(true,'joining navigates Bob to the split')
await p.waitForFunction(()=>/Members/.test(document.body.innerText),null,{timeout:15000})
t=await p.textContent('body')
log(/invited to this split|only the owner/i.test(t),'Bob sees the member banner')
log(!/^Invite$|New link|Revoke/m.test(t),'Bob sees no owner-only invite controls')
log(/Leave this split/i.test(t),'Bob has a Leave action')
log(/Add a line|Add bill|Bills/i.test(t),'Bob can still see and work on bills')
if(shots) await p.screenshot({path:`${shots}/inv-3-bob-member-view.png`,fullPage:true})

// Bob's split list: Invited tab
await p.goto(BASE+'/',{waitUntil:'networkidle'})
await p.waitForFunction(()=>/Invited/.test(document.body.innerText),null,{timeout:15000})
await p.locator('button,a',{hasText:/^Invited/}).first().click()
await p.waitForTimeout(500)
t=await p.textContent('body')
log(/ZZ Invite E2E/.test(t),'the split appears under Bob\'s Invited tab')
log(/Leave/.test(t)&&!/Archive/.test(t),'Invited row offers Leave, not Archive')
if(shots) await p.screenshot({path:`${shots}/inv-4-bob-invited-tab.png`,fullPage:true})

// the same link must not work twice
const mal=await b.newContext(); const m=await mal.newPage()
await m.goto(BASE+'/login',{waitUntil:'networkidle'}); await m.click('button:has-text("Sign up")')
await m.fill('input[type=email]',`zz-mal-${Date.now()}@example.invalid`); await m.fill('input[type=password]','hunter2hunter2')
await m.click('button[type=submit]'); await m.waitForURL(u=>!u.pathname.startsWith('/login'))
await m.goto(inviteUrl,{waitUntil:'networkidle'})
await m.waitForFunction(()=>/isn.t valid|not valid/i.test(document.body.innerText),null,{timeout:15000}).catch(()=>{})
t=await m.textContent('body')
log(/isn.t valid|not valid/i.test(t),'a replayed link shows the not-valid page to a third user')
log(!/log ?in/i.test(t)||/signed in/i.test(t),'…without offering login as the fix')

// owner's view now shows Linked
await o.reload({waitUntil:'networkidle'}); await o.waitForFunction(()=>/Members/.test(document.body.innerText),null,{timeout:15000})
t=await o.textContent('body')
log(/Linked/.test(t)&&!/Invite pending/.test(t),'owner now sees Bob as Linked, no longer pending')

await j(o)('DELETE',`/api/splits/${split.id}`)
console.log('\n  page errors:', errs.length?errs:'none')
await b.close()
console.log(fails.length?`\nFAILURES (${fails.length}):\n - `+fails.join('\n - '):'\nInvite flow verified end to end in a real browser.')
process.exit(fails.length?1:0)
