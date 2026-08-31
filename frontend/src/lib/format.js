// Display formatting shared across views.
//
// These live here rather than in each view because three views already need
// them and the summary/bill views will too. Duplicated copies drift: a fix to
// the date parsing in one view silently leaves the others broken.

const TYPE_LABELS = {
  one_time: 'One-time',
  ongoing:  'Ongoing',
}

// Renders a split's `type` for humans. Unknown values fall back to a generic
// label rather than exposing the raw enum.
export function typeLabel(type) {
  return TYPE_LABELS[type] ?? 'Split'
}

export function memberLabel(count) {
  const n = Number(count) || 0
  return `${n} ${n === 1 ? 'member' : 'members'}`
}

// Postgres timestamps arrive as "2026-08-30 23:46:44.364639+00", which is not
// valid ISO 8601 in two separate ways: a space instead of "T", and an offset
// with no minutes. Safari rejects both and V8 rejects the second, so repair
// both before parsing. Anything unparseable returns "" so a caller can v-if the
// label away rather than rendering "Invalid Date".
export function formatDate(raw) {
  if (!raw) return ''
  const iso = String(raw)
    .replace(' ', 'T')
    .replace(/([+-]\d{2})$/, '$1:00')
  const d = new Date(iso)
  if (Number.isNaN(d.getTime())) return ''
  return d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' })
}
