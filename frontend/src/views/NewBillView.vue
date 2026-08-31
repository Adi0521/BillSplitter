<template>
  <AppLayout>
    <div class="p-6 max-w-lg mx-auto">
      <h1 class="text-2xl font-bold mb-1">New Bill</h1>
      <p class="text-sm text-gray-500 mb-6">
        A bill is one receipt — where and when you paid, plus tax, tip and fees.
        You add its line items on the next screen, so the subtotal and total are
        both zero until you do.
      </p>

      <!-- The split is loaded first because the payer dropdown and the currency
           placeholder both come from it. -->
      <div v-if="loading" class="card text-sm text-gray-500">Loading split…</div>

      <div v-else-if="loadError" class="card">
        <p class="text-sm text-red-600">{{ loadError }}</p>
        <div class="mt-4 flex items-center gap-3">
          <button type="button" class="btn-primary" @click="load">Try again</button>
          <button type="button" class="btn-secondary" @click="cancel">Back to split</button>
        </div>
      </div>

      <form v-else class="card space-y-5" novalidate @submit.prevent="submit">
        <p v-if="split" class="text-xs text-gray-500 -mt-1">
          Adding to <span class="font-medium text-gray-700">{{ split.name }}</span>
          · {{ typeLabel(split.type) }} · {{ memberLabel(members.length) }}
        </p>

        <!-- Store -->
        <div>
          <label class="label" for="bill-store">Store or place</label>
          <input
            id="bill-store"
            v-model="storeName"
            type="text"
            class="input"
            maxlength="200"
            placeholder="Safeway"
            autocomplete="off"
            :disabled="saving"
          />
          <p v-if="fieldErrors.storeName" class="mt-1 text-xs text-red-600">
            {{ fieldErrors.storeName }}
          </p>
        </div>

        <!-- Date. type="date" already emits YYYY-MM-DD, which is exactly the
             wire format, so the value is sent through untouched. -->
        <div>
          <label class="label" for="bill-date">Date</label>
          <input
            id="bill-date"
            v-model="date"
            type="date"
            class="input"
            :disabled="saving"
          />
          <p v-if="fieldErrors.date" class="mt-1 text-xs text-red-600">
            {{ fieldErrors.date }}
          </p>
        </div>

        <!-- Currency -->
        <div>
          <label class="label" for="bill-currency">
            Currency <span class="text-gray-400 font-normal">(optional)</span>
          </label>
          <input
            id="bill-currency"
            v-model="currency"
            type="text"
            class="input w-28 uppercase tracking-wider"
            maxlength="3"
            :placeholder="split?.currency || 'USD'"
            autocapitalize="characters"
            autocomplete="off"
            spellcheck="false"
            :disabled="saving"
          />
          <p v-if="fieldErrors.currency" class="mt-1 text-xs text-red-600">
            {{ fieldErrors.currency }}
          </p>
          <p v-else class="mt-1 text-xs text-gray-400">
            Leave blank to use the split's currency ({{ split?.currency || 'USD' }}).
          </p>
        </div>

        <!-- Tax / tip / fees. Kept as text, not type="number": the value is sent
             to the server as the string the user typed, and a number input would
             hand back a float and reintroduce the rounding the string format
             exists to avoid. inputmode="decimal" still gets the numeric keypad. -->
        <div class="grid grid-cols-3 gap-3">
          <div v-for="f in AMOUNT_FIELDS" :key="f.key">
            <label class="label" :for="`bill-${f.key}`">{{ f.label }}</label>
            <input
              :id="`bill-${f.key}`"
              v-model="amounts[f.key]"
              type="text"
              inputmode="decimal"
              class="input"
              maxlength="16"
              placeholder="0.00"
              autocomplete="off"
              :disabled="saving"
            />
            <p v-if="fieldErrors[f.key]" class="mt-1 text-xs text-red-600">
              {{ fieldErrors[f.key] }}
            </p>
          </div>
        </div>
        <p v-if="!anyAmountError" class="-mt-3 text-xs text-gray-400">
          Optional, each defaults to 0. Up to 4 decimal places, no negatives.
        </p>

        <!-- Payer -->
        <div>
          <label class="label" for="bill-payer">
            Who paid <span class="text-gray-400 font-normal">(optional)</span>
          </label>
          <select
            id="bill-payer"
            v-model="payerMemberId"
            class="input"
            :disabled="saving"
          >
            <option value="">Nobody / not decided</option>
            <option v-for="m in members" :key="m.id" :value="m.id">
              {{ optionLabel(m) }}
            </option>
          </select>
          <p v-if="fieldErrors.payerMemberId" class="mt-1 text-xs text-red-600">
            {{ fieldErrors.payerMemberId }}
          </p>
          <p v-else-if="members.length === 0" class="mt-1 text-xs text-gray-400">
            This split has no members yet. Add them on the split page, then edit
            this bill to record who fronted the money.
          </p>
          <p v-else class="mt-1 text-xs text-gray-400">
            You can leave this undecided and set it later.
          </p>
        </div>

        <!-- Whatever the server rejected, verbatim. -->
        <p v-if="error" class="text-sm text-red-600">{{ error }}</p>

        <div class="flex items-center gap-3 pt-1">
          <button type="submit" class="btn-primary" :disabled="saving">
            {{ saving ? 'Creating…' : 'Create bill' }}
          </button>
          <button type="button" class="btn-secondary" :disabled="saving" @click="cancel">
            Cancel
          </button>
        </div>
      </form>
    </div>
  </AppLayout>
</template>

<script setup>
import { computed, onMounted, reactive, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppLayout from '@/components/AppLayout.vue'
import { useBillsStore } from '@/stores/bills'
import { useSplitsStore } from '@/stores/splits'
import { memberLabel, typeLabel } from '@/lib/format'

const AMOUNT_FIELDS = [
  { key: 'tax',  label: 'Tax' },
  { key: 'tip',  label: 'Tip' },
  { key: 'fees', label: 'Fees' },
]

// What the server accepts (verified against POST /api/splits/:id/bills):
// digits, optionally a decimal point and 1-4 digits. ".5", "3.", "1e2", "+1"
// and "1,5" are all 400s, so reject them here rather than round-tripping.
const MONEY_RE = /^\d+(\.\d{1,4})?$/
const MONEY_MAX = 99999999.9999

const route  = useRoute()
const router = useRouter()
const splits = useSplitsStore()
const bills  = useBillsStore()

const splitId = String(route.params.id)

const storeName     = ref('')
const date          = ref(today())
const currency      = ref('')
const payerMemberId = ref('')                                  // '' means "no payer"
const amounts       = reactive({ tax: '0', tip: '0', fees: '0' })

const split     = ref(null)
const members   = ref([])
const loading   = ref(true)
const loadError = ref('')

const saving      = ref(false)
const error       = ref('')      // server-side / transport failure
const submitted   = ref(false)   // only nag inline after a first attempt
const fieldErrors = ref({})

const anyAmountError = computed(() =>
  AMOUNT_FIELDS.some(f => fieldErrors.value[f.key]))

onMounted(load)

async function load() {
  loading.value = true
  loadError.value = ''
  try {
    const data = await splits.fetchSplit(splitId)
    split.value = data
    members.value = data.members ?? []
  } catch (e) {
    // fetchSplit() already normalized this to the backend's {"error": "..."}.
    loadError.value = e.message || 'Could not load this split.'
  } finally {
    loading.value = false
  }
}

// Local date, not toISOString(): west of UTC that would default the picker to
// tomorrow for most of the evening.
function today() {
  const d = new Date()
  const pad = n => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`
}

// Duplicate member names are legal, so lean on the email to tell two Bobs
// apart when there is one.
function optionLabel(m) {
  return m.email ? `${m.name} (${m.email})` : m.name
}

// Currency is normalized as the user types, so what they see is what is sent.
watch(currency, v => {
  const upper = v.toUpperCase()
  if (upper !== v) currency.value = upper
})

// Re-validate live once they've tried to submit, so errors clear as they fix
// them rather than lingering until the next submit.
watch([storeName, date, currency, payerMemberId, () => ({ ...amounts })], () => {
  error.value = ''
  if (submitted.value) validate()
})

// "2026-02-30" matches the shape but is not a date; the server rejects it with
// "date must be a calendar date in YYYY-MM-DD form", so catch it here too.
function isCalendarDate(s) {
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(s)
  if (!m) return false
  const [y, mo, d] = m.slice(1).map(Number)
  if (mo < 1 || mo > 12 || d < 1) return false
  const dt = new Date(Date.UTC(y, mo - 1, d))
  return dt.getUTCFullYear() === y && dt.getUTCMonth() === mo - 1 && dt.getUTCDate() === d
}

// Mirrors the server's rules in docs/api.md → Phase 3 → Bills → Validation.
function validate() {
  const errors = {}

  const trimmedStore = storeName.value.trim()
  if (!trimmedStore) {
    errors.storeName = 'Where was this bill from?'
  } else if (trimmedStore.length > 200) {
    errors.storeName = 'Store name must be 200 characters or fewer.'
  }

  const trimmedDate = date.value.trim()
  if (!trimmedDate) {
    errors.date = 'Pick the date on the receipt.'
  } else if (!isCalendarDate(trimmedDate)) {
    errors.date = 'Enter a real date in YYYY-MM-DD form.'
  }

  const trimmedCurrency = currency.value.trim()
  if (trimmedCurrency && !/^[A-Za-z]{3}$/.test(trimmedCurrency)) {
    errors.currency = 'Currency must be a 3-letter code, like USD.'
  }

  for (const f of AMOUNT_FIELDS) {
    const raw = amounts[f.key].trim()
    if (!raw) continue                    // blank is fine — it means 0
    if (!MONEY_RE.test(raw)) {
      // A leading "-" is the likeliest way to land here, so name it.
      errors[f.key] = raw.startsWith('-')
        ? `${f.label} cannot be negative.`
        : `${f.label} must be a number like 12.50, with at most 4 decimals.`
    } else if (Number(raw) > MONEY_MAX) {
      // Bounds check only. The string above is what gets sent, never this.
      errors[f.key] = `${f.label} must be at most ${MONEY_MAX}.`
    }
  }

  if (payerMemberId.value && !members.value.some(m => m.id === payerMemberId.value)) {
    errors.payerMemberId = 'Pick someone who is a member of this split.'
  }

  fieldErrors.value = errors
  return Object.keys(errors).length === 0
}

async function submit() {
  submitted.value = true
  error.value = ''
  if (!validate()) return
  if (saving.value) return          // belt and braces against a double submit

  const payload = {
    store_name: storeName.value.trim(),
    date: date.value.trim(),
    // Omitted rather than blank so the server inherits the split's currency.
    currency: currency.value.trim().toUpperCase() || undefined,
    // The API takes "" as "no payer", which is exactly what an unselected
    // dropdown produces, so it is passed through as-is.
    payer_member_id: payerMemberId.value,
  }

  // Amounts go over the wire as the exact strings the user typed. Parsing to a
  // float and re-serializing would be the one thing docs/api.md forbids. Blank
  // becomes "0" because the server rejects an empty string outright.
  for (const f of AMOUNT_FIELDS) {
    payload[f.key] = amounts[f.key].trim() || '0'
  }

  saving.value = true
  try {
    const bill = await bills.createBill(splitId, payload)
    if (bill?.id) {
      router.push({ name: 'BillDetail', params: { splitId, billId: bill.id } })
    } else {
      // Created, but nothing to navigate to — fall back to the split.
      router.push({ name: 'SplitDetail', params: { id: splitId } })
    }
  } catch (e) {
    // createBill() already normalized this to the backend's {"error": "..."}.
    error.value = e.message || 'Could not create the bill. Try again.'
  } finally {
    saving.value = false
  }
}

function cancel() {
  router.push({ name: 'SplitDetail', params: { id: splitId } })
}
</script>
