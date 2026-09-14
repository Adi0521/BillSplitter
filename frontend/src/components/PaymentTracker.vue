<template>
  <section class="space-y-4">
    <div class="flex flex-wrap items-baseline justify-between gap-x-4 gap-y-1">
      <h2 class="text-lg font-semibold text-gray-900">Settling up</h2>
      <p v-if="!loading && !loadError && payments.length" class="text-xs text-gray-500">
        {{ pluralize(payments.length, 'payment') }} recorded
      </p>
    </div>

    <!-- 1. Loading -->
    <div v-if="loading" class="space-y-3 rounded-lg border border-gray-200 bg-white p-4" aria-busy="true">
      <p class="sr-only">Loading payments…</p>
      <div class="h-4 w-40 animate-pulse rounded bg-gray-200"></div>
      <div class="h-4 w-2/3 animate-pulse rounded bg-gray-100"></div>
      <div class="h-4 w-1/2 animate-pulse rounded bg-gray-100"></div>
    </div>

    <!-- 2. Load failed. The form is hidden too: recording into a list we
            couldn't read would show the new payment as the only one there. -->
    <div v-else-if="loadError" class="rounded-lg border border-red-200 bg-red-50 px-3 py-2 text-sm">
      <p class="font-medium text-red-800">Couldn’t load payments</p>
      <p class="mt-1 text-red-700">{{ loadError }}</p>
      <button type="button" class="btn-secondary mt-3 px-3 py-1.5 text-xs" @click="load">
        Try again
      </button>
    </div>

    <!-- 3. Loaded -->
    <template v-else>
      <!-- A payment needs a sender and a recipient, so one member (or none)
           makes the form impossible rather than merely empty. -->
      <div
        v-if="memberOptions.length < 2"
        class="rounded-lg border border-gray-200 bg-gray-50 px-4 py-3 text-sm text-gray-600"
      >
        <p class="font-medium text-gray-900">Nobody to pay yet</p>
        <p class="mt-1">
          A payment moves money from one member to another, and this split has
          {{ pluralize(memberOptions.length, 'member') }}. Add another member to
          the split, then come back to record who settled up.
        </p>
      </div>

      <form v-else class="rounded-lg border border-gray-200 bg-gray-50 p-4" @submit.prevent="submit">
        <fieldset :disabled="submitting" class="space-y-4">
          <legend class="sr-only">Record a payment</legend>

          <!-- Members are chosen by id, never by name: a split can hold two
               people called "Bob", so identical labels are disambiguated but
               the value carried is always the id. -->
          <div class="grid gap-3 sm:grid-cols-[1fr_auto_1fr] sm:items-end">
            <div>
              <label class="label" :for="fieldId('from')">Who paid</label>
              <select
                :id="fieldId('from')"
                v-model="fromId"
                class="input"
                :aria-invalid="Boolean(errors.from)"
                @change="clearFeedback"
              >
                <option value="">Select a member…</option>
                <option v-for="m in memberOptions" :key="m.id" :value="m.id">{{ m.label }}</option>
              </select>
            </div>

            <div class="flex justify-center sm:pb-1">
              <button
                type="button"
                class="btn-secondary px-3 py-2 text-xs"
                :disabled="submitting || (!fromId && !toId)"
                title="Swap payer and recipient"
                @click="swap"
              >
                <span aria-hidden="true">⇄</span>
                <span class="sr-only">Swap payer and recipient</span>
              </button>
            </div>

            <div>
              <label class="label" :for="fieldId('to')">Who they paid</label>
              <select
                :id="fieldId('to')"
                v-model="toId"
                class="input"
                :aria-invalid="Boolean(errors.to)"
                @change="clearFeedback"
              >
                <option value="">Select a member…</option>
                <option v-for="m in memberOptions" :key="m.id" :value="m.id">{{ m.label }}</option>
              </select>
            </div>
          </div>

          <p v-if="duplicateNames.length" class="text-xs text-gray-500">
            {{ duplicateNames.length === 1 ? 'Two members share the name' : 'Some members share a name' }}
            {{ duplicateNames.map(n => `“${n}”`).join(', ') }} — they’re numbered in
            the order they joined so you can tell them apart.
          </p>

          <div class="grid gap-3 sm:grid-cols-3">
            <div class="sm:col-span-1">
              <label class="label" :for="fieldId('amount')">Amount</label>
              <input
                :id="fieldId('amount')"
                v-model="amount"
                type="text"
                inputmode="decimal"
                autocomplete="off"
                placeholder="20.00"
                class="input text-right tabular-nums"
                :aria-invalid="Boolean(errors.amount)"
                @input="clearFeedback"
              />
            </div>

            <!-- Free-text with suggestions, not a select: a split can gain a
                 currency the moment someone adds a bill in one. -->
            <div class="sm:col-span-1">
              <label class="label" :for="fieldId('currency')">Currency</label>
              <input
                :id="fieldId('currency')"
                v-model="currency"
                type="text"
                :list="fieldId('currency-options')"
                maxlength="3"
                autocapitalize="characters"
                autocomplete="off"
                spellcheck="false"
                placeholder="USD"
                class="input uppercase"
                :aria-invalid="Boolean(errors.currency)"
                @input="clearFeedback"
              />
              <datalist :id="fieldId('currency-options')">
                <option v-for="c in currencyOptions" :key="c" :value="c" />
              </datalist>
            </div>

            <!-- Also free text: the API takes any string up to 50 characters,
                 and settling up happens over things no enum will predict. -->
            <div class="sm:col-span-1">
              <label class="label" :for="fieldId('method')">
                How <span class="font-normal text-gray-400">(optional)</span>
              </label>
              <input
                :id="fieldId('method')"
                v-model="method"
                type="text"
                :list="fieldId('method-options')"
                maxlength="50"
                autocomplete="off"
                placeholder="venmo, cash, …"
                class="input"
                :aria-invalid="Boolean(errors.method)"
                @input="clearFeedback"
              />
              <datalist :id="fieldId('method-options')">
                <option v-for="m in METHOD_SUGGESTIONS" :key="m" :value="m" />
              </datalist>
            </div>
          </div>

          <div>
            <label class="label" :for="fieldId('notes')">
              Notes <span class="font-normal text-gray-400">(optional)</span>
            </label>
            <textarea
              :id="fieldId('notes')"
              v-model="notes"
              rows="2"
              maxlength="1000"
              class="input"
              placeholder="What this covered, if it isn’t obvious later."
              :aria-invalid="Boolean(errors.notes)"
              @input="clearFeedback"
            ></textarea>
            <p v-if="notes.length > 800" class="mt-1 text-xs" :class="errors.notes ? 'text-red-600' : 'text-gray-500'">
              {{ notes.length }} / 1000 characters
            </p>
          </div>

          <ul v-if="visibleErrors.length" class="space-y-1">
            <li v-for="e in visibleErrors" :key="e.field" class="text-xs text-red-600">{{ e.message }}</li>
          </ul>

          <p v-if="submitError" class="rounded-lg border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700">
            {{ submitError }}
          </p>

          <div class="flex flex-wrap items-center gap-3">
            <button type="submit" class="btn-primary" :disabled="submitting">
              {{ submitting ? 'Recording…' : 'Record payment' }}
            </button>
            <p v-if="previewLine" class="text-xs text-gray-600">{{ previewLine }}</p>
          </div>

          <p v-if="justSaved" class="text-xs text-gray-500" role="status">{{ justSaved }}</p>
        </fieldset>
      </form>

      <!-- Empty. A payment record is easy to misread as a way to cancel a debt,
           so the empty state says what it actually does. -->
      <div
        v-if="payments.length === 0"
        class="rounded-lg border border-dashed border-gray-300 bg-white px-4 py-6 text-center text-sm"
      >
        <p class="font-medium text-gray-900">No payments recorded yet</p>
        <p class="mx-auto mt-1 max-w-md text-gray-600">
          Recording a payment doesn’t change what anyone owes — it records that
          money actually moved, so the balances stop showing a debt that’s
          already been settled.
          <template v-if="memberOptions.length >= 2">
            Use the form above once someone pays someone back.
          </template>
        </p>
      </div>

      <!-- Newest first, exactly as the API returns them. -->
      <ul v-else class="divide-y divide-gray-200 overflow-hidden rounded-lg border border-gray-200 bg-white">
        <li v-for="p in payments" :key="p.id" class="px-4 py-3 text-sm">
          <div class="flex flex-wrap items-baseline justify-between gap-x-4 gap-y-1">
            <p class="min-w-0 break-words text-gray-900">
              <span class="font-medium">{{ p.from_name || 'Someone' }}</span>
              paid
              <span class="font-medium">{{ p.to_name || 'someone' }}</span>{{ ' ' }}
              <span class="tabular-nums font-medium">
                {{ p.currency }} {{ formatMoney(p.amount) }}
              </span>
            </p>
            <button
              v-if="confirmingId !== p.id"
              type="button"
              class="shrink-0 text-xs font-medium text-gray-500 underline-offset-2 hover:text-red-700 hover:underline disabled:opacity-50"
              :disabled="Boolean(deletingId)"
              @click="askDelete(p.id)"
            >
              Delete
            </button>
          </div>

          <p class="mt-0.5 flex flex-wrap items-center gap-x-2 gap-y-0.5 text-xs text-gray-500">
            <span v-if="formatDate(p.paid_at)">{{ formatDate(p.paid_at) }}</span>
            <span v-if="p.method" class="rounded bg-gray-100 px-1.5 py-0.5 text-gray-600">{{ p.method }}</span>
          </p>

          <p v-if="p.notes" class="mt-1 whitespace-pre-wrap break-words text-xs text-gray-600">{{ p.notes }}</p>

          <!-- Two-step inline confirm. Deleting is not an "undo": the debt is
               unaffected, only the record of the transfer disappears. -->
          <div v-if="confirmingId === p.id" class="mt-2 rounded-lg border border-red-200 bg-red-50 px-3 py-2">
            <p class="text-xs text-red-800">
              Delete this record? It doesn’t reverse the payment or change what
              anyone owes — it removes the note that
              {{ p.from_name || 'someone' }} paid {{ p.to_name || 'someone' }}
              {{ p.currency }} {{ formatMoney(p.amount) }}, so that amount will
              go back to counting as unsettled.
            </p>
            <p v-if="deleteError.id === p.id" class="mt-2 text-xs text-red-700">{{ deleteError.message }}</p>
            <div class="mt-2 flex flex-wrap gap-2">
              <button
                type="button"
                class="btn-secondary border-red-300 px-3 py-1.5 text-xs text-red-700 hover:bg-red-50"
                :disabled="Boolean(deletingId)"
                @click="confirmDelete(p.id)"
              >
                {{ deletingId === p.id ? 'Deleting…' : 'Yes, delete the record' }}
              </button>
              <button
                type="button"
                class="btn-secondary px-3 py-1.5 text-xs"
                :disabled="Boolean(deletingId)"
                @click="cancelDelete"
              >
                Keep it
              </button>
            </div>
          </div>
        </li>
      </ul>
    </template>
  </section>
</template>

<script setup>
import { ref, computed, watch, onMounted } from 'vue'
import { storeToRefs } from 'pinia'
import { usePaymentsStore } from '@/stores/payments'
import { formatMoney, formatDate, pluralize } from '@/lib/format'

const props = defineProps({
  splitId:         { type: String, required: true },
  members:         { type: Array,  default: () => [] },   // [{ id, name }]
  currencies:      { type: Array,  default: () => [] },   // currencies already in use here
  defaultCurrency: { type: String, default: '' },
})

// Emitted after a successful record or delete, never after a failure: a payment
// moves every balance in the split, so the parent refetches the summary. Firing
// on a failed write would make the parent redraw numbers that didn't change.
const emit = defineEmits(['changed'])

const store = usePaymentsStore()
const { payments } = storeToRefs(store)

const METHOD_SUGGESTIONS = [
  'venmo', 'cash', 'zelle', 'paypal', 'bank transfer', 'apple pay', 'revolut', 'check',
]

const loading   = ref(true)
const loadError = ref('')

const fromId   = ref('')
const toId     = ref('')
const amount   = ref('')
const currency = ref('')
const method   = ref('')
const notes    = ref('')

const submitting  = ref(false)
const submitError = ref('')
const justSaved   = ref('')
const submitted   = ref(false)   // field errors stay quiet until the first attempt

const confirmingId = ref('')
const deletingId   = ref('')
const deleteError  = ref({ id: '', message: '' })

// ── Members ─────────────────────────────────────────────────────────────────

// Names are not identities. Two members really can both be "Bob", so the option
// value is always the id and duplicates get a positional suffix — otherwise the
// two selects show two indistinguishable rows and the choice is a coin flip.
const memberOptions = computed(() => {
  const counts = new Map()
  for (const m of props.members) counts.set(m.name, (counts.get(m.name) ?? 0) + 1)
  const seen = new Map()
  return props.members.map(m => {
    const n = (seen.get(m.name) ?? 0) + 1
    seen.set(m.name, n)
    const name = m.name || 'Unnamed member'
    return {
      id: m.id,
      name,
      label: counts.get(m.name) > 1 ? `${name} (#${n})` : name,
    }
  })
})

const duplicateNames = computed(() => {
  const counts = new Map()
  for (const m of props.members) counts.set(m.name, (counts.get(m.name) ?? 0) + 1)
  return [...counts.entries()].filter(([, c]) => c > 1).map(([name]) => name || 'Unnamed member')
})

const labelFor = (id) => memberOptions.value.find(m => m.id === id)?.label ?? ''

const currencyOptions = computed(() => {
  const seen = new Set()
  const out = []
  for (const c of [props.defaultCurrency, ...props.currencies]) {
    const code = String(c ?? '').trim().toUpperCase()
    if (code && !seen.has(code)) { seen.add(code); out.push(code) }
  }
  return out
})

// ── Validation, mirroring docs/api.md "Phase 6 — Payments" ──────────────────
// The server stays the authority; these exist so the mistakes people actually
// make (paying yourself, five decimal places) don't cost a round trip.

const AMOUNT_RE   = /^(\d+(\.\d+)?|\.\d+)$/
const CURRENCY_RE = /^[A-Za-z]{3}$/
const MAX_INT_DIGITS = 8      // 99999999.9999 is the ceiling

// Deliberately string-only: no parseFloat anywhere near a value that is about
// to be POSTed. "> 0" is "has a non-zero digit", not a numeric comparison.
function amountProblem(raw) {
  const s = raw.trim()
  if (!s) return 'Enter the amount that was paid.'
  if (!AMOUNT_RE.test(s)) {
    return 'Amount must be a plain number like 20 or 20.50 — no currency symbols, commas or minus signs.'
  }
  const [int = '', dec = ''] = s.split('.')
  if (dec.length > 4) return 'Amount can have at most 4 decimal places.'
  if (!/[1-9]/.test(int + dec)) return 'Amount must be greater than 0 — a zero payment doesn’t move anything.'
  if (int.replace(/^0+/, '').length > MAX_INT_DIGITS) return 'Amount must be at most 99999999.9999.'
  return ''
}

const errors = computed(() => {
  const out = {}
  if (!fromId.value) out.from = 'Choose who made the payment.'
  if (!toId.value) out.to = 'Choose who received the payment.'
  if (fromId.value && toId.value && fromId.value === toId.value) {
    out.to = `${labelFor(fromId.value)} can’t pay themselves — pick the member who received the money. ` +
      'A self-payment would move no debt and the server rejects it.'
  }
  const amountErr = amountProblem(amount.value)
  if (amountErr) out.amount = amountErr
  const cur = currency.value.trim()
  if (cur && !CURRENCY_RE.test(cur)) out.currency = 'Currency must be a 3-letter code, like USD.'
  if (method.value.trim().length > 50) out.method = 'How it was paid must be at most 50 characters.'
  if (notes.value.length > 1000) out.notes = 'Notes must be at most 1000 characters.'
  return out
})

const FIELD_ORDER = ['from', 'to', 'amount', 'currency', 'method', 'notes']

// Picking the same member twice is unambiguous and is the whole reason this
// check exists, so it surfaces immediately; the rest wait for a submit attempt
// so the form isn't shouting at a half-filled field.
const visibleErrors = computed(() =>
  FIELD_ORDER
    .filter(f => errors.value[f] && (submitted.value || (f === 'to' && fromId.value && fromId.value === toId.value)))
    .map(f => ({ field: f, message: errors.value[f] })))

// The typed amount verbatim — formatMoney would round "5.1234" to "5.12" and
// preview something other than what is about to be sent.
const previewLine = computed(() => {
  if (!fromId.value || !toId.value || fromId.value === toId.value) return ''
  if (amountProblem(amount.value)) return ''
  const cur = currency.value.trim().toUpperCase()
  return `${labelFor(fromId.value)} pays ${labelFor(toId.value)} ${cur ? cur + ' ' : ''}${amount.value.trim()}.`
})

function fieldId(kind) {
  return `pay-${kind}-${props.splitId}`
}

// ── Loading ─────────────────────────────────────────────────────────────────

onMounted(load)

// The store's `payments` ref is shared, so a change of split must refetch
// rather than leave another split's records on screen.
watch(() => props.splitId, () => {
  resetForm()
  load()
})

watch(() => props.defaultCurrency, (c) => {
  if (!currency.value) currency.value = String(c ?? '').trim().toUpperCase()
}, { immediate: true })

// A member removed from the split underneath us must not stay selected — the
// id would 400 on submit with a message about "another split".
watch(memberOptions, (opts) => {
  const ids = new Set(opts.map(m => m.id))
  if (fromId.value && !ids.has(fromId.value)) fromId.value = ''
  if (toId.value && !ids.has(toId.value)) toId.value = ''
})

async function load() {
  loading.value = true
  loadError.value = ''
  confirmingId.value = ''
  deleteError.value = { id: '', message: '' }
  try {
    await store.fetchPayments(props.splitId)
  } catch (e) {
    loadError.value = e.message
  } finally {
    loading.value = false
  }
}

// ── Writing ─────────────────────────────────────────────────────────────────

function clearFeedback() {
  submitError.value = ''
  justSaved.value = ''
}

function swap() {
  const from = fromId.value
  fromId.value = toId.value
  toId.value = from
  clearFeedback()
}

function resetForm() {
  fromId.value = ''
  toId.value = ''
  amount.value = ''
  method.value = ''
  notes.value = ''
  currency.value = String(props.defaultCurrency ?? '').trim().toUpperCase()
  submitted.value = false
  submitError.value = ''
  justSaved.value = ''
}

async function submit() {
  // The guard, not just the disabled attribute: a double-submit from the keyboard
  // would otherwise record the same payment twice, and there is nothing in the
  // API to deduplicate it afterwards.
  if (submitting.value) return
  submitted.value = true
  submitError.value = ''
  justSaved.value = ''
  if (Object.keys(errors.value).length) return

  // Trimmed, otherwise byte-for-byte what was typed. Rounding here would store
  // an amount the user never entered.
  const payload = {
    from_member: fromId.value,
    to_member: toId.value,
    amount: amount.value.trim(),
  }
  const cur = currency.value.trim()
  if (cur) payload.currency = cur.toUpperCase()
  const how = method.value.trim()
  if (how) payload.method = how
  const note = notes.value.trim()
  if (note) payload.notes = note

  submitting.value = true
  try {
    const saved = await store.recordPayment(props.splitId, payload)
    justSaved.value =
      `Recorded: ${saved.from_name} paid ${saved.to_name} ${saved.currency} ${formatMoney(saved.amount)}.`
    // The pair and how they paid usually repeat; the amount and the note do not.
    amount.value = ''
    notes.value = ''
    submitted.value = false
    emit('changed')
  } catch (e) {
    submitError.value = e.message
  } finally {
    submitting.value = false
  }
}

function askDelete(id) {
  if (deletingId.value) return
  deleteError.value = { id: '', message: '' }
  confirmingId.value = id
}

function cancelDelete() {
  if (deletingId.value) return
  confirmingId.value = ''
  deleteError.value = { id: '', message: '' }
}

async function confirmDelete(id) {
  if (deletingId.value) return
  deletingId.value = id
  deleteError.value = { id: '', message: '' }
  try {
    await store.deletePayment(props.splitId, id)
    confirmingId.value = ''
    emit('changed')
  } catch (e) {
    deleteError.value = { id, message: e.message }
  } finally {
    deletingId.value = ''
  }
}
</script>
