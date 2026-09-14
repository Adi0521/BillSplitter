import { chromium } from 'playwright'
const BASE='http://localhost:5173', shots=process.env.SHOTS
const fails=[], log=(ok,m)=>{console.log(`  ${ok?'PASS':'FAIL'}  ${m}`); if(!ok)fails.push(m)}
const b=await chromium.launch({channel:'chrome'})
const p=await b.newPage({viewport:{width:1400,height:1300}})
const errs=[]; p.on('console',m=>{if(m.type()==='error')errs.push(m.text())}); p.on('pageerror',e=>errs.push('pageerror: '+e.message))

await p.goto(BASE+'/login',{waitUntil:'networkidle'})
await p.fill('input[type=email]','a1@example.com'); await p.fill('input[type=password]','hunter2hunter2')
await p.click('button[type=submit]'); await p.waitForURL(u=>!u.pathname.startsWith('/login'))

const splitId = await p.evaluate(async () => {
  const r = await fetch('/api/splits',{method:'POST',headers:{'Content-Type':'application/json'},
    credentials:'include', body:JSON.stringify({name:'ZZ Receipt E2E',type:'one_time',currency:'USD'})})
  return (await r.json()).id
})

await p.goto(`${BASE}/splits/${splitId}/bills/new`,{waitUntil:'networkidle'})
await p.waitForFunction(()=>/receipt|upload|drag/i.test(document.body.innerText),null,{timeout:15000}).catch(()=>{})
let t=await p.textContent('body')
log(/scan a receipt/i.test(t),'the new-bill page offers a receipt upload path')
log(/store or place/i.test(t),'the manual form is the default and still present')
if(shots) await p.screenshot({path:`${shots}/p5-1-manual.png`,fullPage:true})

// Scanning is opt-in: the uploader only exists after this toggle.
await p.locator('button',{hasText:/scan a receipt/i}).first().click()
await p.waitForSelector('input[type=file]',{timeout:15000})
t=await p.textContent('body')
log(/third[- ]party|your own .*server|not (be )?(sent|shared|stored)/i.test(t),
    'it states receipts are not sent to a third party')
if(shots) await p.screenshot({path:`${shots}/p5-1-uploader.png`,fullPage:true})

// upload an HTML receipt whose items deliberately do NOT match the printed total
const html = '<html><head><title>Your online order</title></head><body><h1>SAFEWAY</h1>'
  + '<p>09/01/2026</p><p>MILK 2% 3.49</p><p>SOURDOUGH 4.99</p>'
  + '<p>MFR COUPON -0.50</p><p>TAX 0.52</p><p>TOTAL 12.00</p></body></html>'
const input = p.locator('input[type=file]').first()
log(await input.count()>0,'a file input exists')
await input.setInputFiles({name:'receipt.html', mimeType:'text/html', buffer:Buffer.from(html)})
await p.waitForFunction(()=>/SOURDOUGH/i.test(document.body.innerText),null,{timeout:25000}).catch(()=>{})
t=await p.textContent('body')

// Parsed rows are editable inputs, so their names are in `value`, not textContent.
const values = await p.locator('input').evaluateAll(els => els.map(e => e.value))
log(values.some(v=>/MILK/i.test(v)) && values.some(v=>/SOURDOUGH/i.test(v)),
    'parsed items are shown, in editable fields')
log(values.some(v=>v==='3.49') && values.some(v=>v==='4.99'),
    'prices are prefilled exactly, without rounding artifacts')
log(/SAFEWAY/.test(t),'store name came from the heading, not the page title')
log(!/Your online order/.test(t),'the HTML <title> did not leak in as the store name')
log(/COUPON/i.test(t),'the unmatched coupon line is shown, not hidden')
log(/12\.00/.test(t)&&/8\.48|8,48/.test(t),'both the receipt total and the parsed total are shown side by side')
log(/disagree|do not add|missing|misread/i.test(t),'the mismatch is called out in words')
log(!/NaN|\[object Object\]|undefined/.test(t),'no NaN/undefined leakage')
if(shots) await p.screenshot({path:`${shots}/p5-2-draft.png`,fullPage:true})

// every parsed row must be editable before saving
const editable = await p.locator('input').count()
log(editable >= 4, `parsed rows are editable before saving (${editable} inputs on the page)`)

// a PDF must be refused with a message that names the reason.
// After a successful parse the picker is replaced by the draft editor, so get
// back to it first — that path is worth exercising anyway.
const again = p.locator('button',{hasText:/different receipt|clear|replace/i}).first()
log(await again.count()>0,'a parsed draft can be replaced without reloading')
await again.click()
await p.waitForSelector('input[type=file]',{timeout:15000})
const input2 = p.locator('input[type=file]').first()
await input2.setInputFiles({name:'receipt.pdf', mimeType:'application/pdf', buffer:Buffer.from('%PDF-1.4 not really')})
await p.waitForTimeout(1800)
t=await p.textContent('body')
log(/pdf/i.test(t)&&/not support/i.test(t),'a PDF is refused with a reason naming PDFs')
if(shots) await p.screenshot({path:`${shots}/p5-3-pdf.png`,fullPage:true})

await p.evaluate(async (id)=>{ await fetch(`/api/splits/${id}`,{method:'DELETE',credentials:'include'}) }, splitId)
console.log('\n  console errors:', errs.length?errs.slice(0,4):'none')
await b.close()
console.log(fails.length?`\nFAILURES (${fails.length}):\n - `+fails.join('\n - '):'\nAll Phase 5 browser checks passed.')
process.exit(fails.length?1:0)
