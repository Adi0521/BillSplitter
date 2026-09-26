<template>
  <AppLayout>
    <div class="p-6 max-w-3xl mx-auto">
      <router-link to="/" class="text-sm text-gray-500 hover:text-gray-700">
        &larr; All splits
      </router-link>

      <!-- 1. Loading -->
      <div v-if="loading" class="mt-4 space-y-4" aria-busy="true">
        <p class="sr-only">Loading this split…</p>
        <div class="card animate-pulse">
          <div class="h-6 w-1/3 rounded bg-gray-200"></div>
          <div class="mt-3 h-4 w-1/2 rounded bg-gray-100"></div>
        </div>
        <div class="card animate-pulse">
          <div class="h-5 w-24 rounded bg-gray-200"></div>
          <div class="mt-4 h-4 w-2/3 rounded bg-gray-100"></div>
          <div class="mt-3 h-4 w-1/2 rounded bg-gray-100"></div>
        </div>
      </div>

      <!-- 2a. Not found — an id that doesn't exist, or one this user neither
              owns nor holds a linked seat in. The API answers 404 for both so
              it can't be used to probe which split ids exist. -->
      <div v-else-if="notFound" class="card mt-4 text-center py-12">
        <h1 class="text-lg font-medium text-gray-900">Split not found</h1>
        <p class="mt-2 text-sm text-gray-500 max-w-sm mx-auto">
          This split either doesn’t exist or you don’t have access to it. If
          the owner meant to include you, ask them for an invite link.
        </p>
        <router-link to="/" class="btn-primary inline-block mt-6">
          Back to my splits
        </router-link>
      </div>

      <!-- 2b. Any other failure -->
      <div v-else-if="error" class="card mt-4 border-red-200 bg-red-50">
        <h1 class="text-sm font-medium text-red-800">Couldn’t load this split</h1>
        <p class="mt-1 text-sm text-red-700">{{ error }}</p>
        <div class="mt-4 flex gap-2">
          <button class="btn-secondary" @click="load">Try again</button>
          <router-link to="/" class="btn-secondary">Back to my splits</router-link>
        </div>
      </div>

      <!-- 3. Loaded -->
      <template v-else-if="split">
        <!-- Header -->
        <div class="card mt-4">
          <div class="flex items-start justify-between gap-4">
            <div class="min-w-0 flex-1">
              <!-- Rename: inline, and only sends the field that changed. -->
              <form v-if="editingName" class="space-y-2" @submit.prevent="saveName">
                <label class="label" for="split-name">Split name</label>
                <input
                  id="split-name"
                  ref="nameInput"
                  v-model="nameDraft"
                  type="text"
                  maxlength="200"
                  class="input"
                  :disabled="savingName"
                />
                <p v-if="nameError" class="text-sm text-red-600">{{ nameError }}</p>
                <div class="flex gap-2 pt-1">
                  <button type="submit" class="btn-primary" :disabled="savingName">
                    {{ savingName ? 'Saving…' : 'Save' }}
                  </button>
                  <button
                    type="button"
                    class="btn-secondary"
                    :disabled="savingName"
                    @click="cancelRename"
                  >
                    Cancel
                  </button>
                </div>
              </form>

              <h1 v-else class="text-2xl font-bold break-words">{{ split.name }}</h1>

              <p v-if="split.description" class="mt-2 text-sm text-gray-600 whitespace-pre-line">
                {{ split.description }}
              </p>
            </div>

            <div v-if="!editingName" class="flex shrink-0 gap-2">
              <!-- The summary is where the split's balances live; without a
                   link here the page is only reachable by typing the URL. -->
              <router-link
                :to="{ name: 'SplitSummary', params: { id: route.params.id } }"
                class="btn-secondary text-xs px-3 py-1.5"
              >
                Who owes what
              </router-link>
              <button
                type="button"
                class="btn-secondary text-xs px-3 py-1.5"
                @click="startRename"
              >
                Rename
              </button>
            </div>
          </div>

          <div class="mt-4 flex flex-wrap items-center gap-2">
            <span class="rounded-full bg-primary-50 text-primary-700 px-2 py-0.5 text-xs font-medium">
              {{ typeLabel(split.type) }}
            </span>
            <span class="rounded-full bg-gray-100 text-gray-700 px-2 py-0.5 text-xs font-medium">
              {{ split.currency }}
            </span>
            <span
              v-if="split.archived_at"
              class="rounded-full bg-gray-200 text-gray-600 px-2 py-0.5 text-xs font-medium"
            >
              Archived
            </span>
            <span v-if="formatDate(split.created_at)" class="text-xs text-gray-500">
              Created {{ formatDate(split.created_at) }}
            </span>
          </div>

          <!-- A member landed here through an invite link, not by creating the
               split. Say so, and what that does and doesn't let them do. -->
          <p
            v-if="isMember"
            class="mt-4 rounded-lg bg-primary-50 px-3 py-2 text-sm text-primary-700"
          >
            You were invited to this split by its owner. You can add and edit
            bills, payments and members just like they can; only the owner can
            invite or remove people.
          </p>
        </div>

        <!-- Members -->
        <section class="card mt-4">
          <div class="flex items-baseline justify-between gap-4">
            <h2 class="text-lg font-semibold">Members</h2>
            <span class="text-xs text-gray-500">{{ memberLabel(members.length) }}</span>
          </div>

          <p v-if="memberError" class="mt-3 rounded-lg border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700">
            {{ memberError }}
          </p>

          <!-- Members empty state -->
          <p v-if="members.length === 0" class="mt-4 text-sm text-gray-500">
            Nobody is in this split yet. Add the first person below — a name is
            all you need.
          </p>

          <!-- Duplicate names are legal (two people really can both be "Bob"),
               so the row key is the member id, never the name. -->
          <ul v-else class="mt-4 divide-y divide-gray-100">
            <li v-for="member in members" :key="member.id" class="py-3 first:pt-0">
              <div class="flex items-center justify-between gap-4">
                <div class="min-w-0">
                  <div class="flex flex-wrap items-center gap-2">
                    <span class="text-sm font-medium text-gray-900 truncate">{{ member.name }}</span>
                    <span
                      v-if="isYou(member)"
                      class="rounded-full bg-primary-50 text-primary-700 px-2 py-0.5 text-xs font-medium"
                    >
                      You
                    </span>
                    <!-- Seat status. Owner and Linked mean an account holds the
                         seat; Invite pending means a link is out but unclaimed;
                         no badge means it's just a name for now. -->
                    <span
                      v-if="statusLabel(member)"
                      class="rounded-full px-2 py-0.5 text-xs"
                      :class="member.invite_pending && !member.linked
                        ? 'bg-amber-50 text-amber-700'
                        : 'bg-gray-100 text-gray-600'"
                    >
                      {{ statusLabel(member) }}
                    </span>
                  </div>
                  <p v-if="member.email" class="text-xs text-gray-500 truncate">{{ member.email }}</p>
                  <p v-if="formatDate(member.joined_at)" class="text-xs text-gray-400">
                    Added {{ formatDate(member.joined_at) }}
                  </p>
                </div>

                <!-- Seat management is owner-only (the API answers 403 for a
                     member), so a member sees no buttons here at all rather
                     than buttons that fail. The owner's own seat can't be
                     removed or invited, so it gets none either. -->
                <div
                  v-if="isOwner && !member.is_owner"
                  class="flex shrink-0 flex-wrap items-center justify-end gap-2"
                >
                  <!-- Inline confirm for whichever action is pending on this
                       row, so the row stays visible while you decide. -->
                  <template v-if="isConfirming(member)">
                    <span class="text-xs text-gray-600">{{ confirmPrompt(confirming.action) }}</span>
                    <button
                      type="button"
                      class="btn-secondary text-xs px-3 py-1.5 border-red-300 text-red-700 hover:bg-red-50"
                      :disabled="isBusy(member)"
                      @click="runConfirmed(member)"
                    >
                      {{ isBusy(member) ? busyLabel(confirming.action) : confirmYes(confirming.action) }}
                    </button>
                    <button
                      type="button"
                      class="btn-secondary text-xs px-3 py-1.5"
                      :disabled="isBusy(member)"
                      @click="confirming = null"
                    >
                      Cancel
                    </button>
                  </template>

                  <template v-else>
                    <!-- Unlinked, no link out: a fresh invite needs no confirm. -->
                    <button
                      v-if="!member.linked && !member.invite_pending"
                      type="button"
                      class="btn-primary text-xs px-3 py-1.5"
                      :disabled="anyBusy"
                      @click="invite(member, { replace: false })"
                    >
                      {{ isBusy(member, 'invite') ? 'Creating link…' : 'Invite' }}
                    </button>

                    <!-- Link out, not yet claimed. Both of these kill the link
                         someone may already be holding, so both confirm first. -->
                    <template v-if="!member.linked && member.invite_pending">
                      <button
                        type="button"
                        class="btn-secondary text-xs px-3 py-1.5"
                        :disabled="anyBusy"
                        @click="startConfirm(member, 'reinvite')"
                      >
                        New link
                      </button>
                      <button
                        type="button"
                        class="btn-secondary text-xs px-3 py-1.5"
                        :disabled="anyBusy"
                        @click="startConfirm(member, 'revoke')"
                      >
                        Revoke
                      </button>
                    </template>

                    <button
                      type="button"
                      class="btn-secondary text-xs px-3 py-1.5"
                      :disabled="anyBusy"
                      @click="startConfirm(member, 'remove')"
                    >
                      Remove
                    </button>
                  </template>
                </div>
              </div>

              <!-- The invite link, shown exactly once: the API never returns
                   the token again, only "pending". -->
              <div
                v-if="inviteLink && inviteLink.memberId === member.id"
                class="mt-3 rounded-lg border border-primary-100 bg-primary-50 px-3 py-3"
              >
                <p class="text-sm font-medium text-primary-700">
                  Invite link for {{ member.name }}
                </p>
                <p v-if="inviteLink.replaced" class="mt-1 text-xs text-primary-700">
                  The previous link for this seat no longer works.
                </p>
                <div class="mt-2 flex flex-col gap-2 sm:flex-row">
                  <input
                    :ref="setInviteInput"
                    :value="inviteLink.url"
                    type="text"
                    readonly
                    class="input flex-1 text-xs font-mono"
                    aria-label="Invite link"
                    @focus="$event.target.select()"
                  />
                  <button
                    type="button"
                    class="btn-primary shrink-0 text-xs px-3 py-1.5"
                    @click="copyInviteLink"
                  >
                    {{ copyState === 'copied' ? 'Copied' : 'Copy link' }}
                  </button>
                  <button
                    type="button"
                    class="btn-secondary shrink-0 text-xs px-3 py-1.5"
                    @click="dismissInviteLink"
                  >
                    Done
                  </button>
                </div>
                <p v-if="copyState === 'manual'" class="mt-2 text-xs text-primary-700">
                  Copying isn’t available here — the link is selected above, so
                  copy it by hand.
                </p>
                <p class="mt-2 text-xs text-primary-700">
                  This link is shown once. Anyone who opens it while signed in
                  joins this split as {{ member.name }}, so send it only to
                  them. If it goes astray, use Revoke or New link.
                </p>
              </div>
            </li>
          </ul>

          <!-- Add member -->
          <form class="mt-6 border-t border-gray-100 pt-4 space-y-3" @submit.prevent="submitMember">
            <div class="flex flex-col gap-3 sm:flex-row">
              <div class="flex-1">
                <label class="label" for="member-name">Name</label>
                <input
                  id="member-name"
                  v-model="newName"
                  type="text"
                  maxlength="100"
                  placeholder="Bob"
                  class="input"
                  :disabled="adding"
                />
              </div>
              <div class="flex-1">
                <label class="label" for="member-email">
                  Email <span class="text-gray-400">(optional)</span>
                </label>
                <input
                  id="member-email"
                  v-model="newEmail"
                  type="text"
                  maxlength="320"
                  placeholder="bob@example.com"
                  class="input"
                  :disabled="adding"
                />
              </div>
            </div>

            <p v-if="addError" class="text-sm text-red-600">{{ addError }}</p>
            <p class="text-xs text-gray-400">
              Adding a name creates a seat; no email is sent.
              <template v-if="isOwner">
                To give someone access to this split, use Invite on their row
                and send them the link.
              </template>
              <template v-else>Only the owner can send invite links.</template>
            </p>

            <button type="submit" class="btn-primary" :disabled="adding">
              {{ adding ? 'Adding…' : 'Add member' }}
            </button>
          </form>

          <!-- Leaving unlinks the account from the seat; the seat itself and
               everything recorded against it stay in the ledger. The owner
               can't leave (the API says 400) — they archive instead. -->
          <div v-if="isMember" class="mt-6 border-t border-gray-100 pt-4">
            <h3 class="text-sm font-medium text-gray-900">Leave this split</h3>
            <p class="mt-1 text-xs text-gray-500">
              Your account is unlinked from your seat and this split leaves
              your list. Your payments and allocations stay in the ledger under
              your name. To come back, you’d need a new invite link from the
              owner.
            </p>
            <p v-if="leaveError" class="mt-2 text-sm text-red-600">{{ leaveError }}</p>
            <div v-if="confirmingLeave" class="mt-3 flex items-center gap-2">
              <span class="text-xs text-gray-600">Leave this split?</span>
              <button
                type="button"
                class="btn-secondary text-xs px-3 py-1.5 border-red-300 text-red-700 hover:bg-red-50"
                :disabled="leaving"
                @click="leave"
              >
                {{ leaving ? 'Leaving…' : 'Yes, leave' }}
              </button>
              <button
                type="button"
                class="btn-secondary text-xs px-3 py-1.5"
                :disabled="leaving"
                @click="confirmingLeave = false"
              >
                Cancel
              </button>
            </div>
            <button
              v-else
              type="button"
              class="btn-secondary mt-3 text-xs px-3 py-1.5 border-red-300 text-red-700 hover:bg-red-50"
              @click="confirmingLeave = true"
            >
              Leave this split
            </button>
          </div>
        </section>

        <!-- Bills. Loads separately from the split so a bills failure leaves
             the header and members above it intact. -->
        <section class="card mt-4">
          <div class="flex items-baseline justify-between gap-4">
            <h2 class="text-lg font-semibold">Bills</h2>
            <span v-if="!billsLoading && !billsError && bills.length" class="text-xs text-gray-500">
              {{ billLabel(bills.length) }}
            </span>
          </div>

          <!-- A failed delete shouldn’t take the list with it. -->
          <p
            v-if="billActionError"
            class="mt-3 rounded-lg border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700"
          >
            {{ billActionError }}
          </p>

          <!-- 1. Loading -->
          <div v-if="billsLoading" class="mt-4 space-y-3" aria-busy="true">
            <p class="sr-only">Loading bills…</p>
            <div v-for="n in 2" :key="n" class="animate-pulse py-3">
              <div class="h-4 w-1/3 rounded bg-gray-200"></div>
              <div class="mt-3 h-3 w-1/2 rounded bg-gray-100"></div>
            </div>
          </div>

          <!-- 2. Error — scoped to this section -->
          <div v-else-if="billsError" class="mt-4 rounded-lg border border-red-200 bg-red-50 px-4 py-3">
            <p class="text-sm font-medium text-red-800">Couldn’t load bills</p>
            <p class="mt-1 text-sm text-red-700">{{ billsError }}</p>
            <button type="button" class="btn-secondary mt-3" @click="loadBills">Try again</button>
          </div>

          <!-- 3. Empty -->
          <div v-else-if="bills.length === 0" class="mt-2 text-center py-8">
            <p class="text-sm text-gray-500 max-w-sm mx-auto">
              No bills here yet. Add the receipt from the last shop or meal and
              this split starts keeping track of it.
            </p>
            <router-link
              :to="{ name: 'NewBill', params: { id: split.id } }"
              class="btn-primary inline-block mt-6"
            >
              Add the first bill
            </router-link>
          </div>

          <!-- 4. Loaded -->
          <template v-else>
            <ul class="mt-4 divide-y divide-gray-100">
              <li v-for="bill in bills" :key="bill.id" class="py-3 first:pt-0">
                <div class="flex items-start justify-between gap-4">
                  <div class="min-w-0">
                    <router-link
                      :to="{ name: 'BillDetail', params: { splitId: split.id, billId: bill.id } }"
                      class="block truncate text-sm font-medium text-gray-900 hover:text-primary-600"
                    >
                      {{ bill.store_name }}
                    </router-link>
                    <div class="mt-1 flex flex-wrap items-center gap-x-2 gap-y-1 text-xs text-gray-500">
                      <template v-if="formatDay(bill.date)">
                        <span>{{ formatDay(bill.date) }}</span>
                        <span aria-hidden="true">·</span>
                      </template>
                      <span>{{ bill.currency }}</span>
                      <span aria-hidden="true">·</span>
                      <span>{{ itemLabel(bill.item_count) }}</span>
                    </div>
                  </div>

                  <div class="flex shrink-0 items-center gap-3">
                    <!-- The server computes this total; it is only reformatted
                         to 2dp here, never recomputed from the items. -->
                    <span class="text-sm font-medium tabular-nums text-gray-900">
                      {{ formatMoney(bill.total) }}
                    </span>
                    <button
                      v-if="confirmingBillId !== bill.id"
                      type="button"
                      class="btn-secondary text-xs px-3 py-1.5"
                      :disabled="deletingBillId !== null"
                      @click="confirmDeleteBill(bill)"
                    >
                      Delete
                    </button>
                  </div>
                </div>

                <!-- Confirmed inline, like member removal, so the bill stays on
                     screen while you decide. -->
                <div
                  v-if="confirmingBillId === bill.id"
                  class="mt-2 rounded-lg border border-red-200 bg-red-50 px-3 py-2"
                >
                  <p class="text-xs text-red-700">
                    Delete this bill? Unlike a split, a bill isn’t archived — it and
                    its {{ itemLabel(bill.item_count) }} are gone for good.
                  </p>
                  <div class="mt-2 flex gap-2">
                    <button
                      type="button"
                      class="btn-secondary text-xs px-3 py-1.5 border-red-300 text-red-700 hover:bg-red-100"
                      :disabled="deletingBillId === bill.id"
                      @click="deleteBill(bill)"
                    >
                      {{ deletingBillId === bill.id ? 'Deleting…' : 'Yes, delete' }}
                    </button>
                    <button
                      type="button"
                      class="btn-secondary text-xs px-3 py-1.5"
                      :disabled="deletingBillId === bill.id"
                      @click="confirmingBillId = null"
                    >
                      Cancel
                    </button>
                  </div>
                </div>
              </li>
            </ul>

            <div class="mt-6 border-t border-gray-100 pt-4">
              <router-link
                :to="{ name: 'NewBill', params: { id: split.id } }"
                class="btn-primary inline-block"
              >
                Add bill
              </router-link>
            </div>
          </template>
        </section>
      </template>
    </div>
  </AppLayout>
</template>

<script setup>
import { ref, computed, nextTick, onMounted, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { storeToRefs } from 'pinia'
import AppLayout from '@/components/AppLayout.vue'
import { useSplitsStore } from '@/stores/splits'
import { useBillsStore } from '@/stores/bills'
import { typeLabel, memberLabel, itemLabel, billLabel, formatDate, formatDay, formatMoney } from '@/lib/format'
import { useAuthStore } from '@/stores/auth'

const route  = useRoute()
const router = useRouter()
const store  = useSplitsStore()
const billsStore = useBillsStore()
const auth  = useAuthStore()

// `current` is the SplitDetail; `members` is kept in sync by the store's
// add/remove actions, so the list never needs a refetch after a mutation.
const { current: split, members } = storeToRefs(store)
// Bills live in their own store and are kept in sync by its delete action, so
// removing one doesn’t need a refetch.
const { bills } = storeToRefs(billsStore)

const loading  = ref(true)
const error    = ref('')
const notFound = ref(false)

// `role` is the caller's relationship to this split, decided by the server.
// Owner-only controls are hidden — not merely disabled — for a member, because
// every one of them would come back 403.
const isOwner  = computed(() => split.value?.role === 'owner')
const isMember = computed(() => split.value?.role === 'member')

// Rename
const editingName = ref(false)
const nameDraft   = ref('')
const nameError   = ref('')
const savingName  = ref(false)
const nameInput   = ref(null)

// Add member
const newName  = ref('')
const newEmail = ref('')
const adding   = ref(false)
const addError = ref('')

// Seat actions — remove / invite / re-invite / revoke. One inline confirm and
// one in-flight request at a time, keyed by member id so the right row shows
// its own state.
const confirming  = ref(null)   // { id, action: 'remove' | 'reinvite' | 'revoke' }
const busy        = ref(null)   // { id, action } while a request is in flight
const memberError = ref('')
const anyBusy     = computed(() => busy.value !== null)

// The invite link is returned once and never listed again, so it is held here
// until dismissed. `replaced` is true when it superseded an earlier link.
const inviteLink  = ref(null)   // { memberId, url, replaced }
const copyState   = ref('')     // '' | 'copied' | 'manual'
let   inviteInput = null        // the readonly <input>, for the select fallback
let   copiedTimer = null

// Leave (member only)
const confirmingLeave = ref(false)
const leaving         = ref(false)
const leaveError      = ref('')

// Bills — their own loading/error state so a bills failure never blanks out
// the split header or the members list.
const billsLoading     = ref(true)
const billsError       = ref('')
const billActionError  = ref('')
const confirmingBillId = ref(null)
const deletingBillId   = ref(null)

onMounted(load)
// The route is reused when navigating from one split to another, so the id has
// to be watched rather than only read once on mount.
watch(() => route.params.id, load)

async function load() {
  loading.value  = true
  error.value    = ''
  notFound.value = false
  cancelRename()
  resetMemberState()
  // Held true so the previous split’s bills can’t flash in the new one.
  billsLoading.value = true
  resetBillState()
  try {
    await store.fetchSplit(route.params.id)
    // Not awaited: the header and members render as soon as the split lands,
    // and the bills section fills in under its own loading state.
    loadBills()
  } catch (e) {
    // The store already normalized this into a user-facing message; the status
    // is what tells 404 apart from a real failure.
    if (e.status === 404) notFound.value = true
    else error.value = e.message
  } finally {
    loading.value = false
  }
}

function resetBillState() {
  billsError.value       = ''
  billActionError.value  = ''
  confirmingBillId.value = null
  deletingBillId.value   = null
}

// Never throws: a bills failure is reported inside the bills section only.
async function loadBills() {
  billsLoading.value = true
  resetBillState()
  try {
    await billsStore.fetchBills(route.params.id)
  } catch (e) {
    billsError.value = e.message
  } finally {
    billsLoading.value = false
  }
}

function confirmDeleteBill(bill) {
  billActionError.value  = ''
  confirmingBillId.value = bill.id
}

async function deleteBill(bill) {
  if (deletingBillId.value) return
  deletingBillId.value  = bill.id
  billActionError.value = ''
  try {
    // The store drops the row from `bills` itself.
    await billsStore.deleteBill(split.value.id, bill.id)
    confirmingBillId.value = null
  } catch (e) {
    billActionError.value = e.message
  } finally {
    deletingBillId.value = null
  }
}

function startRename() {
  nameDraft.value   = split.value?.name ?? ''
  nameError.value   = ''
  editingName.value = true
  nextTick(() => nameInput.value?.focus())
}

function cancelRename() {
  editingName.value = false
  nameError.value   = ''
  nameDraft.value   = ''
}

async function saveName() {
  if (savingName.value) return
  const next = nameDraft.value.trim()
  nameError.value = ''

  if (next.length < 1) {
    nameError.value = 'A name is required.'
    return
  }
  if (next.length > 200) {
    nameError.value = 'Keep the name to 200 characters or fewer.'
    return
  }
  // Nothing changed — close the form without spending a request.
  if (next === (split.value?.name ?? '')) {
    cancelRename()
    return
  }

  savingName.value = true
  try {
    // A partial patch: only the field the user actually edited is sent.
    await store.updateSplit(split.value.id, { name: next })
    cancelRename()
  } catch (e) {
    nameError.value = e.message
  } finally {
    savingName.value = false
  }
}

async function submitMember() {
  if (adding.value) return
  const name  = newName.value.trim()
  const email = newEmail.value.trim()
  addError.value = ''

  if (name.length < 1) {
    addError.value = 'A name is required.'
    return
  }
  if (name.length > 100) {
    addError.value = 'Keep the name to 100 characters or fewer.'
    return
  }
  if (email && !email.includes('@')) {
    addError.value = 'That email doesn’t look right — it needs an @.'
    return
  }

  adding.value = true
  try {
    await store.addMember(split.value.id, { name, email })
    newName.value  = ''
    newEmail.value = ''
  } catch (e) {
    addError.value = e.message
  } finally {
    adding.value = false
  }
}

function resetMemberState() {
  confirming.value      = null
  busy.value            = null
  memberError.value     = ''
  inviteLink.value      = null
  copyState.value       = ''
  confirmingLeave.value = false
  leaveError.value      = ''
}

// ── Seat status ─────────────────────────────────────────────────────────────

function isYou(member) {
  return !!member.user_id && member.user_id === auth.user?.id
}

// Owner and Linked both mean an account holds the seat; a pending invite means
// a link is out but nobody has claimed it yet. Plain names get no badge.
function statusLabel(member) {
  if (member.is_owner) return 'Owner'
  if (member.linked) return 'Linked'
  if (member.invite_pending) return 'Invite pending'
  return ''
}

// ── Confirmed seat actions ──────────────────────────────────────────────────

function isConfirming(member) {
  return confirming.value?.id === member.id
}

function isBusy(member, action) {
  if (busy.value?.id !== member.id) return false
  return action ? busy.value.action === action : true
}

function startConfirm(member, action) {
  memberError.value = ''
  confirming.value  = { id: member.id, action }
}

const PROMPTS = {
  remove:   'Remove?',
  // Both of these invalidate a link the owner may already have sent.
  reinvite: 'Replace the link? The one you sent stops working.',
  revoke:   'Revoke? The link you sent stops working.',
}
const YES   = { remove: 'Yes, remove',  reinvite: 'Yes, new link',  revoke: 'Yes, revoke' }
const BUSY  = { remove: 'Removing…',    reinvite: 'Creating link…', revoke: 'Revoking…', invite: 'Creating link…' }

function confirmPrompt(action) { return PROMPTS[action] ?? 'Are you sure?' }
function confirmYes(action)    { return YES[action] ?? 'Yes' }
function busyLabel(action)     { return BUSY[action] ?? 'Working…' }

function runConfirmed(member) {
  const action = confirming.value?.action
  if (action === 'remove')   return removeMember(member)
  if (action === 'reinvite') return invite(member, { replace: true })
  if (action === 'revoke')   return revokeInvite(member)
}

// Owner-only actions fail with 403 if the caller has stopped being the owner
// since the page loaded. The store's message says so; refetching afterwards
// makes the controls match the role the server now reports.
function handleSeatError(e) {
  memberError.value = e.message
  if (e.status === 403 && split.value) {
    store.fetchSplit(split.value.id).catch(() => {})
  }
}

async function removeMember(member) {
  if (busy.value) return
  busy.value        = { id: member.id, action: 'remove' }
  memberError.value = ''
  try {
    await store.removeMember(split.value.id, member.id)
    confirming.value = null
    if (inviteLink.value?.memberId === member.id) dismissInviteLink()
  } catch (e) {
    handleSeatError(e)
  } finally {
    busy.value = null
  }
}

// ── Invites ─────────────────────────────────────────────────────────────────

// The token comes back exactly once; the member list only ever reports
// `invite_pending`. So the full URL is built here and held until dismissed.
async function invite(member, { replace }) {
  if (busy.value) return
  busy.value        = { id: member.id, action: replace ? 'reinvite' : 'invite' }
  memberError.value = ''
  try {
    const { invite_path } = await store.inviteMember(split.value.id, member.id)
    confirming.value = null
    copyState.value  = ''
    inviteLink.value = {
      memberId: member.id,
      url: window.location.origin + invite_path,
      replaced: replace,
    }
    nextTick(() => inviteInput?.focus())
  } catch (e) {
    handleSeatError(e)
  } finally {
    busy.value = null
  }
}

async function revokeInvite(member) {
  if (busy.value) return
  busy.value        = { id: member.id, action: 'revoke' }
  memberError.value = ''
  try {
    await store.revokeInvite(split.value.id, member.id)
    confirming.value = null
    if (inviteLink.value?.memberId === member.id) dismissInviteLink()
  } catch (e) {
    handleSeatError(e)
  } finally {
    busy.value = null
  }
}

// Only one invite box is ever open, so a single element ref is enough; a
// function ref is used because the input lives inside a v-for.
function setInviteInput(el) {
  inviteInput = el ?? null
}

async function copyInviteLink() {
  if (!inviteLink.value) return
  clearTimeout(copiedTimer)
  try {
    if (!navigator.clipboard?.writeText) throw new Error('no clipboard')
    await navigator.clipboard.writeText(inviteLink.value.url)
    copyState.value = 'copied'
    copiedTimer = setTimeout(() => { if (copyState.value === 'copied') copyState.value = '' }, 2000)
  } catch {
    // No clipboard API (plain-http origins, some embedded browsers): leave the
    // link selected so a manual copy is one keystroke away.
    inviteInput?.focus()
    inviteInput?.select()
    copyState.value = 'manual'
  }
}

function dismissInviteLink() {
  clearTimeout(copiedTimer)
  inviteLink.value = null
  copyState.value  = ''
}

// ── Leave ───────────────────────────────────────────────────────────────────

async function leave() {
  if (leaving.value) return
  leaving.value    = true
  leaveError.value = ''
  const id = split.value.id
  try {
    // The store clears `current` and drops the split from the list itself.
    await store.leaveSplit(id)
    router.push('/')
  } catch (e) {
    leaveError.value = e.message
    leaving.value    = false
  }
}

const TYPE_LABELS = { one_time: 'One-time', ongoing: 'Ongoing' }

</script>
