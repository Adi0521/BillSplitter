<template>
  <!-- Deliberately NOT wrapped in <AppLayout>. That component renders the
       logged-in nav, the signed-in user's email and a Log out button, and calls
       useAuthStore() — all meaningless to a stranger following a link. This page
       must render for someone with no account and no session, so it brings its
       own minimal chrome and links nowhere into the authenticated app. -->
  <div class="min-h-screen bg-gray-50">
    <header class="border-b border-gray-200 bg-white">
      <div class="mx-auto flex max-w-3xl items-center justify-between gap-4 px-6 py-3">
        <span class="text-sm font-semibold text-gray-900">BillSplitter</span>
        <span class="rounded-full bg-gray-100 px-2 py-0.5 text-xs font-medium text-gray-600">
          Shared view · read only
        </span>
      </div>
    </header>

    <main class="mx-auto max-w-3xl p-6">
      <!-- 1. Loading -->
      <div v-if="loading" class="space-y-4" aria-busy="true">
        <p class="sr-only">Loading this shared split…</p>
        <div class="card animate-pulse">
          <div class="h-7 w-1/2 rounded bg-gray-200"></div>
          <div class="mt-3 h-4 w-2/3 rounded bg-gray-100"></div>
          <div class="mt-3 h-4 w-1/3 rounded bg-gray-100"></div>
        </div>
        <div class="card animate-pulse">
          <div class="h-5 w-24 rounded bg-gray-200"></div>
          <div class="mt-4 h-4 w-3/4 rounded bg-gray-100"></div>
          <div class="mt-3 h-4 w-2/3 rounded bg-gray-100"></div>
          <div class="mt-3 h-4 w-1/2 rounded bg-gray-100"></div>
        </div>
      </div>

      <!-- 2a. Dead link. The single most likely thing to happen on this page:
              strangers arrive from a chat message weeks after the fact. The API
              answers 404 identically for "never existed", "malformed" and
              "regenerated", so the copy must not claim to know which — and it
              must not offer signing in as the fix, because the visitor has no
              account and /login would be a dead end. -->
      <div v-else-if="notFound" class="card py-12 text-center">
        <h1 class="text-lg font-medium text-gray-900">This link isn’t valid any more</h1>
        <p class="mx-auto mt-3 max-w-sm text-sm text-gray-600">
          It doesn’t point to a split we can show you. Whoever owns the split can
          regenerate its share link at any time, which switches the old one off
          straight away — so a link that worked before may simply have been
          replaced.
        </p>
        <p class="mx-auto mt-3 max-w-sm text-sm text-gray-500">
          Ask the person who sent it for the current link. Nothing has been lost;
          only this address stopped working.
        </p>
      </div>

      <!-- 2b. Anything else: the network is down, or the server errored. -->
      <div v-else-if="error" class="card border-red-200 bg-red-50">
        <h1 class="text-sm font-medium text-red-800">Couldn’t load this shared split</h1>
        <p class="mt-2 text-sm text-red-700">{{ error }}</p>
        <button type="button" class="btn-secondary mt-4" :disabled="loading" @click="load">
          Try again
        </button>
      </div>

      <!-- 3. Loaded -->
      <template v-else-if="split">
        <!-- Header -->
        <section class="card">
          <div class="flex items-start justify-between gap-4">
            <div class="min-w-0 flex-1">
              <h1 class="break-words text-2xl font-bold">{{ split.name }}</h1>
              <p v-if="split.description" class="mt-1 break-words text-sm text-gray-600">
                {{ split.description }}
              </p>
            </div>
            <span
              class="shrink-0 rounded-full bg-gray-100 px-2 py-0.5 text-xs font-medium text-gray-700"
            >
              {{ split.base_currency }}
            </span>
          </div>

          <div class="mt-4 flex flex-wrap items-center gap-2 text-xs">
            <span class="rounded-full bg-primary-50 px-2 py-0.5 font-medium text-primary-700">
              {{ typeLabel(split.type) }}
            </span>
            <span v-if="createdOn" class="text-gray-500">Created {{ createdOn }}</span>
            <span class="text-gray-500">· {{ billLabel(bills.length) }}</span>
            <span class="text-gray-500">· {{ paymentLabel(payments.length) }}</span>
          </div>

          <p
            v-if="split.archived"
            class="mt-4 rounded-lg border border-amber-200 bg-amber-50 px-3 py-2 text-sm text-amber-800"
          >
            <span class="font-medium">Archived.</span>
            The owner has put this split away, so it’s unlikely to change again.
            The link still works — archiving doesn’t break a link that was already
            shared.
          </p>

          <p class="mt-4 border-t border-gray-100 pt-4 text-xs text-gray-500">
            This is a read-only snapshot of someone else’s split, shared by link.
            You’re not signed in and don’t need to be — nothing here can be
            edited, and the figures update only when the page is reloaded.
          </p>
        </section>

        <!-- Nothing recorded at all. `by_currency` is [] for a split with
             neither bills nor payments; the API does not fabricate a zero row. -->
        <section v-if="isEmpty" class="card mt-4 py-10 text-center">
          <h2 class="text-base font-medium text-gray-900">Nothing to show yet</h2>
          <p class="mx-auto mt-2 max-w-sm text-sm text-gray-500">
            No bills and no payments have been added to this split, so there are
            no balances to work out. Whoever shared this link will need to add a
            bill first — check back once they have, or ask them.
          </p>
        </section>

        <template v-else>
          <p
            v-if="split.mixed_currency"
            class="mt-4 rounded-lg border border-amber-200 bg-amber-50 px-3 py-2 text-sm text-amber-800"
          >
            <span class="font-medium">This split uses more than one currency.</span>
            Each is tracked completely separately and nothing is ever converted
            between them — there is no exchange rate anywhere on this page. Read
            each block below on its own; the totals are not meant to be added
            together.
          </p>

          <!-- Bills -->
          <section class="card mt-4">
            <h2 class="text-lg font-semibold">Bills</h2>

            <p v-if="bills.length === 0" class="mt-3 text-sm text-gray-500">
              No bills on this split. The payments below are the only money
              recorded so far.
            </p>

            <ul v-else class="mt-4 divide-y divide-gray-100">
              <li
                v-for="(bill, i) in bills"
                :key="`${bill.store_name}-${bill.date}-${i}`"
                class="flex items-start justify-between gap-4 py-3"
              >
                <div class="min-w-0">
                  <p class="break-words font-medium text-gray-900">{{ bill.store_name }}</p>
                  <p class="mt-0.5 text-xs text-gray-500">
                    <span v-if="formatDay(bill.date)">{{ formatDay(bill.date) }} · </span>
                    {{ itemLabel(bill.item_count) }}
                    <template v-if="bill.payer_name"> · paid by {{ bill.payer_name }}</template>
                    <template v-else> · no payer recorded</template>
                  </p>
                </div>
                <div class="shrink-0 text-right">
                  <p class="tabular-nums font-medium text-gray-900">
                    {{ formatMoney(bill.total) }}
                  </p>
                  <p class="text-xs text-gray-500">{{ bill.currency }}</p>
                </div>
              </li>
            </ul>
          </section>

          <!-- Balances, one block per currency. Every figure below is a string
               the server computed; this page does no arithmetic of its own, so
               it never contradicts the app the split lives in. -->
          <section
            v-for="block in byCurrency"
            :key="block.currency"
            class="card mt-4"
          >
            <div class="flex items-baseline justify-between gap-4">
              <h2 class="text-lg font-semibold">{{ block.currency }} balances</h2>
              <span class="text-xs text-gray-500">
                {{ billLabel(block.bill_count) }} · {{ formatMoney(block.total) }}
                {{ block.currency }}
              </span>
            </div>

            <p v-if="Number(block.bill_count) === 0" class="mt-2 text-xs text-gray-500">
              No bills in {{ block.currency }} — this block exists because a
              payment was recorded in it.
            </p>

            <table class="mt-4 w-full text-sm">
              <thead>
                <tr
                  class="border-b border-gray-200 text-left text-xs uppercase tracking-wide text-gray-500"
                >
                  <th scope="col" class="pb-2 font-medium">Member</th>
                  <th scope="col" class="pb-2 pl-3 text-right font-medium">Owes</th>
                  <th scope="col" class="pb-2 pl-3 text-right font-medium">Fronted</th>
                  <th scope="col" class="pb-2 pl-3 text-right font-medium">Balance</th>
                </tr>
              </thead>
              <tbody class="divide-y divide-gray-100">
                <tr v-for="m in block.members" :key="m.member_id" class="align-top">
                  <td class="py-3 pr-3">
                    <span class="break-words font-medium text-gray-900">{{ m.name }}</span>
                    <!-- Settled amounts explain a balance that neither `owes`
                         nor `fronted` accounts for, so they are shown rather
                         than leaving the number looking wrong. -->
                    <p v-if="hasSettled(m)" class="mt-0.5 text-xs text-gray-500">
                      <template v-if="!isZeroAmount(m.payments_made)">
                        paid {{ formatMoney(m.payments_made) }}
                      </template>
                      <template v-if="hasBothSettled(m)"> · </template>
                      <template v-if="!isZeroAmount(m.payments_received)">
                        received {{ formatMoney(m.payments_received) }}
                      </template>
                    </p>
                  </td>
                  <td class="py-3 pl-3 text-right tabular-nums text-gray-600">
                    {{ formatMoney(m.owes) }}
                  </td>
                  <td class="py-3 pl-3 text-right tabular-nums text-gray-600">
                    {{ formatMoney(m.fronted) }}
                  </td>
                  <td
                    class="py-3 pl-3 text-right tabular-nums font-medium"
                    :class="balanceClass(m.balance)"
                  >
                    {{ formatMoney(m.balance) }}
                  </td>
                </tr>
              </tbody>
            </table>

            <p class="mt-3 text-xs text-gray-500">
              A positive balance means the split owes that person; a negative one
              means they owe the split. The two sides need not add up to exactly
              zero — rounding and any unassigned amount are left visible here
              rather than quietly absorbed.
            </p>

            <!-- Unallocated. Shown whenever it is non-zero: it is the part of
                 the bills nobody is on the hook for, and hiding it would make
                 the member rows look like they account for the whole total. -->
            <div
              v-if="!isZeroAmount(block.unallocated?.total)"
              class="mt-4 rounded-lg border border-amber-200 bg-amber-50 p-3"
            >
              <div class="flex items-baseline justify-between gap-4 text-sm">
                <span class="font-medium text-amber-800">Not assigned to anyone</span>
                <span class="tabular-nums font-medium text-amber-800">
                  {{ formatMoney(block.unallocated.total) }} {{ block.currency }}
                </span>
              </div>
              <p class="mt-1 text-xs text-amber-800">
                Line items nobody was assigned to, plus the share of tax, tip and
                fees that goes with them. It is part of the
                {{ formatMoney(block.total) }} {{ block.currency }} total above but
                belongs to no one, so it is not in anybody’s “owes”.
              </p>
              <dl class="mt-2 grid grid-cols-2 gap-x-4 gap-y-1 text-xs text-amber-800 sm:grid-cols-4">
                <div class="flex justify-between gap-2">
                  <dt>Items</dt>
                  <dd class="tabular-nums">{{ formatMoney(block.unallocated.items) }}</dd>
                </div>
                <div class="flex justify-between gap-2">
                  <dt>Tax</dt>
                  <dd class="tabular-nums">{{ formatMoney(block.unallocated.tax) }}</dd>
                </div>
                <div class="flex justify-between gap-2">
                  <dt>Tip</dt>
                  <dd class="tabular-nums">{{ formatMoney(block.unallocated.tip) }}</dd>
                </div>
                <div class="flex justify-between gap-2">
                  <dt>Fees</dt>
                  <dd class="tabular-nums">{{ formatMoney(block.unallocated.fees) }}</dd>
                </div>
              </dl>
            </div>

            <!-- Settlements -->
            <div class="mt-5 border-t border-gray-100 pt-4">
              <h3 class="text-sm font-semibold text-gray-900">One way to square up</h3>

              <p v-if="(block.settlements ?? []).length === 0" class="mt-2 text-sm text-gray-500">
                Nothing left to transfer in {{ block.currency }} — the balances
                above have nothing that can be matched up.
              </p>

              <ul v-else class="mt-2 space-y-1.5 text-sm text-gray-700">
                <li
                  v-for="(s, i) in block.settlements"
                  :key="`${s.from_member}-${s.to_member}-${i}`"
                  class="flex items-baseline justify-between gap-3"
                >
                  <span class="break-words">
                    <span class="font-medium text-gray-900">{{ s.from_name }}</span>
                    pays
                    <span class="font-medium text-gray-900">{{ s.to_name }}</span>
                  </span>
                  <span class="shrink-0 tabular-nums font-medium text-gray-900">
                    {{ formatMoney(s.amount) }} {{ block.currency }}
                  </span>
                </li>
              </ul>

              <p class="mt-3 text-xs text-gray-500">
                This is <span class="font-medium">one</span> valid set of transfers
                that clears the balances, not the only one and not necessarily the
                fewest. A different set may appear the next time this page loads,
                and any amount left over from rounding is not forced into a
                transfer. Treat it as a suggestion, not a bill.
              </p>
            </div>
          </section>

          <!-- Payments -->
          <section class="card mt-4">
            <h2 class="text-lg font-semibold">Payments recorded</h2>

            <p v-if="payments.length === 0" class="mt-3 text-sm text-gray-500">
              Nobody has recorded settling up yet. When someone does, it will show
              here and the balances above will move to match.
            </p>

            <ul v-else class="mt-4 divide-y divide-gray-100">
              <li
                v-for="(p, i) in payments"
                :key="`${p.from_name}-${p.to_name}-${p.paid_at}-${i}`"
                class="flex items-start justify-between gap-4 py-3"
              >
                <div class="min-w-0">
                  <p class="break-words text-sm text-gray-900">
                    <span class="font-medium">{{ p.from_name }}</span>
                    paid
                    <span class="font-medium">{{ p.to_name }}</span>
                  </p>
                  <p class="mt-0.5 text-xs text-gray-500">
                    <span v-if="formatDate(p.paid_at)">{{ formatDate(p.paid_at) }}</span>
                    <template v-if="p.method"> · {{ p.method }}</template>
                    <template v-else> · method not recorded</template>
                  </p>
                </div>
                <div class="shrink-0 text-right">
                  <p class="tabular-nums font-medium text-gray-900">{{ formatMoney(p.amount) }}</p>
                  <p class="text-xs text-gray-500">{{ p.currency }}</p>
                </div>
              </li>
            </ul>

            <p class="mt-4 text-xs text-gray-400">
              Any notes attached to a payment are left out of a shared link on
              purpose, along with everyone’s email address.
            </p>
          </section>
        </template>

        <p class="mt-6 pb-4 text-center text-xs text-gray-400">
          Shared from BillSplitter. Anyone with this link can see this page.
        </p>
      </template>
    </main>
  </div>
</template>

<script setup>
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { storeToRefs } from 'pinia'
import { usePaymentsStore } from '@/stores/payments'
import { billLabel, formatDate, formatDay, formatMoney, itemLabel, pluralize, typeLabel, isZeroAmount } from '@/lib/format'

const route = useRoute()
const paymentsStore = usePaymentsStore()
const { publicSplit } = storeToRefs(paymentsStore)

const token = computed(() => String(route.params.token ?? ''))

const loading  = ref(true)
const error    = ref('')
const notFound = ref(false)

// The store holds one publicSplit at a time. Recording which token produced it
// keeps a previously-viewed split off the screen while a new token loads —
// otherwise pasting a second link would briefly show someone else's numbers
// under the new URL.
const shownToken = ref('')
const split = computed(() => (shownToken.value && shownToken.value === token.value ? publicSplit.value : null))

const bills      = computed(() => split.value?.bills ?? [])
const payments   = computed(() => split.value?.payments ?? [])
const byCurrency = computed(() => split.value?.by_currency ?? [])

// A split with neither bills nor payments returns `by_currency: []`. That is a
// real state, not an error, and it deserves an explanation rather than three
// empty panels.
const isEmpty = computed(
  () => bills.value.length === 0 && payments.value.length === 0 && byCurrency.value.length === 0,
)

const createdOn = computed(() => formatDate(split.value?.created_at))

// Guards against an out-of-order response when the token changes mid-flight.
let seq = 0

onMounted(load)
// The route component is reused when the token changes, so the param is watched
// rather than read once.
watch(token, load)

async function load() {
  const t = token.value
  const mine = ++seq

  loading.value  = true
  error.value    = ''
  notFound.value = false

  if (!t) {
    if (mine === seq) {
      notFound.value = true
      loading.value = false
    }
    return
  }

  try {
    await paymentsStore.fetchPublic(t)
    if (mine !== seq) return
    shownToken.value = t
  } catch (e) {
    if (mine !== seq) return
    shownToken.value = ''
    // 404 covers unknown, malformed and regenerated tokens alike — the API
    // refuses to distinguish them, and so does this page.
    if (e.status === 404) notFound.value = true
    else error.value = e.message || 'Something went wrong loading this link.'
  } finally {
    if (mine === seq) loading.value = false
  }
}

// "1 payment" / "3 payments". pluralize lives in lib/format; only the noun is
// local to this view.
function paymentLabel(count) {
  return pluralize(count, 'payment')
}

function hasSettled(m) {
  return !isZeroAmount(m?.payments_made) || !isZeroAmount(m?.payments_received)
}

function hasBothSettled(m) {
  return !isZeroAmount(m?.payments_made) && !isZeroAmount(m?.payments_received)
}

// Colour only — the sign is read off the string, never computed.
function balanceClass(raw) {
  if (isZeroAmount(raw)) return 'text-gray-500'
  return String(raw).trim().startsWith('-') ? 'text-red-700' : 'text-green-700'
}
</script>
