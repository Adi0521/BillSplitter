<template>
  <AppLayout>
    <!-- Widened once a receipt has been parsed: the item review table needs
         more room than the plain form does. -->
    <div class="p-6 mx-auto" :class="draft ? 'max-w-3xl' : 'max-w-lg'">
      <h1 class="text-2xl font-bold mb-1">New Bill</h1>
      <p class="text-sm text-gray-500 mb-6">
        A bill is one receipt — where and when you paid, plus tax, tip and fees.
        Type it in below, or scan the receipt and correct what the parser read.
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

      <template v-else>
        <!-- ── The second way in: scan the receipt ──────────────────────────
             This is an alternative source for the same form, never a bypass.
             Parsing writes nothing; every number below still goes through the
             form's validation and the ordinary create endpoints. -->
        <div class="mb-4">
          <button
            v-if="!showUploader"
            type="button"
            class="btn-secondary w-full"
            @click="showUploader = true"
          >
            Scan a receipt instead of typing it
          </button>
          <!-- Locked once a bill exists: parsing a second receipt into a
               half-saved bill would be a mess with no good reading. -->
          <ReceiptUploader
            v-else
            :split-id="splitId"
            :disabled="formLocked"
            @parsed="onParsed"
            @cleared="onCleared"
          />
        </div>

        <!-- What the parser read. Nothing here is saved, and nothing here is
             trusted: it is a proposal the user edits. -->
        <div v-if="draft" class="card mb-4 space-y-4">
          <div>
            <h2 class="font-semibold text-gray-900">What the parser read</h2>
            <p class="mt-1 text-xs text-gray-500">
              From {{ draft.source === 'html' ? 'the HTML receipt' : 'the photo' }}<template
                v-if="draft.store_name"
              >, {{ draft.store_name }}</template><template v-if="draft.date">, dated
              {{ formatDay(draft.date) }}</template>. Amounts are in
              {{ draft.currency }} — the split's currency, which the parser does not guess.
            </p>
          </div>

          <!-- ── The totals cross-check ──────────────────────────────────────
               The most useful thing the parser produces. Both numbers come
               from the server and are shown side by side; neither is adjusted
               to meet the other, and no total is computed here. -->
          <div
            class="rounded-lg border px-3 py-3"
            :class="mismatch
              ? 'border-amber-300 bg-amber-50'
              : 'border-gray-200 bg-gray-50'"
          >
            <p class="font-medium" :class="mismatch ? 'text-amber-900' : 'text-gray-900'">
              <template v-if="mismatch">These two numbers disagree</template>
              <template v-else-if="draft.total_read">The totals agree</template>
              <template v-else>There is no total to check against</template>
            </p>

            <dl class="mt-2 grid grid-cols-2 gap-x-6 gap-y-1 text-sm">
              <dt class="text-gray-600">Parsed items add up to</dt>
              <dd class="text-right font-mono text-gray-900">
                {{ draft.currency }} {{ formatMoney(draft.items_total) }}
              </dd>

              <template v-if="!isZeroAmount(draft.tax)">
                <dt class="text-gray-600">Tax read from the receipt</dt>
                <dd class="text-right font-mono text-gray-900">
                  {{ draft.currency }} {{ formatMoney(draft.tax) }}
                </dd>
              </template>
              <template v-if="!isZeroAmount(draft.tip)">
                <dt class="text-gray-600">Tip read from the receipt</dt>
                <dd class="text-right font-mono text-gray-900">
                  {{ draft.currency }} {{ formatMoney(draft.tip) }}
                </dd>
              </template>

              <dt class="text-gray-600">The receipt says its total is</dt>
              <dd class="text-right font-mono text-gray-900">
                <template v-if="draft.total_read">
                  {{ draft.currency }} {{ formatMoney(draft.total_read) }}
                </template>
                <span v-else class="font-sans text-gray-400">unreadable</span>
              </dd>
            </dl>

            <p v-if="mismatch" class="mt-3 text-xs text-amber-900">
              A line was probably missed or misread. Nothing is adjusted to make
              these match — fix it by editing, adding or removing items below,
              and leave it alone if the receipt itself is the odd one out.
            </p>
            <p v-else-if="!draft.total_read" class="mt-3 text-xs text-gray-500">
              The parser could not read the receipt's own total, so there is nothing
              to cross-check the items against. Check them line by line.
            </p>
            <p v-else class="mt-3 text-xs text-gray-500">
              The items add up to what the receipt claims. That means nothing was
              dropped — it does not mean every name and price was read correctly.
            </p>
          </div>

          <!-- Everything the parser flagged, verbatim. -->
          <div v-if="draft.warnings?.length">
            <h3 class="text-sm font-medium text-gray-900">The parser was unsure about</h3>
            <ul class="mt-1 list-disc space-y-1 pl-5 text-xs text-gray-600">
              <li v-for="(w, i) in draft.warnings" :key="i">{{ w }}</li>
            </ul>
          </div>

          <!-- ── Unmatched lines ─────────────────────────────────────────────
               Shown, always. These are lines the parser refused to guess at,
               and dropping them quietly is how a coupon vanishes. -->
          <div v-if="draft.unmatched_lines?.length">
            <h3 class="text-sm font-medium text-gray-900">
              {{ pluralize(draft.unmatched_lines.length, 'line') }} the parser would not guess at
            </h3>
            <p class="mt-1 text-xs text-gray-500">
              A coupon or discount is the usual reason: an item's price cannot be
              negative, so a “−0.50” line cannot be an item. These are not part of
              the parsed total above. Apply them yourself — as a line of their own,
              or by lowering the price they discount.
            </p>
            <ul class="mt-2 space-y-1">
              <li
                v-for="(line, i) in draft.unmatched_lines"
                :key="i"
                class="flex items-center justify-between gap-3 rounded border border-gray-200 bg-gray-50 px-3 py-1.5"
              >
                <code class="min-w-0 truncate font-mono text-xs text-gray-700">{{ line }}</code>
                <button
                  type="button"
                  class="btn-secondary shrink-0 px-2 py-1 text-xs"
                  :disabled="saving"
                  @click="addRowFromLine(line)"
                >
                  Turn into an item
                </button>
              </li>
            </ul>
          </div>

          <!-- A photo that yields nothing is an ordinary outcome, not an error.
               An empty table would just look broken. -->
          <div
            v-if="draft.items.length === 0"
            class="rounded-lg border border-gray-200 bg-gray-50 px-3 py-3 text-sm"
          >
            <p class="font-medium text-gray-900">No line items could be read</p>
            <p class="mt-1 text-xs text-gray-600">
              That usually means the photo was blurred, angled or cropped. Retake it
              square-on in good light, with the whole receipt in frame, and scan again —
              or fill the bill in by hand and add its items on the next screen.
            </p>
          </div>
        </div>

        <form class="card space-y-5" novalidate @submit.prevent="submit">
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
              :disabled="formLocked"
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
              :disabled="formLocked"
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
              :disabled="formLocked"
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
                :disabled="formLocked"
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
              :disabled="formLocked"
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

          <!-- ── Parsed line items, every one of them editable ─────────────────
               The review step the whole design rests on. Nothing here has been
               saved; each row is created through the ordinary item endpoint,
               with the ordinary rules, only when the user presses the button
               below — and only after they have had every number in front of
               them. -->
          <div v-if="draft" class="border-t border-gray-200 pt-5">
            <div class="flex flex-wrap items-baseline justify-between gap-x-4 gap-y-1">
              <h2 class="font-semibold text-gray-900">Line items</h2>
              <p class="text-xs text-gray-500">
                {{ pluralize(rows.length, 'item') }} will be added to this bill
              </p>
            </div>
            <p class="mt-1 text-xs text-gray-500">
              Check every name and price against the paper. OCR misreads an 8 as a 3
              and drops decimal points, and it does it confidently.
            </p>

            <div v-if="rows.length" class="mt-3 space-y-2">
              <div
                v-for="row in rows"
                :key="row.key"
                class="rounded-lg border px-3 py-2"
                :class="itemErrors[row.key] || isLowConfidence(row)
                  ? 'border-amber-300 bg-amber-50'
                  : 'border-gray-200 bg-white'"
              >
                <div class="flex flex-wrap items-end gap-2">
                  <div class="min-w-[10rem] flex-1">
                    <label class="label text-xs" :for="`item-name-${row.key}`">Item</label>
                    <input
                      :id="`item-name-${row.key}`"
                      v-model="row.name"
                      type="text"
                      class="input"
                      maxlength="200"
                      placeholder="What was it?"
                      autocomplete="off"
                      :disabled="saving"
                    />
                  </div>
                  <div class="w-28">
                    <label class="label text-xs" :for="`item-price-${row.key}`">Price</label>
                    <input
                      :id="`item-price-${row.key}`"
                      v-model="row.price"
                      type="text"
                      inputmode="decimal"
                      class="input font-mono"
                      maxlength="16"
                      placeholder="0.00"
                      autocomplete="off"
                      :disabled="saving"
                    />
                  </div>
                  <div class="w-20">
                    <label class="label text-xs" :for="`item-qty-${row.key}`">Qty</label>
                    <input
                      :id="`item-qty-${row.key}`"
                      v-model="row.quantity"
                      type="text"
                      inputmode="numeric"
                      class="input font-mono"
                      maxlength="6"
                      placeholder="1"
                      autocomplete="off"
                      :disabled="saving"
                    />
                  </div>
                  <button
                    type="button"
                    class="btn-secondary mb-px px-2 py-2 text-xs"
                    :disabled="saving"
                    :aria-label="`Remove ${row.name || 'this item'}`"
                    @click="removeRow(row.key)"
                  >
                    Remove
                  </button>
                </div>

                <div class="mt-1.5 flex flex-wrap items-center gap-x-3 gap-y-1 text-xs">
                  <!-- Confidence is per line and only means something for OCR.
                       An HTML receipt was read as text, so a "100%" badge there
                       would be a number pretending to be evidence. -->
                  <span
                    v-if="showsConfidence && row.confidence !== null"
                    class="rounded px-1.5 py-0.5 font-medium"
                    :class="isLowConfidence(row)
                      ? 'bg-amber-200 text-amber-900'
                      : 'bg-gray-100 text-gray-600'"
                  >
                    {{ isLowConfidence(row) ? 'Hard to read · ' : 'Scanned clearly · '
                    }}{{ confidencePct(row.confidence) }}
                  </span>
                  <span v-if="row.note" class="text-amber-800">{{ row.note }}</span>
                  <span v-if="itemErrors[row.key]" class="font-medium text-red-700">
                    {{ itemErrors[row.key] }}
                  </span>
                </div>
              </div>
            </div>

            <p v-else class="mt-3 rounded-lg border border-gray-200 bg-gray-50 px-3 py-3 text-xs text-gray-600">
              No items are queued. Add one below, or create the bill now and add its
              items on the next screen.
            </p>

            <div class="mt-3 flex flex-wrap items-center gap-3">
              <button
                type="button"
                class="btn-secondary px-3 py-1.5 text-xs"
                :disabled="saving"
                @click="addRow()"
              >
                Add a line
              </button>
              <p v-if="showsConfidence" class="text-xs text-gray-400">
                Confidence is how legibly a line scanned, not whether it is right — a
                crisply printed wrong price scores 100%.
              </p>
              <p v-else class="text-xs text-gray-400">
                This receipt was read as text rather than scanned, so there is no
                confidence to report. The parser can still have misread a line.
              </p>
            </div>
          </div>

          <!-- Whatever the server rejected, verbatim. -->
          <p v-if="error" class="text-sm text-red-600">{{ error }}</p>

          <!-- The bill exists but some of its items did not make it. Creating a
               second bill on a retry would be worse than the original failure,
               so the header fields lock and only the leftover items are sent. -->
          <p v-if="createdBillId" class="text-xs text-amber-800">
            The bill has been created, so its store, date and amounts are locked here.
            Edit them on the bill itself if they need changing.
          </p>

          <div class="flex flex-wrap items-center gap-3 pt-1">
            <button type="submit" class="btn-primary" :disabled="saving">
              {{ submitLabel }}
            </button>
            <button
              v-if="createdBillId"
              type="button"
              class="btn-secondary"
              :disabled="saving"
              @click="openBill"
            >
              Open the bill without them
            </button>
            <button v-else type="button" class="btn-secondary" :disabled="saving" @click="cancel">
              Cancel
            </button>
          </div>
        </form>
      </template>
    </div>
  </AppLayout>
</template>

<script setup>
import { computed, onMounted, reactive, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppLayout from '@/components/AppLayout.vue'
import ReceiptUploader from '@/components/ReceiptUploader.vue'
import { useBillsStore } from '@/stores/bills'
import { useSplitsStore } from '@/stores/splits'
import {
  formatDay, formatMoney, isZeroAmount, memberLabel, pluralize, typeLabel,
} from '@/lib/format'

const AMOUNT_FIELDS = [
  { key: 'tax',  label: 'Tax' },
  { key: 'tip',  label: 'Tip' },
  { key: 'fees', label: 'Fees' },
]

// Below this, a line is worth a second look. It is a legibility score, not a
// correctness one, so a high number is not a reason to skip checking a row.
const LOW_CONFIDENCE = 0.75

// Server rules for an item (docs/api.md → Phase 3 → Items → Validation).
const QUANTITY_RE  = /^\d+$/
const QUANTITY_MAX = 100000
const NAME_MAX     = 200

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

// ── Receipt parsing ─────────────────────────────────────────────────────────
// `draft` is the server's ReceiptDraft, kept verbatim and never mutated: it is
// the record of what the parser claimed, which is what the cross-check panel
// reports. `rows` is the user's editable copy, and it is the only thing that
// can become an item.
const showUploader  = ref(false)
const draft         = ref(null)
const rows          = ref([])
const itemErrors    = ref({})
const createdBillId = ref('')     // set only when a create half-succeeded
let   rowSeq        = 0

const mismatch = computed(() => draft.value ? draft.value.totals_agree === false : false)

// Confidence is a property of OCR. An HTML receipt was read as text, and the
// server sends 1.0 for every line of one; rendering that as a score would be
// inventing evidence.
const showsConfidence = computed(() => draft.value?.source === 'image')

// The header fields describe a bill that already exists once createBill has
// succeeded, so editing them here would be a lie — the retry only sends items.
const formLocked = computed(() => saving.value || !!createdBillId.value)

const submitLabel = computed(() => {
  if (saving.value) return createdBillId.value ? 'Adding…' : 'Creating…'
  if (createdBillId.value) return 'Add remaining items'
  if (rows.value.length) return `Create bill with ${pluralize(rows.value.length, 'item')}`
  return 'Create bill'
})

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

// Same for the item rows. The banner is left alone once a bill exists: it is
// the only record that the bill was created and some items were not.
watch(rows, () => {
  if (!createdBillId.value) error.value = ''
  if (submitted.value) validateItems()
}, { deep: true })

// ── Turning a draft into editable rows ──────────────────────────────────────

// Money arrives as a NUMERIC(12,4) string. Dropping trailing zeros is exact —
// "3.4900" and "3.49" are the same number — so what lands in the field is
// still literally what the parser read, and what is sent is whatever the user
// leaves there. formatMoney is for display only and is deliberately not used
// here: it would turn "2.6049" into "2.60" and that rounded value would then
// be the one POSTed, which is exactly what the contract forbids.
function exactAmount(raw) {
  const s = String(raw ?? '').trim()
  if (!/^\d+\.\d+$/.test(s)) return s
  return s.replace(/0+$/, '').replace(/\.$/, '')
}

function toRow(item) {
  return {
    key: `r${rowSeq++}`,
    name: String(item?.name ?? '').slice(0, NAME_MAX),
    price: exactAmount(item?.price),
    quantity: String(item?.quantity ?? 1),
    confidence: typeof item?.confidence === 'number' ? item.confidence : null,
    // The parser reports a price it could read on a line whose name it could
    // not. The row is kept — the price is real — but it cannot be saved until
    // the user names it.
    note: String(item?.name ?? '').trim() ? '' : 'The parser read a price here but no name.',
  }
}

function isLowConfidence(row) {
  return showsConfidence.value && row.confidence !== null && row.confidence < LOW_CONFIDENCE
}

function confidencePct(c) {
  return `${Math.round((Number(c) || 0) * 100)}%`
}

function addRow(preset = {}) {
  rows.value = [...rows.value, {
    key: `r${rowSeq++}`,
    name: '', price: '', quantity: '1', confidence: null, note: '',
    ...preset,
  }]
}

// An unmatched line is text, not an item — most often a coupon, whose negative
// amount an item can never hold. The line's words are offered as a name and
// nothing more: the price is left blank so the user has to decide it.
function addRowFromLine(line) {
  addRow({
    name: String(line).trim().slice(0, NAME_MAX),
    note: 'From an unmatched line — set a price. An item cannot be negative.',
  })
}

function removeRow(key) {
  rows.value = rows.value.filter(r => r.key !== key)
  const { [key]: _dropped, ...rest } = itemErrors.value
  itemErrors.value = rest
}

// The draft fills the form in; it does not submit anything. Every field it
// touches is the same field the user would have typed into, and it is still
// validated by validate() before anything is sent.
function onParsed(received) {
  draft.value = received
  rows.value = (received?.items ?? []).map(toRow)
  itemErrors.value = {}

  const guessedStore = String(received?.store_name ?? '').trim()
  if (guessedStore) storeName.value = guessedStore.slice(0, NAME_MAX)
  if (received?.date) date.value = received.date

  // Only tax and tip are read off a receipt; fees are not, so that field is
  // left as the user set it. A null means the receipt printed no such line,
  // which is not the same as zero — so it is left alone too.
  if (received?.tax != null) amounts.tax = exactAmount(received.tax)
  if (received?.tip != null) amounts.tip = exactAmount(received.tip)

  // Currency is deliberately not filled in: the draft's currency *is* the
  // split's, and a blank field already means "use the split's".
  error.value = ''
  if (submitted.value) { validate(); validateItems() }
}

// Called when the uploader is cleared or a different file is picked. The draft
// and its rows go, because they describe a receipt that is no longer on
// screen. The header fields stay: the user may have corrected them by hand,
// and silently reverting that would be its own small betrayal.
function onCleared() {
  draft.value = null
  rows.value = []
  itemErrors.value = {}
}

// Mirrors the server's item rules so a bad row is caught before a bill has
// been created for it.
function validateItems() {
  const errors = {}
  for (const row of rows.value) {
    const name  = row.name.trim()
    const price = row.price.trim()
    const qty   = row.quantity.trim() || '1'

    if (!name) {
      errors[row.key] = 'Give this line a name.'
    } else if (name.length > NAME_MAX) {
      errors[row.key] = `Name must be ${NAME_MAX} characters or fewer.`
    } else if (!price) {
      errors[row.key] = 'Enter the price on the receipt.'
    } else if (!MONEY_RE.test(price)) {
      errors[row.key] = price.startsWith('-')
        ? 'A price cannot be negative. A coupon has to come off the item it discounts.'
        : 'Price must be a number like 12.50, with at most 4 decimals.'
    } else if (Number(price) > MONEY_MAX) {
      errors[row.key] = `Price must be at most ${MONEY_MAX}.`
    } else if (!QUANTITY_RE.test(qty) || Number(qty) < 1 || Number(qty) > QUANTITY_MAX) {
      errors[row.key] = `Quantity must be a whole number from 1 to ${QUANTITY_MAX}.`
    }
  }
  itemErrors.value = errors
  return Object.keys(errors).length === 0
}

function openBill() {
  if (!createdBillId.value) return
  router.push({ name: 'BillDetail', params: { splitId, billId: createdBillId.value } })
}

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
  // Both halves are validated before either is sent, so a bad item cannot
  // leave a bill behind with nothing on it.
  const formOk  = createdBillId.value ? true : validate()
  const itemsOk = validateItems()
  if (!formOk || !itemsOk) return
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
    // A retry after a partial failure must not create a second bill, so the
    // id of the one already created wins over making another.
    let billId = createdBillId.value
    if (!billId) {
      const bill = await bills.createBill(splitId, payload)
      billId = bill?.id ?? ''
      createdBillId.value = billId
    }
    if (!billId) {
      // Created, but nothing to navigate to — fall back to the split.
      router.push({ name: 'SplitDetail', params: { id: splitId } })
      return
    }

    // Items go one at a time and in order: the API orders them by created_at,
    // and firing them in parallel would shuffle the receipt's own order. Each
    // one that lands is dropped from `rows` immediately, so a retry can never
    // add it twice.
    const failures = []
    for (const row of [...rows.value]) {
      try {
        await bills.addItem(splitId, billId, {
          name: row.name.trim(),
          // The exact string in the field, trimmed. Never parseFloat.
          price: row.price.trim(),
          quantity: Number(row.quantity.trim() || '1'),
        })
        rows.value = rows.value.filter(r => r.key !== row.key)
      } catch (e) {
        failures.push([row.key, e.message || 'This item could not be added.'])
      }
    }

    if (failures.length) {
      itemErrors.value = Object.fromEntries(failures)
      error.value =
        `The bill was created, but ${pluralize(failures.length, 'item')} could not be ` +
        'added. Fix the lines marked below and press “Add remaining items” — the ' +
        'ones that already saved will not be added twice.'
      return
    }

    router.push({ name: 'BillDetail', params: { splitId, billId } })
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
