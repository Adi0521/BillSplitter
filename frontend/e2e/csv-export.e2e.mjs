import { chromium } from 'playwright'
const BASE='http://localhost:5173'
const fails=[], log=(ok,m)=>{console.log(`  ${ok?'PASS':'FAIL'}  ${m}`); if(!ok)fails.push(m)}
const b=await chromium.launch({channel:'chrome'})
const ctx=await b.newContext({acceptDownloads:true})
const p=await ctx.newPage()
await p.goto(BASE+'/login',{waitUntil:'networkidle'})
await p.fill('input[type=email]','a1@example.com'); await p.fill('input[type=password]','hunter2hunter2')
await p.click('button[type=submit]'); await p.waitForURL(u=>!u.pathname.startsWith('/login'))

const sid = await p.evaluate(async () => {
  const j=async(m,u,b)=>(await fetch(u,{method:m,headers:{'Content-Type':'application/json'},credentials:'include',body:b?JSON.stringify(b):undefined})).json()
  const s=await j('POST','/api/splits',{name:'ZZ CSV E2E',type:'one_time',currency:'USD'})
  const mem=await j('GET',`/api/splits/${s.id}/members`)
  const bob=await j('POST',`/api/splits/${s.id}/members`,{name:'Bob'})
  const bill=await j('POST',`/api/splits/${s.id}/bills`,{store_name:'Safeway',date:'2026-08-30'})
  const it=await j('POST',`/api/bills/${bill.id}/items`,{name:'Olive oil',price:'12.50',quantity:2})
  await j('PUT',`/api/bills/${bill.id}/items/${it.id}/allocations`,{mode:'ratio',allocations:[
    {member_id:mem[0].id,ratio:'60'},{member_id:bob.id,ratio:'40'}]})
  await j('POST',`/api/bills/${bill.id}/items`,{name:'Bread',price:'3.25'})   // unallocated
  return s.id
})

await p.goto(`${BASE}/splits/${sid}/summary`,{waitUntil:'networkidle'})
await p.waitForFunction(()=>/balance|owes/i.test(document.body.innerText),null,{timeout:20000})
const link=p.locator('a',{hasText:/export csv/i}).first()
log(await link.count()>0,'the summary page offers a CSV download')

const [dl]=await Promise.all([p.waitForEvent('download',{timeout:20000}), link.click()])
const name=dl.suggestedFilename()
log(/\.csv$/.test(name),`the download is a .csv (${name})`)
const fs=await import('node:fs'); const path=`/tmp/${Date.now()}.csv`
await dl.saveAs(path)
const text=fs.readFileSync(path,'utf8')
const rows=text.trim().split('\r\n')
log(rows[0].startsWith('split,bill_date,store'),'header row is the documented shape')
log(rows.some(r=>/Olive oil.*Alice/.test(r)) && rows.some(r=>/Olive oil.*Bob/.test(r)),
    'a two-way split produces two rows for the same item')
log(rows.some(r=>/Bread,3\.2500,1,3\.2500,,$/.test(r)),
    'the unallocated item still has a row, with empty member and share')
log(/15\.0000/.test(text) && /10\.0000/.test(text),'shares are the server-computed 60/40 of 25.00')
console.log('\n  --- file ---'); rows.forEach(r=>console.log('   ',r))
await p.evaluate(async id=>{await fetch(`/api/splits/${id}`,{method:'DELETE',credentials:'include'})},sid)
await b.close()
console.log(fails.length?`\nFAILURES: ${fails.join(', ')}`:'\nCSV download verified in a real browser.')
process.exit(fails.length?1:0)
