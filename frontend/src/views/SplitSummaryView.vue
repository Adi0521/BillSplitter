<template>
  <AppLayout>
    <div class="p-6 max-w-4xl mx-auto">
      <router-link
        :to="`/splits/${splitId}`"
        class="text-sm text-gray-500 hover:text-gray-700"
      >
        &larr; {{ splitName ? `Back to ${splitName}` : 'Back to the split' }}
      </router-link>

      <!-- 1. Loading -->
      <div v-if="loading" class="mt-4 space-y-4" aria-busy="true">
        <p class="sr-only">Working out who owes what…</p>
        <div class="card animate-pulse">
          <div class="h-6 w-1/3 rounded bg-gray-200"></div>
          <div class="mt-3 h-4 w-1/2 rounded bg-gray-100"></div>
        </div>
        <div class="card animate-pulse">
          <div class="h-5 w-24 rounded bg-gray-200"></div>
          <div class="mt-4 h-4 w-2/3 rounded bg-gray-100"></div>
          <div class="mt-3 h-4 w-1/2 rounded bg-gray-100"></div>
          <div class="mt-3 h-4 w-3/5 rounded bg-gray-100"></div>
        </div>
      </div>

      <!-- 2a. Not found — the id doesn't exist, or the split isn't this user's.
              The API answers 404 for both so it can't be used to probe which
              splits exist. -->
      <div v-else-if="notFound" class="card mt-4 text-center py-12">
        <h1 class="text-lg font-medium text-gray-900">Split not found</h1>
        <p class="mt-2 text-sm text-gray-500 max-w-sm mx-auto">
          This split either doesn’t exist or isn’t yours. If you archived it, it
          still exists — open it from the archived list rather than this link.
        </p>
        <div class="mt-6 flex justify-center gap-2">
          <router-link to="/" class="btn-primary">All splits</router-link>
        </div>
      </div>

      <!-- 2b. Any other failure -->
      <div v-else-if="error" class="card mt-4 border-red-200 bg-red-50">
        <h1 class="text-sm font-medium text-red-800">Couldn’t load this summary</h1>
        <p class="mt-1 text-sm text-red-700">{{ error }}</p>
        <div class="mt-4 flex gap-2">
          <button type="button" class="btn-primary" :disabled="loading" @click="load">
            Try again
          </button>
          <router-link :to="`/splits/${splitId}`" class="btn-secondary">
            Back to the split
          </router-link>
        </div>
      </div>

      <!-- 3. Loaded -->
      <template v-else-if="summary">
        <!-- Header -->
        <div class="card mt-4">
          <div class="flex items-start justify-between gap-4">
            <div class="min-w-0 flex-1">
              <h1 class="text-2xl font-bold break-words">{{ summary.name }}</h1>
              <p class="mt-1 text-sm text-gray-600">
                Who owes what across every bill in this split, net of who fronted
                the money and what has already been paid back.
              </p>
            </div>
            <span class="shrink-0 rounded-full bg-gray-100 text-gray-700 px-2 py-0.5 text-xs font-medium">
              {{ summary.base_currency }}
            </span>
          </div>

          <div class="mt-4 flex flex-wrap items-center gap-2">
            <button
              type="button"
              class="btn-secondary text-xs px-3 py-1.5"
              :disabled="refreshing"
              @click="refresh"
            >
              {{ refreshing ? 'Refreshing…' : 'Refresh' }}
            </button>
            <router-link :to="`/splits/${splitId}`" class="btn-secondary text-xs px-3 py-1.5">
              Bills &amp; members
            </router-link>
          </div>

          <p v-if="refreshError" class="mt-3 text-xs text-amber-700">
            Couldn’t refresh ({{ refreshError }}). The figures below are from the
            last successful load and may be out of date.
          </p>
          <p v-if="membersError" class="mt-3 text-xs text-amber-700">
            Couldn’t load this split’s member list ({{ membersError }}), so
            recording a payment is unavailable until it loads.
          </p>
          <p v-if="billsError" class="mt-3 text-xs text-amber-700">
            Couldn’t load this split’s bills ({{ billsError }}), so the per-currency
            sections can’t link to them. The balances themselves are unaffected.
          </p>
        </div>

        <!-- Mixed currencies. Stated once, up top, because it changes how every
             section below has to be read: there is no grand total on this page
             and there deliberately isn't one. -->
        <div
          v-if="summary.mixed_currency"
          class="card mt-4 border-amber-200 bg-amber-50"
        >
          <h2 class="text-sm font-medium text-amber-900">
            This split holds {{ currencyCount }}
          </h2>
          <p class="mt-1 text-sm text-amber-800">
            {{ currencyList }} are tracked completely separately and are
            <strong>not converted</strong>. Each section below is its own set of
            balances that only settles within that currency, and nothing on this
            page adds two currencies together — that would invent an exchange
            rate nobody chose. Currency conversion is a later phase.
          </p>
        </div>

        <!-- Empty: no bills and no payments, so the server sends no currency
             blocks at all rather than a fabricated zero row. -->
        <div v-if="summary.by_currency.length === 0" class="card mt-4 text-center py-12">
          <h2 class="text-lg font-medium text-gray-900">Nothing to split yet</h2>
          <p class="mt-2 text-sm text-gray-500 max-w-md mx-auto">
            This split has no bills and no recorded payments, so nobody owes
            anybody anything. Add a bill, put its items against the people who
            had them, and the balances will appear here.
          </p>
          <div class="mt-6 flex justify-center gap-2">
            <router-link :to="`/splits/${splitId}/bills/new`" class="btn-primary">
              Add the first bill
            </router-link>
            <router-link :to="`/splits/${splitId}`" class="btn-secondary">
              Back to the split
            </router-link>
          </div>
        </div>

        <!-- One section per currency. Never a combined one. -->
        <section
          v-for="block in summary.by_currency"
          :key="block.currency"
          class="card mt-4"
        >
          <div class="flex flex-wrap items-baseline justify-between gap-x-4 gap-y-2">
            <h2 class="text-lg font-semibold">
              {{ block.currency }}
              <span
                v-if="block.currency === summary.base_currency"
                class="ml-1 align-middle rounded-full bg-primary-50 px-2 py-0.5 text-xs font-medium text-primary-700"
              >
                split currency
              </span>
            </h2>
            <p class="text-sm text-gray-500">
              <span class="tabular-nums font-medium text-gray-900">
                {{ block.currency }} {{ formatMoney(block.total) }}
              </span>
              across {{ billLabel(block.bill_count) }}
            </p>
          </div>

          <!-- A currency with money moved but no bills behind it. The union of
               bill and payment currencies is what produces this block; dropping
               it would leave a real payment in no section at all. -->
          <p
            v-if="Number(block.bill_count) === 0"
            class="mt-3 rounded-lg bg-gray-50 px-3 py-2 text-sm text-gray-600"
          >
            No bill in this split is in {{ block.currency }} — these balances come
            entirely from payments recorded in {{ block.currency }}. That is why
            the total is {{ formatMoney(block.total) }}: no bills, but money moved.
          </p>

          <!-- Which bills make up this currency. Purely a convenience link;
               absent if the bills request failed. -->
          <p v-else-if="billsFor(block.currency).length" class="mt-3 text-sm text-gray-500">
            <span
              v-for="(b, i) in billsFor(block.currency)"
              :key="b.id"
            >
              <span v-if="i > 0">, </span>
              <router-link
                :to="`/splits/${splitId}/bills/${b.id}`"
                class="text-primary-600 hover:text-primary-700"
              >{{ b.store_name }}</router-link>
              <span v-if="formatDay(b.date)" class="text-gray-400">
                ({{ formatDay(b.date) }})</span>
            </span>
          </p>

          <!-- Unallocated. Shown whenever it is non-zero, never folded into a
               member and never rounded away: an under-allocated split has to
               look under-allocated. -->
          <div
            v-if="!isZeroAmount(block.unallocated.total)"
            class="mt-4 rounded-lg border border-amber-200 bg-amber-50 px-3 py-3"
          >
            <p class="text-sm font-medium text-amber-900">
              {{ block.currency }} {{ formatMoney(block.unallocated.total) }} is
              allocated to nobody
            </p>
            <p class="mt-1 text-sm text-amber-800">
              These are line items nobody has been assigned, plus their share of
              tax, tip and fees. Nobody is on the hook for this amount, so it is
              missing from every balance below, and whoever fronted it is still out
              of pocket for it. Open the bills and allocate the
              remaining items if that isn’t deliberate.
            </p>
            <dl class="mt-2 flex flex-wrap gap-x-5 gap-y-1 text-xs text-amber-900">
              <div><dt class="inline">Items</dt>
                <dd class="inline tabular-nums font-medium ml-1">
                  {{ formatMoney(block.unallocated.items) }}</dd></div>
              <div><dt class="inline">Tax</dt>
                <dd class="inline tabular-nums font-medium ml-1">
                  {{ formatMoney(block.unallocated.tax) }}</dd></div>
              <div><dt class="inline">Tip</dt>
                <dd class="inline tabular-nums font-medium ml-1">
                  {{ formatMoney(block.unallocated.tip) }}</dd></div>
              <div><dt class="inline">Fees</dt>
                <dd class="inline tabular-nums font-medium ml-1">
                  {{ formatMoney(block.unallocated.fees) }}</dd></div>
            </dl>
          </div>

          <!-- Balances. Six money columns don't fit a phone; scrolling the table
               beats dropping a column someone needs. -->
          <p
            v-if="block.members.length === 0"
            class="mt-4 rounded-lg bg-gray-50 px-3 py-3 text-sm text-gray-600"
          >
            This split has no members, so there is nobody to hold a balance.
            <router-link
              :to="`/splits/${splitId}`"
              class="font-medium text-primary-600 hover:text-primary-700"
            >Add members to the split</router-link>
            first.
          </p>

          <div v-else class="mt-4 overflow-x-auto">
            <table class="w-full text-sm">
              <caption class="sr-only">
                {{ block.currency }} balances per member
              </caption>
              <thead>
                <tr class="border-b border-gray-200 text-left text-xs uppercase tracking-wide text-gray-500">
                  <th scope="col" class="pb-2 font-medium">Member</th>
                  <th scope="col" class="pb-2 pl-3 text-right font-medium">Owes</th>
                  <th scope="col" class="pb-2 pl-3 text-right font-medium">Fronted</th>
                  <th scope="col" class="pb-2 pl-3 text-right font-medium">Paid</th>
                  <th scope="col" class="pb-2 pl-3 text-right font-medium">Received</th>
                  <th scope="col" class="pb-2 pl-3 text-right font-medium">Balance</th>
                  <th scope="col" class="pb-2 pl-3 font-medium">Meaning</th>
                </tr>
              </thead>
              <tbody class="divide-y divide-gray-100">
                <!-- Keyed by member_id, never by name: two members of one split
                     may legitimately share a name. A member with nothing but
                     zeros still gets a row — "Carol is square" is information. -->
                <tr v-for="m in block.members" :key="m.member_id">
                  <td class="py-2 pr-3">
                    <span class="font-medium text-gray-900 break-words">
                      {{ displayName(block, m) }}
                    </span>
                  </td>
                  <td class="py-2 pl-3 text-right tabular-nums text-gray-600">
                    {{ formatMoney(m.owes) }}
                  </td>
                  <td class="py-2 pl-3 text-right tabular-nums text-gray-600">
                    {{ formatMoney(m.fronted) }}
                  </td>
                  <td class="py-2 pl-3 text-right tabular-nums text-gray-600">
                    {{ formatMoney(m.payments_made) }}
                  </td>
                  <td class="py-2 pl-3 text-right tabular-nums text-gray-600">
                    {{ formatMoney(m.payments_received) }}
                  </td>
                  <td
                    class="py-2 pl-3 text-right tabular-nums font-medium"
                    :class="balanceTone(m.balance)"
                  >
                    {{ formatMoney(m.balance) }}
                  </td>
                  <!-- The sign in words. Colour alone can't carry this: a minus
                       sign is easy to miss and impossible to read aloud. -->
                  <td class="py-2 pl-3 text-xs" :class="balanceTone(m.balance)">
                    {{ balanceMeaning(block, m) }}
                  </td>
                </tr>
              </tbody>
            </table>
          </div>

          <p v-if="block.members.length" class="mt-2 text-xs text-gray-400">
            Balance is <span class="tabular-nums">fronted + paid − owes − received</span>,
            computed by the server. A positive balance means the split owes that
            member; a negative one means they owe the split. Because shares are
            rounded and unallocated amounts belong to nobody, the balances need
            not add up to zero — the difference is reported rather than papered
            over, and nothing on this page is a client-side sum.
          </p>

          <!-- Settlements -->
          <div v-if="block.members.length" class="mt-5 border-t border-gray-100 pt-4">
            <h3 class="text-sm font-semibold text-gray-900">
              {{ block.settlements.length
                ? `One way to settle up in ${block.currency}`
                : `Settling up in ${block.currency}` }}
            </h3>

            <ul v-if="block.settlements.length" class="mt-2 space-y-1 text-sm">
              <li v-for="(s, i) in block.settlements" :key="`${s.from_member}-${s.to_member}-${i}`" class="text-gray-800">
                <span class="font-medium">{{ settlementName(block, s.from_member, s.from_name) }}</span>
                pays
                <span class="font-medium">{{ settlementName(block, s.to_member, s.to_name) }}</span>{{ ' ' }}
                <span class="tabular-nums font-medium">
                  {{ block.currency }} {{ formatMoney(s.amount) }}</span>.
              </li>
            </ul>

            <p v-else class="mt-2 text-sm text-gray-500">
              Nothing to settle in {{ block.currency }} — no two balances are left
              to match against each other.
            </p>

            <p v-if="block.settlements.length" class="mt-2 text-xs text-gray-500">
              This is <em>a</em> set of transfers that clears these balances, not
              the only one and not necessarily the fewest. It is regenerated on
              every load and may come back differently next time, so treat it as a
              suggestion rather than the answer — any transfers that leave
              everyone’s balance at zero are equally correct.
            </p>
          </div>
        </section>

        <!-- Payments. Recording or deleting one moves every balance above, so
             the summary is refetched rather than adjusted here. -->
        <div class="mt-4">
          <PaymentTracker
            :split-id="splitId"
            :members="members"
            :currencies="currenciesInUse"
            :default-currency="summary.base_currency"
            @changed="refresh"
          />
        </div>
      </template>
    </div>
  </AppLayout>
</template>

<script setup>
import { ref, computed, onMounted, watch } from 'vue'
import { useRoute } from 'vue-router'
import { storeToRefs } from 'pinia'
import AppLayout from '@/components/AppLayout.vue'
import PaymentTracker from '@/components/PaymentTracker.vue'
import { usePaymentsStore } from '@/stores/payments'
import { useSplitsStore } from '@/stores/splits'
import { useBillsStore } from '@/stores/bills'
import { formatMoney, formatDay, billLabel, pluralize, isZeroAmount } from '@/lib/format'

const route    = useRoute()
const payments = usePaymentsStore()
const splits   = useSplitsStore()
const bills    = useBillsStore()

const { summary: summaryData } = storeToRefs(payments)
const { current: split, members } = storeToRefs(splits)
const { bills: billList } = storeToRefs(bills)

const splitId = computed(() => route.params.id)

// The store holds one summary at a time and it is shared with the share view,
// so a stale one from a previously viewed split must not be rendered against
// this one's id.
const summary = computed(() =>
  summaryData.value?.split_id === splitId.value ? summaryData.value : null,
)

const loading      = ref(true)
const error        = ref('')
const notFound     = ref(false)
const refreshing   = ref(false)
const refreshError = ref('')
const membersError = ref('')
const billsError   = ref('')

// Falls back to the split record so the back link is still named while the
// summary itself is loading.
const splitName = computed(() =>
  summary.value?.name ?? (split.value?.id === splitId.value ? split.value.name : ''),
)

const currencyCount = computed(() =>
  pluralize(summary.value?.by_currency.length ?? 0, 'currency', 'currencies'),
)

// "EUR, GBP and USD" — for the mixed-currency notice.
const currencyList = computed(() => {
  const codes = (summary.value?.by_currency ?? []).map(c => c.currency)
  if (codes.length <= 1) return codes.join('')
  return `${codes.slice(0, -1).join(', ')} and ${codes[codes.length - 1]}`
})

// Every currency the split actually touches, plus its own currency so a split
// with no activity yet can still have a payment recorded against it.
const currenciesInUse = computed(() => {
  const s = summary.value
  if (!s) return []
  return [...new Set([s.base_currency, ...s.by_currency.map(c => c.currency)].filter(Boolean))]
})

onMounted(load)
// The route component is reused when moving between splits, so the param has to
// be watched rather than read once on mount.
watch(() => route.params.id, load)

async function load() {
  loading.value      = true
  error.value        = ''
  notFound.value     = false
  refreshError.value = ''
  membersError.value = ''
  billsError.value   = ''

  // Drop the previous split's summary outright; the guard above only stops it
  // rendering, it doesn't stop it lingering.
  payments.reset()

  // Three independent requests. The summary is the page; the members are only
  // needed to record a payment and the bills only to link out, so losing either
  // must not blank out balances that loaded perfectly well.
  const [summaryResult, splitResult, billsResult] = await Promise.allSettled([
    payments.fetchSummary(splitId.value),
    splits.fetchSplit(splitId.value),
    bills.fetchBills(splitId.value),
  ])

  if (summaryResult.status === 'rejected') {
    const e = summaryResult.reason
    // The store normalized this into a user-facing message; the status is what
    // tells a missing split apart from a real failure.
    if (e.status === 404) notFound.value = true
    else error.value = e.message
  }
  if (splitResult.status === 'rejected') membersError.value = splitResult.reason.message
  if (billsResult.status === 'rejected') billsError.value = billsResult.reason.message

  loading.value = false
}

// A payment changes every balance in its currency, so the summary is refetched
// rather than patched. Unlike `load` this keeps the current figures on screen
// and reports a failure inline, so a flaky refresh never blanks the page.
async function refresh() {
  refreshing.value   = true
  refreshError.value = ''
  try {
    await payments.fetchSummary(splitId.value)
  } catch (e) {
    if (e.status === 404) notFound.value = true
    else refreshError.value = e.message
  } finally {
    refreshing.value = false
  }
}

// ── Display helpers ─────────────────────────────────────────────────────────
// Nothing below does arithmetic. Balances, totals, shares and settlement
// amounts all arrive finished from the server (docs/api.md, "Balances, not
// 'who owes the payer'"); these only decide how to word and colour them.

// The sign, read off the string rather than computed from it.
function balanceSign(raw) {
  if (isZeroAmount(raw)) return 'zero'
  return String(raw ?? '').trim().startsWith('-') ? 'negative' : 'positive'
}

function balanceTone(raw) {
  const sign = balanceSign(raw)
  if (sign === 'positive') return 'text-green-700'
  if (sign === 'negative') return 'text-red-700'
  return 'text-gray-500'
}

// The whole point of this column: a minus sign is easy to miss, so the sign is
// spelled out in words next to it.
function balanceMeaning(block, m) {
  const sign = balanceSign(m.balance)
  if (sign === 'positive') return `the split owes ${displayName(block, m)}`
  if (sign === 'negative') return `${displayName(block, m)} owes the split`
  return 'settled up'
}

// Names are not identities: two members of one split really can both be
// "Alice", which makes "Alice pays Alice" unreadable. PaymentTracker numbers
// duplicates by join order and is handed the very same member list, so the
// same scheme is used here — labelling one person two different ways on one
// page would be worse than not disambiguating at all.
const memberLabels = computed(() => {
  const list = members.value ?? []
  const counts = new Map()
  for (const m of list) counts.set(m.name, (counts.get(m.name) ?? 0) + 1)
  const seen = new Map()
  const labels = new Map()
  for (const m of list) {
    const n = (seen.get(m.name) ?? 0) + 1
    seen.set(m.name, n)
    const name = m.name || 'Unnamed member'
    labels.set(m.id, counts.get(m.name) > 1 ? `${name} (#${n})` : name)
  }
  return labels
})

function displayName(block, m) {
  const shared = memberLabels.value.get(m.member_id)
  if (shared) return shared
  // The member list failed to load, or this member appears in the summary but
  // not in that list. Number within the block instead so duplicates are still
  // distinguishable — those numbers may not match the payment form's, which is
  // why this is only a fallback.
  const name = m.name || 'Unnamed member'
  const sameName = block.members.filter(x => x.name === m.name)
  return sameName.length > 1 ? `${name} (#${sameName.indexOf(m) + 1})` : name
}

// Settlements carry their own joined names, but resolving through the member id
// first keeps a duplicated name disambiguated the same way the table does.
function settlementName(block, memberId, fallback) {
  const m = block.members.find(x => x.member_id === memberId)
  return m ? displayName(block, m) : (fallback ?? 'Unknown member')
}

// Convenience links only. The summary carries no bill ids, so these come from
// the bills list; an empty result just means no links, never a wrong figure.
function billsFor(currency) {
  return billList.value.filter(b => b.split_id === splitId.value && b.currency === currency)
}
</script>
