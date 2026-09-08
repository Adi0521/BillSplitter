<template>
  <AppLayout>
    <div class="p-6 max-w-3xl mx-auto">
      <router-link
        :to="`/splits/${splitId}`"
        class="text-sm text-gray-500 hover:text-gray-700"
      >
        &larr; {{ split?.name ? `Back to ${split.name}` : 'Back to the split' }}
      </router-link>

      <!-- 1. Loading -->
      <div v-if="loading" class="mt-4 space-y-4" aria-busy="true">
        <p class="sr-only">Loading this bill…</p>
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
              bill ids exist. -->
      <div v-else-if="notFound" class="card mt-4 text-center py-12">
        <h1 class="text-lg font-medium text-gray-900">Bill not found</h1>
        <p class="mt-2 text-sm text-gray-500 max-w-sm mx-auto">
          This bill either doesn’t exist or belongs to a split that isn’t yours.
          It may have been deleted — bills are deleted for real, not archived.
        </p>
        <div class="mt-6 flex justify-center gap-2">
          <router-link :to="`/splits/${splitId}`" class="btn-primary">
            Back to the split
          </router-link>
          <router-link to="/" class="btn-secondary">All splits</router-link>
        </div>
      </div>

      <!-- 2b. Any other failure -->
      <div v-else-if="error" class="card mt-4 border-red-200 bg-red-50">
        <h1 class="text-sm font-medium text-red-800">Couldn’t load this bill</h1>
        <p class="mt-1 text-sm text-red-700">{{ error }}</p>
        <div class="mt-4 flex gap-2">
          <button class="btn-secondary" @click="load">Try again</button>
          <router-link :to="`/splits/${splitId}`" class="btn-secondary">
            Back to the split
          </router-link>
        </div>
      </div>

      <!-- 3. Loaded -->
      <template v-else-if="bill">
        <!-- Header -->
        <div class="card mt-4">
          <div class="flex items-start justify-between gap-4">
            <div class="min-w-0 flex-1">
              <h1 class="text-2xl font-bold break-words">{{ bill.store_name }}</h1>
              <p v-if="billDate" class="mt-1 text-sm text-gray-600">{{ billDate }}</p>
            </div>
            <span
              class="shrink-0 rounded-full bg-gray-100 text-gray-700 px-2 py-0.5 text-xs font-medium"
            >
              {{ bill.currency }}
            </span>
          </div>

          <div class="mt-4 flex flex-wrap items-center gap-2 text-xs">
            <!-- The payer is stored as a member id. Rendering the uuid would be
                 useless, so it is resolved against the split's members. -->
            <span
              v-if="bill.payer_member_id"
              class="rounded-full bg-primary-50 text-primary-700 px-2 py-0.5 font-medium"
            >
              Paid by {{ payerName }}
            </span>
            <span v-else class="rounded-full bg-gray-100 text-gray-500 px-2 py-0.5 font-medium">
              No payer set
            </span>
            <span class="text-gray-500">{{ itemCountLabel }}</span>
          </div>

          <p v-if="membersError" class="mt-3 text-xs text-amber-700">
            Couldn’t load this split’s members ({{ membersError }}), so names may
            show as “Unknown member”.
          </p>
        </div>

        <!-- Items -->
        <section class="card mt-4">
          <div class="flex items-baseline justify-between gap-4">
            <h2 class="text-lg font-semibold">Items</h2>
            <span class="text-xs text-gray-500">Prices are per unit</span>
          </div>

          <p
            v-if="itemError"
            class="mt-3 rounded-lg border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700"
          >
            {{ itemError }}
          </p>

          <!-- Items empty state -->
          <p v-if="items.length === 0" class="mt-4 text-sm text-gray-500">
            No line items on this bill yet, so its subtotal is
            {{ bill.currency }} {{ formatMoney(bill.subtotal) }}. Add the first one
            below — a name and a price is all it takes.
          </p>

          <table v-else class="mt-4 w-full text-sm">
            <thead>
              <tr class="border-b border-gray-200 text-left text-xs uppercase tracking-wide text-gray-500">
                <th scope="col" class="pb-2 font-medium">Item</th>
                <th scope="col" class="pb-2 pl-3 text-right font-medium">Unit price</th>
                <th scope="col" class="pb-2 pl-3 text-right font-medium">Qty</th>
                <th scope="col" class="pb-2 pl-3 text-right font-medium">Line total</th>
                <th scope="col" class="pb-2 pl-3"><span class="sr-only">Actions</span></th>
              </tr>
            </thead>
            <tbody class="divide-y divide-gray-100">
              <template v-for="item in items" :key="item.id">
                <!-- Edit row. A <form> can't wrap a <tr>, so the whole editor
                     lives in one full-width cell instead of split across the
                     columns; it also reads better on a narrow screen. -->
                <tr v-if="editingId === item.id">
                  <td colspan="5" class="py-3">
                    <form class="space-y-3" @submit.prevent="saveItem(item)">
                      <div class="flex flex-col gap-3 sm:flex-row">
                        <div class="flex-1">
                          <label class="label" :for="`edit-name-${item.id}`">Name</label>
                          <input
                            :id="`edit-name-${item.id}`"
                            ref="editNameInput"
                            v-model="editDraft.name"
                            type="text"
                            maxlength="200"
                            class="input"
                            :disabled="savingItemId === item.id"
                          />
                        </div>
                        <div class="sm:w-36">
                          <label class="label" :for="`edit-price-${item.id}`">
                            Unit price ({{ item.currency }})
                          </label>
                          <input
                            :id="`edit-price-${item.id}`"
                            v-model="editDraft.price"
                            type="text"
                            inputmode="decimal"
                            class="input text-right"
                            :disabled="savingItemId === item.id"
                          />
                        </div>
                        <div class="sm:w-24">
                          <label class="label" :for="`edit-qty-${item.id}`">Qty</label>
                          <input
                            :id="`edit-qty-${item.id}`"
                            v-model="editDraft.quantity"
                            type="text"
                            inputmode="numeric"
                            class="input text-right"
                            :disabled="savingItemId === item.id"
                          />
                        </div>
                      </div>

                      <p v-if="editError" class="text-sm text-red-600">{{ editError }}</p>

                      <div class="flex gap-2">
                        <button type="submit" class="btn-primary" :disabled="savingItemId === item.id">
                          {{ savingItemId === item.id ? 'Saving…' : 'Save item' }}
                        </button>
                        <button
                          type="button"
                          class="btn-secondary"
                          :disabled="savingItemId === item.id"
                          @click="cancelEdit"
                        >
                          Cancel
                        </button>
                      </div>
                    </form>
                  </td>
                </tr>

                <!-- Display row -->
                <tr v-else class="align-top">
                  <td class="py-3 pr-3">
                    <span class="font-medium text-gray-900 break-words">{{ item.name }}</span>
                    <span
                      v-if="item.currency !== bill.currency"
                      class="ml-2 rounded-full bg-amber-50 text-amber-700 px-2 py-0.5 text-xs"
                    >
                      {{ item.currency }}
                    </span>
                    <!-- Only shown once this item's allocations have been loaded
                         (i.e. its allocator has been opened at least once). The
                         authoritative picture is the "Who owes what" panel; this
                         is a reminder of what you just did, so it is deliberately
                         absent rather than guessed at for items never opened. -->
                    <p v-if="allocSet(item)" class="mt-1 text-xs text-gray-500">
                      <template v-if="allocSet(item).allocations.length">
                        {{ allocNames(item) }}
                      </template>
                      <template v-else>Nobody assigned</template>
                      <span
                        v-if="!isZeroAmount(allocSet(item).unallocated)"
                        class="text-amber-700"
                      >
                        · {{ formatMoney(allocSet(item).unallocated) }} unallocated
                      </span>
                    </p>
                  </td>
                  <td class="py-3 pl-3 text-right tabular-nums text-gray-600">
                    {{ formatMoney(item.price) }}
                  </td>
                  <td class="py-3 pl-3 text-right tabular-nums text-gray-600">
                    {{ item.quantity }}
                  </td>
                  <td class="py-3 pl-3 text-right tabular-nums font-medium text-gray-900">
                    {{ formatMoney(item.line_total) }}
                  </td>
                  <td class="py-3 pl-3 text-right">
                    <!-- Deletion is confirmed inline rather than in a dialog, so
                         the row you're deleting stays on screen while you decide. -->
                    <div v-if="confirmingId === item.id" class="flex justify-end items-center gap-2">
                      <span class="text-xs text-gray-600 whitespace-nowrap">Delete?</span>
                      <button
                        type="button"
                        class="btn-secondary text-xs px-3 py-1.5 border-red-300 text-red-700 hover:bg-red-50"
                        :disabled="deletingId === item.id"
                        @click="deleteItem(item)"
                      >
                        {{ deletingId === item.id ? 'Deleting…' : 'Yes, delete' }}
                      </button>
                      <button
                        type="button"
                        class="btn-secondary text-xs px-3 py-1.5"
                        :disabled="deletingId === item.id"
                        @click="confirmingId = null"
                      >
                        Cancel
                      </button>
                    </div>
                    <div v-else class="flex justify-end gap-2">
                      <!-- Hidden with no members: there is nobody to allocate to,
                           and the panel below explains how to fix that. -->
                      <button
                        v-if="members.length > 0"
                        type="button"
                        class="btn-secondary text-xs px-3 py-1.5"
                        :class="allocatingId === item.id ? 'border-primary-500 bg-primary-50 text-primary-700' : ''"
                        :aria-expanded="allocatingId === item.id"
                        :disabled="itemBusy"
                        @click="toggleAllocator(item)"
                      >
                        {{ allocatingId === item.id ? 'Done' : 'Allocate' }}
                      </button>
                      <button
                        type="button"
                        class="btn-secondary text-xs px-3 py-1.5"
                        :disabled="itemBusy"
                        @click="startEdit(item)"
                      >
                        Edit
                      </button>
                      <button
                        type="button"
                        class="btn-secondary text-xs px-3 py-1.5"
                        :disabled="itemBusy"
                        @click="confirmDelete(item)"
                      >
                        Delete
                      </button>
                    </div>
                  </td>
                </tr>

                <!-- Allocation editor for one item, expanded under its row. Same
                     full-width-cell shape as the item editor above; opening it
                     closes any open editor, so the two never stack. -->
                <tr v-if="allocatingId === item.id" class="bg-gray-50/70">
                  <td colspan="5" class="py-3">
                    <ItemAllocator
                      :bill-id="billId"
                      :item="item"
                      :members="members"
                      @changed="onAllocationChanged"
                    />
                  </td>
                </tr>
              </template>
            </tbody>
          </table>

          <!-- Add item -->
          <form class="mt-6 border-t border-gray-100 pt-4 space-y-3" @submit.prevent="submitItem">
            <div class="flex flex-col gap-3 sm:flex-row">
              <div class="flex-1">
                <label class="label" for="new-item-name">Item</label>
                <input
                  id="new-item-name"
                  v-model="newItem.name"
                  type="text"
                  maxlength="200"
                  placeholder="Olive oil"
                  class="input"
                  :disabled="adding"
                />
              </div>
              <div class="sm:w-36">
                <label class="label" for="new-item-price">
                  Unit price ({{ bill.currency }})
                </label>
                <input
                  id="new-item-price"
                  v-model="newItem.price"
                  type="text"
                  inputmode="decimal"
                  placeholder="12.50"
                  class="input text-right"
                  :disabled="adding"
                />
              </div>
              <div class="sm:w-24">
                <label class="label" for="new-item-qty">
                  Qty <span class="text-gray-400">(opt.)</span>
                </label>
                <input
                  id="new-item-qty"
                  v-model="newItem.quantity"
                  type="text"
                  inputmode="numeric"
                  placeholder="1"
                  class="input text-right"
                  :disabled="adding"
                />
              </div>
            </div>

            <p v-if="addError" class="text-sm text-red-600">{{ addError }}</p>
            <p class="text-xs text-gray-400">
              Enter the price for one unit — the line total is worked out for you.
            </p>

            <button type="submit" class="btn-primary" :disabled="adding">
              {{ adding ? 'Adding…' : 'Add item' }}
            </button>
          </form>
        </section>

        <!-- Totals. Every number here is the string the server sent. The client
             deliberately does not sum items or add tax to the subtotal: these
             are NUMERIC(12,4) and JS floats would drift (see docs/api.md,
             "Money representation"). -->
        <section class="card mt-4">
          <div class="flex items-baseline justify-between gap-4">
            <h2 class="text-lg font-semibold">Totals</h2>
            <button
              v-if="!editingAmounts"
              type="button"
              class="btn-secondary text-xs px-3 py-1.5"
              @click="startAmounts"
            >
              Edit tax, tip &amp; fees
            </button>
          </div>

          <dl class="mt-4 space-y-2 text-sm">
            <div class="flex justify-between gap-4">
              <dt class="text-gray-600">Subtotal <span class="text-gray-400">(from items)</span></dt>
              <dd class="tabular-nums text-gray-900">{{ formatMoney(bill.subtotal) }}</dd>
            </div>
            <div class="flex justify-between gap-4">
              <dt class="text-gray-600">Tax</dt>
              <dd class="tabular-nums text-gray-900">{{ formatMoney(bill.tax) }}</dd>
            </div>
            <div class="flex justify-between gap-4">
              <dt class="text-gray-600">Tip</dt>
              <dd class="tabular-nums text-gray-900">{{ formatMoney(bill.tip) }}</dd>
            </div>
            <div class="flex justify-between gap-4">
              <dt class="text-gray-600">Fees</dt>
              <dd class="tabular-nums text-gray-900">{{ formatMoney(bill.fees) }}</dd>
            </div>
            <div class="flex justify-between gap-4 border-t border-gray-200 pt-2 text-base font-semibold">
              <dt>Total</dt>
              <dd class="tabular-nums">{{ bill.currency }} {{ formatMoney(bill.total) }}</dd>
            </div>
          </dl>

          <!-- Only tax, tip, fees and the payer are user-entered. Subtotal and
               total are derived server-side and a request that sends either is
               a 400, so neither is ever put in an input. -->
          <form
            v-if="editingAmounts"
            class="mt-6 border-t border-gray-100 pt-4 space-y-3"
            @submit.prevent="saveAmounts"
          >
            <div class="flex flex-col gap-3 sm:flex-row">
              <div class="flex-1">
                <label class="label" for="bill-tax">Tax</label>
                <input
                  id="bill-tax"
                  ref="taxInput"
                  v-model="amountDraft.tax"
                  type="text"
                  inputmode="decimal"
                  class="input text-right"
                  :disabled="savingAmounts"
                />
              </div>
              <div class="flex-1">
                <label class="label" for="bill-tip">Tip</label>
                <input
                  id="bill-tip"
                  v-model="amountDraft.tip"
                  type="text"
                  inputmode="decimal"
                  class="input text-right"
                  :disabled="savingAmounts"
                />
              </div>
              <div class="flex-1">
                <label class="label" for="bill-fees">Fees</label>
                <input
                  id="bill-fees"
                  v-model="amountDraft.fees"
                  type="text"
                  inputmode="decimal"
                  class="input text-right"
                  :disabled="savingAmounts"
                />
              </div>
            </div>

            <div>
              <label class="label" for="bill-payer">Paid by</label>
              <select
                id="bill-payer"
                v-model="amountDraft.payer"
                class="input"
                :disabled="savingAmounts || members.length === 0"
              >
                <option value="">No payer set</option>
                <option v-for="m in members" :key="m.id" :value="m.id">{{ m.name }}</option>
              </select>
              <p v-if="members.length === 0" class="mt-1 text-xs text-gray-500">
                This split has no members yet, so there’s nobody to credit.
                <router-link :to="`/splits/${splitId}`" class="text-primary-600 hover:text-primary-700">
                  Add one first.
                </router-link>
              </p>
            </div>

            <p v-if="amountError" class="text-sm text-red-600">{{ amountError }}</p>

            <div class="flex gap-2">
              <button type="submit" class="btn-primary" :disabled="savingAmounts">
                {{ savingAmounts ? 'Saving…' : 'Save' }}
              </button>
              <button
                type="button"
                class="btn-secondary"
                :disabled="savingAmounts"
                @click="cancelAmounts"
              >
                Cancel
              </button>
            </div>
          </form>
        </section>

        <!-- Who owes what. Every figure in this section — each member's item
             share, their proportional slice of tax/tip/fees, and the
             unallocated remainder — is a NUMERIC string the server computed.
             Nothing here is summed, netted or reconciled in JS: see
             docs/api.md, "The rounding rule". -->
        <section class="card mt-4">
          <div class="flex items-baseline justify-between gap-4">
            <h2 class="text-lg font-semibold">Who owes what</h2>
            <button
              type="button"
              class="btn-secondary text-xs px-3 py-1.5"
              :disabled="sharesLoading"
              @click="loadShares"
            >
              {{ sharesLoading ? 'Refreshing…' : 'Refresh' }}
            </button>
          </div>

          <!-- Loading. Only replaces the panel on a first load; a refresh keeps
               the previous figures on screen rather than flashing a skeleton. -->
          <div v-if="sharesLoading && !shares" class="mt-4 space-y-3 animate-pulse" aria-busy="true">
            <p class="sr-only">Working out each member’s share…</p>
            <div class="h-4 w-1/2 rounded bg-gray-100"></div>
            <div class="h-4 w-2/3 rounded bg-gray-100"></div>
            <div class="h-4 w-2/5 rounded bg-gray-100"></div>
          </div>

          <!-- Error, scoped to this panel: the bill, its items and its totals
               above have already loaded and stay readable. -->
          <div
            v-else-if="sharesError"
            class="mt-4 rounded-lg border border-red-200 bg-red-50 px-3 py-3"
          >
            <p class="text-sm font-medium text-red-800">Couldn’t work out the shares</p>
            <p class="mt-1 text-sm text-red-700">{{ sharesError }}</p>
            <button type="button" class="btn-secondary mt-3 text-xs px-3 py-1.5" @click="loadShares">
              Try again
            </button>
          </div>

          <template v-else-if="shares">
            <!-- No members: nothing can be allocated at all, so say that and
                 point at the one page where it gets fixed. -->
            <p
              v-if="shares.members.length === 0"
              class="mt-3 rounded-lg bg-gray-50 px-3 py-3 text-sm text-gray-600"
            >
              This split has no members yet, so the whole
              {{ shares.currency }} {{ formatMoney(shares.total) }} is unallocated
              and there is nobody to bill.
              <router-link
                :to="`/splits/${splitId}`"
                class="font-medium text-primary-600 hover:text-primary-700"
              >
                Add members to the split
              </router-link>
              first, then allocate each item above.
            </p>

            <template v-else>
              <!-- The headline: who settles up with whom. `owes_payer` is the
                   server's figure, not this member's total minus anything. -->
              <template v-if="shares.payer_member_id">
                <p class="mt-1 text-sm text-gray-500">
                  {{ sharesPayerName }} paid, so everyone else settles up with them.
                </p>
                <ul class="mt-3 space-y-1 text-sm">
                  <li v-for="m in owingMembers" :key="m.member_id" class="text-gray-800">
                    <span class="font-medium">{{ m.name }}</span> owes
                    <span class="font-medium">{{ sharesPayerName }}</span>{{ ' ' }}
                    <span class="tabular-nums">
                      {{ shares.currency }} {{ formatMoney(m.owes_payer) }}
                    </span>
                  </li>
                  <li v-if="owingMembers.length === 0" class="text-gray-500">
                    Nobody owes {{ sharesPayerName }} anything yet — no items on this
                    bill are allocated to anyone else.
                  </li>
                </ul>
              </template>
              <p v-else class="mt-1 text-sm text-gray-500">
                No payer is set on this bill, so there is nobody to pay back — these
                are each member’s plain shares. Set who paid under Totals to turn
                them into “owes”.
              </p>

              <p
                v-if="!isZeroAmount(shares.unallocated.total)"
                class="mt-3 rounded-lg border border-amber-200 bg-amber-50 px-3 py-2 text-sm text-amber-800"
              >
                {{ shares.currency }} {{ formatMoney(shares.unallocated.total) }} of
                this bill is allocated to nobody — including
                {{ formatMoney(shares.unallocated.items) }} of items. Allocate the
                remaining items above if that is not deliberate.
              </p>

              <!-- Seven money columns don't fit a phone; scrolling the table
                   beats dropping a column someone needs. -->
              <div class="mt-4 overflow-x-auto">
                <table class="w-full text-sm">
                  <thead>
                    <tr class="border-b border-gray-200 text-left text-xs uppercase tracking-wide text-gray-500">
                      <th scope="col" class="pb-2 font-medium">Member</th>
                      <th scope="col" class="pb-2 pl-3 text-right font-medium">Items</th>
                      <th scope="col" class="pb-2 pl-3 text-right font-medium">Tax</th>
                      <th scope="col" class="pb-2 pl-3 text-right font-medium">Tip</th>
                      <th scope="col" class="pb-2 pl-3 text-right font-medium">Fees</th>
                      <th scope="col" class="pb-2 pl-3 text-right font-medium">Total</th>
                      <th scope="col" class="pb-2 pl-3 text-right font-medium whitespace-nowrap">
                        {{ shares.payer_member_id ? `Owes ${sharesPayerName}` : 'Owes payer' }}
                      </th>
                    </tr>
                  </thead>
                  <tbody class="divide-y divide-gray-100">
                    <!-- Keyed by member_id, never by name: two members of one
                         split may legitimately share a name. -->
                    <tr v-for="m in shares.members" :key="m.member_id">
                      <td class="py-2 pr-3">
                        <span class="font-medium text-gray-900 break-words">{{ m.name }}</span>
                        <span
                          v-if="m.member_id === shares.payer_member_id"
                          class="ml-2 rounded-full bg-primary-50 px-2 py-0.5 text-xs font-medium text-primary-700"
                        >
                          paid
                        </span>
                      </td>
                      <td class="py-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(m.items) }}</td>
                      <td class="py-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(m.tax) }}</td>
                      <td class="py-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(m.tip) }}</td>
                      <td class="py-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(m.fees) }}</td>
                      <td class="py-2 pl-3 text-right tabular-nums font-medium text-gray-900">{{ formatMoney(m.total) }}</td>
                      <td class="py-2 pl-3 text-right tabular-nums text-gray-900">
                        <span v-if="shares.payer_member_id">{{ formatMoney(m.owes_payer) }}</span>
                        <span v-else class="text-gray-400">—</span>
                      </td>
                    </tr>

                    <!-- Unallocated is a row of its own and stays visible even at
                         zero. Folding it into a member, or hiding it, is exactly
                         the bug this design exists to prevent: an under-allocated
                         bill must look under-allocated. -->
                    <tr :class="isZeroAmount(shares.unallocated.total) ? '' : 'bg-amber-50'">
                      <td class="py-2 pr-3">
                        <span
                          class="font-medium"
                          :class="isZeroAmount(shares.unallocated.total) ? 'text-gray-500' : 'text-amber-800'"
                        >
                          Unallocated
                        </span>
                      </td>
                      <td class="py-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(shares.unallocated.items) }}</td>
                      <td class="py-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(shares.unallocated.tax) }}</td>
                      <td class="py-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(shares.unallocated.tip) }}</td>
                      <td class="py-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(shares.unallocated.fees) }}</td>
                      <td class="py-2 pl-3 text-right tabular-nums font-medium text-gray-900">{{ formatMoney(shares.unallocated.total) }}</td>
                      <td class="py-2 pl-3 text-right text-gray-400">—</td>
                    </tr>
                  </tbody>
                  <tfoot>
                    <tr class="border-t border-gray-200 text-sm">
                      <td class="pt-2 pr-3 font-semibold text-gray-900">Bill</td>
                      <td class="pt-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(shares.subtotal) }}</td>
                      <td class="pt-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(shares.tax) }}</td>
                      <td class="pt-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(shares.tip) }}</td>
                      <td class="pt-2 pl-3 text-right tabular-nums text-gray-600">{{ formatMoney(shares.fees) }}</td>
                      <td class="pt-2 pl-3 text-right tabular-nums font-semibold text-gray-900">{{ formatMoney(shares.total) }}</td>
                      <td class="pt-2 pl-3 text-right text-gray-400">—</td>
                    </tr>
                  </tfoot>
                </table>
              </div>

              <p class="mt-3 text-xs text-gray-500">
                Tax, tip and fees are split in proportion to each member’s share of
                the <em>full</em> subtotal, so items nobody is on the hook for keep
                their slice of them in the unallocated row.
              </p>
              <p class="mt-1 text-xs text-gray-400">
                Every figure is rounded to four decimal places by the server and
                shown to two, so the rows need not add up to the bill exactly. The
                residual is reported rather than papered over — nothing on this
                page is a client-side sum.
              </p>
            </template>
          </template>
        </section>
      </template>
    </div>
  </AppLayout>
</template>

<script setup>
import { ref, reactive, computed, nextTick, onMounted, watch } from 'vue'
import { useRoute } from 'vue-router'
import { storeToRefs } from 'pinia'
import AppLayout from '@/components/AppLayout.vue'
import ItemAllocator from '@/components/ItemAllocator.vue'
import { useBillsStore } from '@/stores/bills'
import { useSplitsStore } from '@/stores/splits'
import { useAllocationsStore } from '@/stores/allocations'
import { formatDay, formatMoney, itemLabel } from '@/lib/format'

const route       = useRoute()
const bills       = useBillsStore()
const splits      = useSplitsStore()
const allocations = useAllocationsStore()

// `current`/`items` are kept in sync by the store's own actions, including the
// refetch of the server-derived subtotal and total after every item mutation.
const { current, items } = storeToRefs(bills)
const { current: split, members } = storeToRefs(splits)
// `byItem` is filled in by ItemAllocator through the same store; the view only
// reads it to label a row it has already been opened on.
const { shares: sharesData, byItem: allocationsByItem } = storeToRefs(allocations)

const splitId = computed(() => route.params.splitId)
const billId  = computed(() => route.params.billId)

// Guards against a flash of the previously-viewed bill while a new one loads:
// the store's `current` is shared, so only render it once it is *this* bill.
const bill = computed(() => (current.value?.id === billId.value ? current.value : null))

// Same guard for the shares: the store holds one BillShares at a time, so a
// stale one from the previously viewed bill must not be rendered against this
// one's items.
const shares = computed(() => (sharesData.value?.bill_id === billId.value ? sharesData.value : null))

const loading      = ref(true)
const error        = ref('')
const notFound     = ref(false)
const membersError = ref('')

// Add item
const newItem  = reactive({ name: '', price: '', quantity: '' })
const adding   = ref(false)
const addError = ref('')

// Edit item
const editingId     = ref(null)
const editDraft     = reactive({ name: '', price: '', quantity: '' })
const editError     = ref('')
const savingItemId  = ref(null)
const editNameInput = ref(null)

// Delete item
const confirmingId = ref(null)
const deletingId   = ref(null)
const itemError    = ref('')

// Per-item allocation. One allocator open at a time, mirroring the item editor.
const allocatingId = ref(null)

// Who owes what. Its own loading/error pair so a failed shares request leaves
// the bill, its items and its totals on screen.
const sharesLoading = ref(false)
const sharesError   = ref('')

// Edit tax / tip / fees / payer
const editingAmounts = ref(false)
const amountDraft    = reactive({ tax: '', tip: '', fees: '', payer: '' })
const amountError    = ref('')
const savingAmounts  = ref(false)
const taxInput       = ref(null)

// One item mutation at a time, so a stray click can't race a save.
const itemBusy = computed(() => savingItemId.value !== null || deletingId.value !== null)

const payerName = computed(() => {
  const id = bill.value?.payer_member_id
  if (!id) return ''
  // A member can be removed from a split while still named as a bill's payer,
  // and the members list may itself have failed to load. Either way, a label
  // beats leaking a uuid into the page.
  return members.value.find(m => m.id === id)?.name ?? 'Unknown member'
})

// The payer's name for the shares panel. Taken from the shares response first,
// which joins it in, so the panel still reads properly when the split's members
// failed to load separately.
const sharesPayerName = computed(() => {
  const id = shares.value?.payer_member_id
  if (!id) return ''
  return (
    shares.value.members.find(m => m.member_id === id)?.name ??
    members.value.find(m => m.id === id)?.name ??
    'Unknown member'
  )
})

// Members with something to settle. Filtering on "is this string zero" is a
// display decision, not arithmetic — see `isZeroAmount`.
const owingMembers = computed(() =>
  (shares.value?.members ?? []).filter(m => !isZeroAmount(m.owes_payer)),
)

// `date` is a plain calendar date ("2026-08-30"), not a timestamp. Feeding it
// to Date() bare parses it as UTC midnight, which renders as the day before in
// any negative-offset timezone; appending a time makes the shared formatter
// parse it as local midnight instead.
const billDate = computed(() => formatDay(bill.value?.date))

const itemCountLabel = computed(() => itemLabel(items.value.length))

onMounted(load)
// The route component is reused when moving between bills, so both params have
// to be watched rather than read once on mount.
watch(() => [route.params.splitId, route.params.billId], load)

async function load() {
  loading.value      = true
  error.value        = ''
  notFound.value     = false
  membersError.value = ''
  resetForms()

  // The bill and the split are independent requests: the members are only
  // needed to put a name on `payer_member_id`, so losing them must not blank
  // out a bill that loaded perfectly well.
  // Drop the previous bill's allocation sets and shares outright; the guards
  // above only stop them rendering, they don't stop them lingering.
  allocations.reset()
  allocatingId.value = null

  const [billResult, splitResult] = await Promise.allSettled([
    bills.fetchBill(splitId.value, billId.value),
    splits.fetchSplit(splitId.value),
  ])

  if (billResult.status === 'rejected') {
    const e = billResult.reason
    // The store normalized this into a user-facing message; the status is what
    // tells a missing bill apart from a real failure.
    if (e.status === 404) notFound.value = true
    else error.value = e.message
  } else if (splitResult.status === 'rejected') {
    membersError.value = splitResult.reason.message
  }

  loading.value = false

  // Fired without awaiting: the shares panel renders its own loading state, so
  // the bill doesn't wait on a second round trip. Pointless if there is no bill.
  if (billResult.status === 'fulfilled') loadShares()
}

// ── Who owes what ───────────────────────────────────────────────────────────

// Guards against out-of-order responses: allocating an item fires a refresh
// while another may still be in flight, and the older reply must not decide
// what the panel says.
let sharesRequest = 0

async function loadShares() {
  const seq = ++sharesRequest
  sharesLoading.value = true
  sharesError.value   = ''
  try {
    await allocations.fetchShares(splitId.value, billId.value)
  } catch (e) {
    if (seq === sharesRequest) sharesError.value = e.message
  } finally {
    if (seq === sharesRequest) sharesLoading.value = false
  }
}

// Anything that moves money — an allocation, an item, tax/tip/fees, the payer —
// changes what everyone owes, so the panel is refetched rather than patched.
function refreshShares() {
  if (bill.value) loadShares()
}

function toggleAllocator(item) {
  cancelEdit()
  confirmingId.value = null
  allocatingId.value = allocatingId.value === item.id ? null : item.id
}

function onAllocationChanged() {
  refreshShares()
}

function allocSet(item) {
  return allocationsByItem.value[item.id] ?? null
}

function allocNames(item) {
  return (allocSet(item)?.allocations ?? []).map(a => a.member_name).join(', ')
}

// True for "0", "0.00", "0.0000" — a string test on purpose. Turning these into
// Numbers to compare against 0 would be the float round trip the whole money
// path avoids, and this is only ever used to decide what to show, never what
// to send.
function isZeroAmount(raw) {
  const s = String(raw ?? '').trim()
  return s === '' || /^-?0*(\.0*)?$/.test(s)
}

function resetForms() {
  newItem.name = ''
  newItem.price = ''
  newItem.quantity = ''
  addError.value = ''
  cancelEdit()
  cancelAmounts()
  confirmingId.value = null
  itemError.value = ''
}

// ── Amount handling ─────────────────────────────────────────────────────────
// Money arrives as a NUMERIC(12,4) string ("12.5000"). Everything below treats
// it as a string; the only place a Number is produced is `formatMoney()`, for
// display. What the user typed is what gets sent.

// Prefills an edit field from a stored amount without changing its value.
// `formatMoney()` must never be used for this: it rounds to two places, so opening
// the tax editor on a stored "3.8350" and pressing Save would quietly write
// 3.84. Here the digits are only trimmed, never rounded — trailing zeros go,
// but the field is padded back out to cents so it still reads like a price.
function editableAmount(raw) {
  const s = String(raw ?? '').trim()
  if (!s || !s.includes('.')) return s
  const [whole, fraction] = s.split('.')
  let dec = fraction.replace(/0+$/, '')
  if (dec.length < 2) dec = (dec + '00').slice(0, 2)
  return `${whole}.${dec}`
}

// Pads a validated amount to the server's 4dp form using string operations, so
// two spellings of the same amount ("3.83" and "3.8300") can be compared for
// equality without float arithmetic. Only ever called after `amountProblem`
// has confirmed there are at most 4 decimal places, so nothing is truncated.
function to4dp(raw) {
  const s = String(raw ?? '').trim()
  if (!s) return null
  const [int = '', dec = ''] = s.split('.')
  const whole = (int.replace(/^0+(?=\d)/, '') || '0')
  return `${whole}.${(dec + '0000').slice(0, 4)}`
}

// Returns a message when the amount is unusable, '' when it is fine. Mirrors
// the server's rules (docs/api.md) so the common mistakes are caught without a
// round trip; the server remains the authority.
function amountProblem(raw, { label, required }) {
  const s = String(raw ?? '').trim()
  if (!s) return required ? `${label} is required.` : ''
  if (!/^(\d+(\.\d+)?|\.\d+)$/.test(s)) {
    return `${label} must be a plain number like 12.50 — no currency symbols, and no negatives.`
  }
  const decimals = s.split('.')[1] ?? ''
  if (decimals.length > 4) return `${label} can have at most 4 decimal places.`
  if (Number(s) > 99999999.9999) return `${label} is too large.`
  return ''
}

function quantityProblem(raw) {
  const s = String(raw ?? '').trim()
  if (!s) return ''            // optional; the server defaults it to 1
  if (!/^\d+$/.test(s)) return 'Quantity must be a whole number.'
  const n = Number(s)
  if (n < 1) return 'Quantity must be at least 1.'
  if (n > 100000) return 'Quantity must be 100000 or less.'
  return ''
}

// ── Items ───────────────────────────────────────────────────────────────────

async function submitItem() {
  if (adding.value) return
  const name = newItem.name.trim()
  // Trimmed, but otherwise exactly the characters the user typed: parseFloat
  // here would turn "0.1" into a double and hand the server a rounded amount.
  const price = newItem.price.trim()
  const quantity = newItem.quantity.trim()
  addError.value = ''

  if (name.length < 1) { addError.value = 'An item name is required.'; return }
  if (name.length > 200) { addError.value = 'Keep the name to 200 characters or fewer.'; return }

  const priceProblem = amountProblem(price, { label: 'Price', required: true })
  if (priceProblem) { addError.value = priceProblem; return }

  const qtyProblem = quantityProblem(quantity)
  if (qtyProblem) { addError.value = qtyProblem; return }

  const payload = { name, price }
  if (quantity) payload.quantity = Number(quantity)

  adding.value = true
  try {
    await bills.addItem(splitId.value, billId.value, payload)
    newItem.name = ''
    newItem.price = ''
    newItem.quantity = ''
    // A new item is unallocated, and it moves the subtotal every proportional
    // slice is computed against.
    refreshShares()
  } catch (e) {
    addError.value = e.message
  } finally {
    adding.value = false
  }
}

function startEdit(item) {
  allocatingId.value = null
  confirmingId.value = null
  itemError.value = ''
  editError.value = ''
  editingId.value = item.id
  editDraft.name = item.name
  editDraft.price = editableAmount(item.price)
  editDraft.quantity = String(item.quantity)
  nextTick(() => {
    const el = editNameInput.value
    ;(Array.isArray(el) ? el[0] : el)?.focus()
  })
}

function cancelEdit() {
  editingId.value = null
  editError.value = ''
  editDraft.name = ''
  editDraft.price = ''
  editDraft.quantity = ''
}

async function saveItem(item) {
  if (savingItemId.value) return
  const name = editDraft.name.trim()
  const price = editDraft.price.trim()
  const quantity = editDraft.quantity.trim()
  editError.value = ''

  if (name.length < 1) { editError.value = 'An item name is required.'; return }
  if (name.length > 200) { editError.value = 'Keep the name to 200 characters or fewer.'; return }

  const priceProblem = amountProblem(price, { label: 'Price', required: true })
  if (priceProblem) { editError.value = priceProblem; return }

  const qtyProblem = quantityProblem(quantity)
  if (qtyProblem) { editError.value = qtyProblem; return }
  if (!quantity) { editError.value = 'Quantity is required — use 1 for a single unit.'; return }

  // A partial patch: only fields the user actually changed. Comparing prices
  // in their padded 4dp string form means retyping "3.83" over a stored
  // "3.8300" counts as no change instead of a pointless write.
  const patch = {}
  if (name !== item.name) patch.name = name
  if (to4dp(price) !== to4dp(item.price)) patch.price = price
  if (Number(quantity) !== Number(item.quantity)) patch.quantity = Number(quantity)

  if (Object.keys(patch).length === 0) { cancelEdit(); return }

  savingItemId.value = item.id
  try {
    await bills.updateItem(splitId.value, billId.value, item.id, patch)
    cancelEdit()
    refreshShares()
  } catch (e) {
    editError.value = e.message
  } finally {
    savingItemId.value = null
  }
}

function confirmDelete(item) {
  itemError.value = ''
  confirmingId.value = item.id
}

async function deleteItem(item) {
  if (deletingId.value) return
  deletingId.value = item.id
  itemError.value = ''
  try {
    await bills.removeItem(splitId.value, billId.value, item.id)
    confirmingId.value = null
    if (allocatingId.value === item.id) allocatingId.value = null
    refreshShares()
  } catch (e) {
    itemError.value = e.message
  } finally {
    deletingId.value = null
  }
}

// ── Tax / tip / fees / payer ────────────────────────────────────────────────

function startAmounts() {
  amountError.value = ''
  amountDraft.tax   = editableAmount(bill.value?.tax)
  amountDraft.tip   = editableAmount(bill.value?.tip)
  amountDraft.fees  = editableAmount(bill.value?.fees)
  amountDraft.payer = bill.value?.payer_member_id ?? ''
  editingAmounts.value = true
  nextTick(() => taxInput.value?.focus())
}

function cancelAmounts() {
  editingAmounts.value = false
  amountError.value = ''
}

async function saveAmounts() {
  if (savingAmounts.value) return
  amountError.value = ''

  const fields = [
    ['tax',  'Tax',  amountDraft.tax],
    ['tip',  'Tip',  amountDraft.tip],
    ['fees', 'Fees', amountDraft.fees],
  ]

  for (const [, label, raw] of fields) {
    const problem = amountProblem(raw, { label, required: true })
    if (problem) { amountError.value = problem; return }
  }

  // Only what changed. `subtotal` and `total` are derived server-side and are
  // rejected with a 400 if sent, so they are never part of this patch.
  const patch = {}
  for (const [key, , raw] of fields) {
    const typed = String(raw).trim()
    if (to4dp(typed) !== to4dp(bill.value[key])) patch[key] = typed
  }

  const payer = amountDraft.payer || null
  if (payer !== (bill.value.payer_member_id ?? null)) patch.payer_member_id = payer

  if (Object.keys(patch).length === 0) { cancelAmounts(); return }

  savingAmounts.value = true
  try {
    await bills.updateBill(splitId.value, billId.value, patch)
    cancelAmounts()
    refreshShares()
  } catch (e) {
    amountError.value = e.message
  } finally {
    savingAmounts.value = false
  }
}
</script>
