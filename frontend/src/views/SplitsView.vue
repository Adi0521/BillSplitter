<template>
  <AppLayout>
    <div class="p-6 max-w-3xl mx-auto">
      <!-- Header -->
      <div class="flex items-center justify-between mb-6 gap-4">
        <h1 class="text-2xl font-bold">Splits</h1>
        <div class="flex items-center gap-4">
          <label class="flex items-center gap-2 text-sm text-gray-600 select-none cursor-pointer">
            <input
              type="checkbox"
              v-model="showArchived"
              :disabled="loading"
              class="h-4 w-4 accent-primary-600 cursor-pointer"
            />
            Show archived
          </label>
          <router-link
            to="/splits/new"
            class="rounded-lg bg-primary-600 hover:bg-primary-700 text-white
                   font-medium px-4 py-2 text-sm transition-colors"
          >
            + New Split
          </router-link>
        </div>
      </div>

      <!-- Tabs: a client-side filter on each split's `role` (see docs/api.md). -->
      <div class="mb-4 border-b border-gray-200">
        <nav class="-mb-px flex gap-6" role="tablist" aria-label="Split lists">
          <button
            v-for="t in TABS"
            :key="t.id"
            type="button"
            role="tab"
            :aria-selected="tab === t.id"
            class="flex items-center gap-2 border-b-2 pb-3 text-sm font-medium transition-colors"
            :class="tab === t.id
              ? 'border-primary-600 text-primary-700'
              : 'border-transparent text-gray-500 hover:text-gray-700 hover:border-gray-300'"
            @click="tab = t.id"
          >
            {{ t.label }}
            <span
              class="rounded-full px-2 py-0.5 text-xs tabular-nums"
              :class="tab === t.id ? 'bg-primary-50 text-primary-700' : 'bg-gray-100 text-gray-600'"
            >
              {{ loading ? '–' : counts[t.id] }}
            </span>
          </button>
        </nav>
      </div>

      <!-- An archive or leave that failed shouldn't wipe out the list we already have. -->
      <p
        v-if="actionError"
        class="mb-4 rounded-lg border border-red-200 bg-red-50 px-4 py-2 text-sm text-red-700"
      >
        {{ actionError }}
      </p>

      <!-- 1. Loading -->
      <div v-if="loading" class="space-y-3" aria-busy="true">
        <p class="sr-only">Loading your splits…</p>
        <div v-for="n in 3" :key="n" class="card animate-pulse">
          <div class="h-5 w-1/3 rounded bg-gray-200"></div>
          <div class="mt-3 h-4 w-1/2 rounded bg-gray-100"></div>
        </div>
      </div>

      <!-- 2. Error -->
      <div v-else-if="error" class="card border-red-200 bg-red-50">
        <h2 class="text-sm font-medium text-red-800">Couldn’t load your splits</h2>
        <p class="mt-1 text-sm text-red-700">{{ error }}</p>
        <button class="btn-secondary mt-4" @click="load">Try again</button>
      </div>

      <!-- 3a. Empty — Invited. Nothing to do from here: someone else has to send a link. -->
      <div v-else-if="visible.length === 0 && tab === 'invited'" class="card text-center py-12">
        <h2 class="text-lg font-medium text-gray-900">No invitations yet</h2>
        <p class="mt-2 text-sm text-gray-500 max-w-sm mx-auto">
          Splits someone else created and invited you into show up here. When a
          split’s owner sends you an invite link and you open it while signed in,
          the split appears on this tab.
        </p>
        <p v-if="!showArchived" class="mt-4 text-xs text-gray-400">
          Invited to one that’s since been archived? Tick “Show archived” above.
        </p>
      </div>

      <!-- 3b. Empty — Mine -->
      <div v-else-if="visible.length === 0" class="card text-center py-12">
        <h2 class="text-lg font-medium text-gray-900">
          {{ invited.length ? 'No splits of your own yet' : 'No splits yet' }}
        </h2>
        <p class="mt-2 text-sm text-gray-500 max-w-sm mx-auto">
          A split is a shared pot — a trip, a house, a dinner. Create one, add the
          people in it, and start tracking who owes what.
        </p>
        <router-link to="/splits/new" class="btn-primary inline-block mt-6">
          Create your first split
        </router-link>
        <p v-if="invited.length" class="mt-4 text-xs text-gray-400">
          The {{ pluralize(invited.length, 'split') }} you’ve been invited to
          {{ invited.length === 1 ? 'is' : 'are' }} under
          <button type="button" class="underline hover:text-gray-600" @click="tab = 'invited'">Invited</button>.
        </p>
        <p v-else-if="!showArchived" class="mt-4 text-xs text-gray-400">
          Archived one already? Tick “Show archived” above.
        </p>
      </div>

      <!-- 4. Loaded -->
      <ul v-else class="space-y-3">
        <li
          v-for="split in visible"
          :key="split.id"
          class="card relative transition-colors"
          :class="isArchived(split)
            ? 'bg-gray-50 border-dashed'
            : 'hover:border-primary-500'"
        >
          <div class="flex items-start gap-4">
            <div class="min-w-0 flex-1" :class="isArchived(split) && 'opacity-60'">
              <div class="flex flex-wrap items-center gap-2">
                <h2 class="text-base font-semibold text-gray-900 truncate">
                  <!-- Stretched link: the whole card is clickable, but the action
                       controls below sit above it and swallow their own clicks. -->
                  <router-link
                    :to="`/splits/${split.id}`"
                    class="hover:text-primary-600 before:absolute before:inset-0 before:content-['']"
                  >
                    {{ split.name }}
                  </router-link>
                </h2>
                <span
                  class="rounded-full bg-primary-50 text-primary-700 px-2 py-0.5 text-xs font-medium"
                >
                  {{ typeLabel(split.type) }}
                </span>
                <!-- Only member rows get a role badge; "owner" on every Mine row is noise. -->
                <span
                  v-if="isMember(split)"
                  class="rounded-full bg-amber-50 text-amber-700 px-2 py-0.5 text-xs font-medium"
                >
                  Invited
                </span>
                <span
                  v-if="isArchived(split)"
                  class="rounded-full bg-gray-200 text-gray-600 px-2 py-0.5 text-xs font-medium"
                >
                  Archived
                </span>
              </div>

              <p v-if="split.description" class="mt-1 text-sm text-gray-600 line-clamp-2">
                {{ split.description }}
              </p>

              <div class="mt-2 flex flex-wrap items-center gap-x-2 gap-y-1 text-xs text-gray-500">
                <span>{{ memberLabel(split.member_count) }}</span>
                <span aria-hidden="true">·</span>
                <span>{{ split.currency }}</span>
                <template v-if="formatDate(split.created_at)">
                  <span aria-hidden="true">·</span>
                  <span>Created {{ formatDate(split.created_at) }}</span>
                </template>
              </div>
            </div>

            <!-- Step one of the two-step confirm. Archive is owner-only (a member
                 gets 403), so a member row offers Leave instead. -->
            <button
              v-if="!isArchived(split) && confirmingId !== split.id"
              type="button"
              class="btn-secondary relative shrink-0 text-xs px-3 py-1.5"
              :disabled="busyId === split.id"
              @click.stop.prevent="confirmingId = split.id"
            >
              <template v-if="isMember(split)">
                {{ busyId === split.id ? 'Leaving…' : 'Leave' }}
              </template>
              <template v-else>
                {{ busyId === split.id ? 'Archiving…' : 'Archive' }}
              </template>
            </button>
          </div>

          <!-- Step two: inline confirm, above the stretched link so its buttons
               don't navigate. -->
          <div
            v-if="confirmingId === split.id"
            class="relative mt-4 flex flex-wrap items-center justify-between gap-3
                   rounded-lg border border-gray-200 bg-gray-50 px-4 py-3"
          >
            <p class="text-sm text-gray-700 min-w-0 flex-1">
              <template v-if="isMember(split)">
                <span class="font-medium">Leave “{{ split.name }}”?</span>
                This unlinks your account from your seat. Your payments and shares
                stay in the split’s ledger, and you’ll need a new invite to get back in.
              </template>
              <template v-else>
                <span class="font-medium">Archive “{{ split.name }}”?</span>
                You can still find it under “Show archived”.
              </template>
            </p>
            <div class="flex shrink-0 items-center gap-2">
              <button
                type="button"
                class="btn-secondary text-xs px-3 py-1.5"
                :disabled="busyId === split.id"
                @click.stop.prevent="confirmingId = null"
              >
                Cancel
              </button>
              <button
                type="button"
                class="btn-primary text-xs px-3 py-1.5"
                :disabled="busyId === split.id"
                @click.stop.prevent="isMember(split) ? leave(split) : archive(split)"
              >
                <template v-if="isMember(split)">
                  {{ busyId === split.id ? 'Leaving…' : 'Leave split' }}
                </template>
                <template v-else>
                  {{ busyId === split.id ? 'Archiving…' : 'Archive split' }}
                </template>
              </button>
            </div>
          </div>
        </li>
      </ul>
    </div>
  </AppLayout>
</template>

<script setup>
import { ref, computed, watch, onMounted } from 'vue'
import { storeToRefs } from 'pinia'
import AppLayout from '@/components/AppLayout.vue'
import { useSplitsStore } from '@/stores/splits'
import { typeLabel, memberLabel, formatDate, pluralize } from '@/lib/format'

const store = useSplitsStore()
const { splits } = storeToRefs(store)

const TABS = [
  { id: 'mine',    label: 'Mine' },
  { id: 'invited', label: 'Invited' },
]
const TAB_STORAGE_KEY = 'splits.tab'

const tab          = ref(readStoredTab())
const showArchived = ref(false)
const loading      = ref(true)
const error        = ref('')
const actionError  = ref('')
const confirmingId = ref(null)  // split awaiting step two of Archive/Leave
const busyId       = ref(null)  // split whose Archive/Leave request is in flight

// `role` is the caller's relationship to the split. Anything that isn't
// explicitly "member" is treated as owned so an older backend without the
// field still lists everything under Mine rather than hiding it.
const mine    = computed(() => splits.value.filter(s => !isMember(s)))
const invited = computed(() => splits.value.filter(isMember))
const visible = computed(() => (tab.value === 'invited' ? invited.value : mine.value))
const counts  = computed(() => ({ mine: mine.value.length, invited: invited.value.length }))

onMounted(load)
watch(showArchived, load)
watch(tab, (t) => {
  confirmingId.value = null
  // Per-viewer convenience only. Storage can throw in private windows or when
  // it's disabled, and the tab still works without it.
  try { localStorage.setItem(TAB_STORAGE_KEY, t) } catch { /* ignore */ }
})

function readStoredTab() {
  try {
    const t = localStorage.getItem(TAB_STORAGE_KEY)
    return TABS.some(x => x.id === t) ? t : 'mine'
  } catch {
    return 'mine'
  }
}

async function load() {
  loading.value = true
  error.value = ''
  actionError.value = ''
  confirmingId.value = null
  try {
    await store.fetchSplits({ archived: showArchived.value })
  } catch (e) {
    // The store already normalized this into a user-facing message.
    error.value = e.message
  } finally {
    loading.value = false
  }
}

function isArchived(split) {
  return Boolean(split.archived_at)
}

function isMember(split) {
  return split.role === 'member'
}

async function archive(split) {
  if (busyId.value) return

  busyId.value = split.id
  actionError.value = ''
  try {
    await store.archiveSplit(split.id)
    // archiveSplit drops the row from the list. That is right for the active
    // view, but when archived rows are being shown it should stay put with an
    // Archived badge — so refetch to get its archived_at.
    if (showArchived.value) await store.fetchSplits({ archived: true })
    confirmingId.value = null
  } catch (e) {
    actionError.value = e.message
  } finally {
    busyId.value = null
  }
}

async function leave(split) {
  if (busyId.value) return

  busyId.value = split.id
  actionError.value = ''
  try {
    // Unlinks this account from its seat; the store drops the row. The seat
    // and its ledger entries stay with the split.
    await store.leaveSplit(split.id)
    confirmingId.value = null
  } catch (e) {
    actionError.value = e.message
  } finally {
    busyId.value = null
  }
}
</script>
