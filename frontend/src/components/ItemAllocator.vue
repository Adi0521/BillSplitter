<template>
  <div class="rounded-lg border border-gray-200 bg-gray-50 p-4 text-sm">
    <!-- 1. Loading -->
    <div v-if="loading" class="space-y-3" aria-busy="true">
      <p class="sr-only">Loading who’s on “{{ item.name }}”…</p>
      <div class="h-4 w-40 animate-pulse rounded bg-gray-200"></div>
      <div class="h-4 w-2/3 animate-pulse rounded bg-gray-100"></div>
      <div class="h-4 w-1/2 animate-pulse rounded bg-gray-100"></div>
    </div>

    <!-- 2. Load failed. The editor is hidden rather than shown empty: an empty
            editor is indistinguishable from "nobody is on this item", and
            saving from it would wipe allocations that are actually there. -->
    <div v-else-if="loadError" class="rounded-lg border border-red-200 bg-red-50 px-3 py-2">
      <p class="font-medium text-red-800">Couldn’t load this item’s split</p>
      <p class="mt-1 text-red-700">{{ loadError }}</p>
      <button type="button" class="btn-secondary mt-3 px-3 py-1.5 text-xs" @click="load">
        Try again
      </button>
    </div>

    <!-- 3. Loaded -->
    <template v-else>
      <div class="flex flex-wrap items-baseline justify-between gap-x-4 gap-y-1">
        <h3 class="font-medium text-gray-900">Who’s on this item?</h3>
        <p class="text-xs text-gray-500">
          Splitting the line total, {{ item.currency }} {{ formatMoney(item.line_total) }}
        </p>
      </div>

      <!-- What the server currently holds. These strings are the authority —
           the running total below is only a preview of unsaved edits. -->
      <p class="mt-1 text-xs" :class="serverUnallocated ? 'text-amber-700' : 'text-gray-500'">
        <template v-if="serverSet && serverSet.allocations.length">
          Saved: {{ item.currency }} {{ preciseAmount(serverSet.allocated) }} allocated across
          {{ pluralize(serverSet.allocations.length, 'member') }}<template v-if="serverUnallocated">,
          {{ item.currency }} {{ preciseAmount(serverSet.unallocated) }} left unallocated</template>.
        </template>
        <template v-else>
          Nobody is on this item yet — its whole
          {{ item.currency }} {{ preciseAmount(serverSet?.unallocated ?? item.line_total) }}
          is unallocated. Check a member below, or use Even split.
        </template>
      </p>

      <!-- Mode. Every row in one save shares a mode; the API rejects a mixed
           set with a 400, so this is a property of the item, not of a row. -->
      <fieldset class="mt-4" :disabled="busy">
        <legend class="label">Split by</legend>
        <div class="inline-flex rounded-lg border border-gray-300 bg-white p-0.5">
          <button
            v-for="opt in MODES"
            :key="opt.value"
            type="button"
            class="rounded-md px-3 py-1 text-xs font-medium transition-colors disabled:opacity-50"
            :class="mode === opt.value
              ? 'bg-primary-600 text-white'
              : 'text-gray-600 hover:bg-gray-50'"
            :aria-pressed="mode === opt.value"
            :disabled="busy"
            @click="setMode(opt.value)"
          >
            {{ opt.label }}
          </button>
        </div>
      </fieldset>

      <!-- Members. Keyed by id, never by name: a split can hold two members
           who are both called "Bob", and the id is the identity. -->
      <p v-if="rowMembers.length === 0" class="mt-4 text-gray-500">
        This split has no members yet, so there’s nobody to put on this item. Add
        a member to the split first.
      </p>

      <fieldset v-else class="mt-4" :disabled="busy">
        <legend class="label">Members</legend>
        <ul class="divide-y divide-gray-200 rounded-lg border border-gray-200 bg-white">
          <li v-for="m in rowMembers" :key="m.id" class="flex items-center gap-3 px-3 py-2">
            <input
              :id="fieldId('on', m.id)"
              v-model="draft[m.id].checked"
              type="checkbox"
              class="h-4 w-4 shrink-0 rounded border-gray-300 text-primary-600 focus:ring-primary-500"
              :disabled="busy"
              @change="onToggle(m.id)"
            />
            <label :for="fieldId('on', m.id)" class="min-w-0 flex-1 break-words text-gray-900">
              {{ m.name }}
              <span v-if="m.missing" class="ml-1 text-xs text-amber-700">(no longer in this split)</span>
            </label>

            <div class="w-32 shrink-0">
              <label :for="fieldId('val', m.id)" class="sr-only">
                {{ mode === 'ratio' ? 'Percent' : 'Amount' }} for {{ m.name }}
              </label>
              <div class="relative">
                <input
                  :id="fieldId('val', m.id)"
                  v-model="draft[m.id].value"
                  type="text"
                  inputmode="decimal"
                  :placeholder="mode === 'ratio' ? '33.3333' : '5.00'"
                  class="input py-1.5 text-right"
                  :class="mode === 'ratio' ? 'pr-6' : 'pl-7'"
                  :disabled="busy || !draft[m.id].checked"
                  :aria-invalid="Boolean(rowProblems[m.id])"
                />
                <span
                  class="pointer-events-none absolute inset-y-0 flex items-center text-xs text-gray-400"
                  :class="mode === 'ratio' ? 'right-2' : 'left-2'"
                >
                  {{ mode === 'ratio' ? '%' : item.currency }}
                </span>
              </div>
            </div>
          </li>
        </ul>

        <ul v-if="problemList.length" class="mt-2 space-y-1">
          <li v-for="p in problemList" :key="p.id" class="text-xs text-red-600">{{ p.message }}</li>
        </ul>
      </fieldset>

      <!-- Running total. Preview only, computed in integer arithmetic on 4dp
           scaled values — a JS float sum of "0.1" and "0.2" is exactly the bug
           the string-money contract exists to prevent. Ratio mode counts
           against 100%, amount mode against the line total. -->
      <dl v-if="rowMembers.length" class="mt-4 space-y-1 text-xs">
        <div class="flex justify-between gap-4">
          <dt class="text-gray-600">Allocated so far</dt>
          <dd class="tabular-nums font-medium text-gray-900">{{ previewAllocatedLabel }}</dd>
        </div>
        <div v-if="preview.over" class="flex justify-between gap-4">
          <dt class="font-medium text-red-700">Over-allocated by</dt>
          <dd class="tabular-nums font-medium text-red-700">{{ previewRemainderLabel }}</dd>
        </div>
        <div v-else-if="preview.remainder > 0" class="flex justify-between gap-4">
          <dt class="text-amber-700">Left unallocated</dt>
          <dd class="tabular-nums font-medium text-amber-700">{{ previewRemainderLabel }}</dd>
        </div>
        <div v-else-if="preview.usable" class="flex justify-between gap-4">
          <dt class="text-gray-600">Left unallocated</dt>
          <dd class="tabular-nums font-medium text-gray-900">{{ previewRemainderLabel }}</dd>
        </div>
      </dl>

      <p v-if="preview.over" class="mt-2 text-xs text-red-600">
        {{ mode === 'ratio'
          ? 'Ratios can’t add up to more than 100% — the server rejects this.'
          : `Amounts can’t add up to more than the line total (${item.currency} ${formatMoney(item.line_total)}) — the server rejects this.` }}
      </p>
      <p v-else-if="preview.remainder > 0 && checkedIds.length" class="mt-2 text-xs text-gray-500">
        Leaving part of the line unallocated is allowed — the remainder stays on
        the bill as nobody’s, rather than being quietly handed to one member.
      </p>

      <p v-if="saveError" class="mt-3 rounded-lg border border-red-200 bg-red-50 px-3 py-2 text-red-700">
        {{ saveError }}
      </p>

      <!-- Actions -->
      <div v-if="rowMembers.length" class="mt-4 flex flex-wrap items-center gap-2">
        <!-- Clearing everyone off the item is a real deletion of the saved set,
             so it is confirmed inline rather than happening on a stray click. -->
        <template v-if="confirmingClear">
          <span class="text-xs text-gray-600">Remove everyone from this item?</span>
          <button
            type="button"
            class="btn-secondary border-red-300 px-3 py-1.5 text-xs text-red-700 hover:bg-red-50"
            :disabled="busy"
            @click="submit"
          >
            {{ saving ? 'Clearing…' : 'Yes, clear' }}
          </button>
          <button
            type="button"
            class="btn-secondary px-3 py-1.5 text-xs"
            :disabled="busy"
            @click="confirmingClear = false"
          >
            Cancel
          </button>
        </template>

        <template v-else>
          <button
            type="button"
            class="btn-primary px-3 py-1.5 text-xs"
            :disabled="saveDisabled"
            @click="onSaveClick"
          >
            {{ saveLabel }}
          </button>
          <button
            type="button"
            class="btn-secondary px-3 py-1.5 text-xs"
            :disabled="busy || checkedIds.length === 0"
            @click="runEvenSplit"
          >
            {{ splittingEvenly ? 'Splitting…' : 'Even split' }}
          </button>
          <span v-if="checkedIds.length === 0 && !hasSaved" class="text-xs text-gray-500">
            Check at least one member.
          </span>
          <span v-else-if="checkedIds.length" class="text-xs text-gray-500">
            Even split divides it between the
            {{ pluralize(checkedIds.length, 'member') }} you’ve checked.
          </span>
        </template>
      </div>

      <p v-if="justSaved" class="mt-2 text-xs text-gray-500" role="status">{{ justSaved }}</p>
    </template>
  </div>
</template>

<script setup>
import { ref, reactive, computed, watch, onMounted } from 'vue'
import { useAllocationsStore } from '@/stores/allocations'
import { formatMoney, pluralize } from '@/lib/format'

const props = defineProps({
  billId:  { type: String, required: true },
  item:    { type: Object, required: true },
  members: { type: Array, default: () => [] },
})

// Emitted with the AllocationSet the server returned, after every successful
// write and never after a failed one. The parent uses it to refresh the bill's
// share breakdown, so emitting on failure would show shares that don't exist.
const emit = defineEmits(['changed'])

const allocations = useAllocationsStore()

const MODES = [
  { value: 'ratio',  label: '% Ratio' },
  { value: 'amount', label: '$ Amount' },
]

const loading   = ref(true)
const loadError = ref('')
const serverSet = ref(null)

const mode  = ref('ratio')
const draft = reactive({})        // memberId -> { checked, value } — never keyed by name

const saving          = ref(false)
const splittingEvenly = ref(false)
const saveError       = ref('')
const justSaved       = ref('')
const confirmingClear = ref(false)

const busy = computed(() => saving.value || splittingEvenly.value)

// ── Scaled integer arithmetic ───────────────────────────────────────────────
// Everything crossing the wire is a NUMERIC(12,4) string. To preview a sum
// without floats, each value is parsed to an integer number of 1/10000ths
// ("3.3333" -> 33333) and the integers are added. The largest value the API
// accepts is 99999999.9999 -> 999999999999, well inside Number.MAX_SAFE_INTEGER
// even summed over every member of a split, so these stay exact.

const DP = 4
const NUMERIC_RE = /^(\d+(\.\d+)?|\.\d+)$/
const RATIO_BOUND = 100 * 10 ** DP   // 100.0000

// null for anything that isn't a plain non-negative decimal with at most 4
// places — i.e. exactly the values the server would reject anyway.
function toScaled(raw) {
  const s = String(raw ?? '').trim()
  if (!s || !NUMERIC_RE.test(s)) return null
  const [int = '', dec = ''] = s.split('.')
  if (dec.length > DP) return null
  const n = Number((int || '0') + (dec + '0'.repeat(DP)).slice(0, DP))
  return Number.isSafeInteger(n) ? n : null
}

// Back to a decimal string, trailing zeros trimmed but never below `minDp`.
function fromScaled(n, minDp = 2) {
  const unit = 10 ** DP
  const whole = Math.floor(Math.abs(n) / unit)
  let dec = String(Math.abs(n) % unit).padStart(DP, '0').replace(/0+$/, '')
  if (dec.length < minDp) dec = (dec + '0'.repeat(minDp)).slice(0, minDp)
  return `${n < 0 ? '-' : ''}${whole}${dec ? `.${dec}` : ''}`
}

// `formatMoney` rounds to 2dp for display, which renders the rounding residue
// the contract is built around — a leftover "0.0001" — as "0.00". Remainders
// are the one place that must not happen, so they print at their real
// precision. (If a second view ends up needing this, it belongs in
// lib/format.js rather than being copied.)
function preciseAmount(raw) {
  const scaled = toScaled(raw)
  return scaled === null ? formatMoney(raw) : fromScaled(scaled, 2)
}

// Prefills an input from a stored value without rounding it: "60.0000" -> "60",
// "14.2500" -> "14.25", "0.0001" -> "0.0001".
function editableValue(raw, forMode) {
  const scaled = toScaled(raw)
  if (scaled === null) return String(raw ?? '')
  return fromScaled(scaled, forMode === 'ratio' ? 0 : 2)
}

// ── Rows ────────────────────────────────────────────────────────────────────

// The split's members, plus anyone who holds an allocation but is no longer in
// the list. Dropping the latter would silently delete their share on the next
// save, since PUT is a full replace.
const rowMembers = computed(() => {
  const rows = props.members.map(m => ({ id: m.id, name: m.name, missing: false }))
  const known = new Set(rows.map(r => r.id))
  for (const a of serverSet.value?.allocations ?? []) {
    if (!known.has(a.member_id)) {
      known.add(a.member_id)
      rows.push({ id: a.member_id, name: a.member_name || 'Unknown member', missing: true })
    }
  }
  return rows
})

watch(rowMembers, rows => {
  for (const r of rows) if (!draft[r.id]) draft[r.id] = { checked: false, value: '' }
}, { immediate: true })

const checkedIds = computed(() => rowMembers.value.filter(m => draft[m.id]?.checked).map(m => m.id))

const hasSaved = computed(() => (serverSet.value?.allocations.length ?? 0) > 0)

const serverUnallocated = computed(() => {
  const scaled = toScaled(serverSet.value?.unallocated)
  return scaled !== null && scaled > 0
})

// Mirrors the server's per-field rules (docs/api.md, Phase 4 validation) so the
// usual mistakes surface without a round trip. The server stays the authority.
const rowProblems = computed(() => {
  const out = {}
  for (const m of rowMembers.value) {
    const state = draft[m.id]
    if (!state?.checked) continue
    const raw = String(state.value ?? '').trim()
    const noun = mode.value === 'ratio' ? 'percentage' : 'amount'
    if (!raw) { out[m.id] = `${m.name} needs a ${noun}, or uncheck them.`; continue }
    const scaled = toScaled(raw)
    if (scaled === null) {
      const decimals = (raw.split('.')[1] ?? '').length
      out[m.id] = !NUMERIC_RE.test(raw)
        ? `${m.name}: enter a plain number like ${mode.value === 'ratio' ? '33.33' : '5.00'} — no symbols, no negatives.`
        : decimals > DP
          ? `${m.name}: at most 4 decimal places.`
          : `${m.name}: that number is too large.`
      continue
    }
    if (scaled === 0) {
      out[m.id] = `${m.name}: allocate more than zero, or uncheck them.`
      continue
    }
    if (mode.value === 'ratio' && scaled > RATIO_BOUND) {
      out[m.id] = `${m.name}: a ratio can’t be more than 100%.`
    }
  }
  return out
})

const problemList = computed(() =>
  Object.entries(rowProblems.value).map(([id, message]) => ({ id, message })))

// Sum and remainder, in scaled integers. In ratio mode the bound is 100%; in
// amount mode it is the line total. Ratio mode deliberately does NOT convert
// percentages into money here: turning a ratio into a share is the server's
// rounding rule, and a second implementation of it would disagree by cents.
const preview = computed(() => {
  const bound = mode.value === 'ratio' ? RATIO_BOUND : toScaled(props.item.line_total)
  let allocated = 0
  for (const id of checkedIds.value) {
    const scaled = toScaled(draft[id]?.value)
    if (scaled !== null) allocated += scaled
  }
  if (bound === null) return { usable: false, allocated, remainder: 0, over: false }
  const remainder = bound - allocated
  return { usable: true, allocated, remainder, over: remainder < 0 }
})

const previewAllocatedLabel = computed(() =>
  mode.value === 'ratio'
    ? `${fromScaled(preview.value.allocated, 0)}% of 100%`
    : `${props.item.currency} ${fromScaled(preview.value.allocated, 2)} of ${formatMoney(props.item.line_total)}`)

const previewRemainderLabel = computed(() => {
  const abs = Math.abs(preview.value.remainder)
  return mode.value === 'ratio'
    ? `${fromScaled(abs, 0)}%`
    : `${props.item.currency} ${fromScaled(abs, 2)}`
})

// Over-allocation is a guaranteed 400, so the button is blocked and the reason
// shown. Under-allocation is legitimate and must never block a save.
const saveDisabled = computed(() =>
  busy.value ||
  problemList.value.length > 0 ||
  preview.value.over ||
  (checkedIds.value.length === 0 && !hasSaved.value))

const saveLabel = computed(() => {
  if (saving.value) return 'Saving…'
  if (checkedIds.value.length === 0) return 'Clear allocations'
  return 'Save split'
})

function fieldId(kind, memberId) {
  return `alloc-${kind}-${props.item.id}-${memberId}`
}

// ── Loading ─────────────────────────────────────────────────────────────────

onMounted(load)
// The parent renders one of these per item and may swap the item under it.
watch(() => props.item.id, load)

async function load() {
  loading.value = true
  loadError.value = ''
  saveError.value = ''
  justSaved.value = ''
  confirmingClear.value = false
  try {
    hydrate(await allocations.fetchAllocations(props.billId, props.item.id))
  } catch (e) {
    loadError.value = e.message
  } finally {
    loading.value = false
  }
}

// Fills the editor from an AllocationSet. `mode` is null on a cleared item, in
// which case whichever mode the user had selected is kept.
function hydrate(set) {
  serverSet.value = set
  if (set.mode) mode.value = set.mode
  const held = new Map(set.allocations.map(a => [a.member_id, a]))
  for (const m of rowMembers.value) {
    const a = held.get(m.id)
    draft[m.id] = a
      ? { checked: true, value: editableValue(set.mode === 'ratio' ? a.ratio : a.amount, set.mode) }
      : { checked: false, value: '' }
  }
}

// ── Editing ─────────────────────────────────────────────────────────────────

// A percentage and a dollar amount are not the same number, so the typed values
// are dropped when the mode changes rather than being reinterpreted — 60 as a
// ratio is most of the line, 60 as an amount may be more than all of it.
function setMode(next) {
  if (mode.value === next || busy.value) return
  mode.value = next
  for (const m of rowMembers.value) if (draft[m.id]) draft[m.id].value = ''
  saveError.value = ''
  justSaved.value = ''
  confirmingClear.value = false
}

function onToggle(memberId) {
  saveError.value = ''
  justSaved.value = ''
  confirmingClear.value = false
  if (!draft[memberId].checked) draft[memberId].value = ''
}

function onSaveClick() {
  // Emptying the set deletes rows the user can't see any more once they're
  // unchecked, so it gets the same inline confirm the rest of the app uses.
  if (checkedIds.value.length === 0 && hasSaved.value) {
    saveError.value = ''
    confirmingClear.value = true
    return
  }
  submit()
}

async function submit() {
  if (busy.value || saveDisabled.value) return
  saveError.value = ''
  justSaved.value = ''

  // Trimmed, but otherwise the exact characters typed. parseFloat here would
  // hand the server a rounded value and put the arithmetic back into a double.
  const payload = {
    mode: mode.value,
    allocations: checkedIds.value.map(id => ({
      member_id: id,
      [mode.value]: String(draft[id].value).trim(),
    })),
  }

  saving.value = true
  try {
    const set = await allocations.saveAllocations(props.billId, props.item.id, payload)
    hydrate(set)
    confirmingClear.value = false
    justSaved.value = describe(set, 'Saved')
    emit('changed', set)
  } catch (e) {
    saveError.value = e.message
  } finally {
    saving.value = false
  }
}

async function runEvenSplit() {
  if (busy.value || checkedIds.value.length === 0) return
  saveError.value = ''
  justSaved.value = ''
  splittingEvenly.value = true
  try {
    const set = await allocations.evenSplit(props.billId, props.item.id, checkedIds.value)
    hydrate(set)
    confirmingClear.value = false
    justSaved.value = describe(set, 'Split evenly')
    emit('changed', set)
  } catch (e) {
    saveError.value = e.message
  } finally {
    splittingEvenly.value = false
  }
}

// The numbers in this line come from the response, not from the preview above:
// an even split floors each share to whole cents and a ratio split rounds each
// share to 4 places, so only the server knows what was actually stored and what
// was left over.
function describe(set, verb) {
  const left = toScaled(set.unallocated)
  const remainder = left !== null && left > 0
    ? ` — ${props.item.currency} ${preciseAmount(set.unallocated)} left unallocated`
    : ''
  if (!set.allocations.length) return `${verb}. Nobody is on this item now.`
  return `${verb}: ${props.item.currency} ${preciseAmount(set.allocated)} across ` +
    `${pluralize(set.allocations.length, 'member')}${remainder}.`
}
</script>
