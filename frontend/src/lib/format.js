// Display formatting shared across views.
//
// These live here rather than in each view because several views need them and
// duplicated copies drift: a fix to the date parsing in one view silently
// leaves the others broken.

const TYPE_LABELS = {
  one_time: 'One-time',
  ongoing:  'Ongoing',
}

// Renders a split's `type` for humans. Unknown values fall back to a generic
// label rather than exposing the raw enum.
export function typeLabel(type) {
  return TYPE_LABELS[type] ?? 'Split'
}

// "1 member" / "3 members". Pass an explicit plural for irregular nouns.
export function pluralize(count, singular, plural = `${singular}s`) {
  const n = Number(count) || 0
  return `${n} ${n === 1 ? singular : plural}`
}

export const memberLabel = (count) => pluralize(count, 'member')
export const itemLabel   = (count) => pluralize(count, 'item')
export const billLabel   = (count) => pluralize(count, 'bill')

const DATE_ONLY = /^\d{4}-\d{2}-\d{2}$/
const HAS_TIME  = /T\d{2}:\d{2}/

// Turns either shape the API returns into something `new Date` accepts:
//
//   "2026-08-30 23:46:44.364639+00"   a TIMESTAMPTZ (splits, members, bills)
//   "2026-08-30"                      a DATE (a bill's own date)
//
// A timestamp is non-ISO in two ways — a space instead of "T", and an offset
// with no minutes ("+00") — and Safari rejects both while V8 rejects the
// second. A bare date needs neither repair, and must not get the offset one:
// the trailing "-30" looks exactly like a negative offset, so repairing it
// unconditionally yields "2026-08-30:00", which is not a date at all. That bug
// silently blanked every bill date. Date-only values are pinned to local
// midnight so they never shift a day across a timezone.
function toDate(raw) {
  if (!raw) return null
  const s = String(raw).trim()
  const iso = DATE_ONLY.test(s)
    ? `${s}T00:00:00`
    : (() => {
        const withT = s.replace(' ', 'T')
        return HAS_TIME.test(withT) ? withT.replace(/([+-]\d{2})$/, '$1:00') : withT
      })()
  const d = new Date(iso)
  return Number.isNaN(d.getTime()) ? null : d
}

// Anything unparseable returns "" so a caller can v-if the label away rather
// than rendering "Invalid Date".
export function formatDate(raw) {
  const d = toDate(raw)
  return d ? d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' }) : ''
}

// Alias kept for readability at call sites dealing with a DATE column.
export const formatDay = formatDate

// Money arrives as a NUMERIC(12,4) string like "65.3200". Four decimals is not
// how currency is read, so trim to two for DISPLAY ONLY — never send the result
// back to the API, and never do arithmetic with it. Totals always come from the
// server (see docs/api.md, "Money representation").
export function formatMoney(raw) {
  if (raw === null || raw === undefined || raw === '') return ''
  const n = Number(raw)
  return Number.isFinite(n) ? n.toFixed(2) : String(raw)
}
