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

      <!-- 2a. Not found — an id that doesn't exist, or isn't owned by this
              user. The API answers 404 for both so it can't be used to probe
              which split ids exist. -->
      <div v-else-if="notFound" class="card mt-4 text-center py-12">
        <h1 class="text-lg font-medium text-gray-900">Split not found</h1>
        <p class="mt-2 text-sm text-gray-500 max-w-sm mx-auto">
          This split either doesn’t exist or isn’t one of yours. If someone
          shared a link with you, ask them to add you to it.
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

            <button
              v-if="!editingName"
              type="button"
              class="btn-secondary shrink-0 text-xs px-3 py-1.5"
              @click="startRename"
            >
              Rename
            </button>
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
            <li
              v-for="member in members"
              :key="member.id"
              class="flex items-center justify-between gap-4 py-3 first:pt-0"
            >
              <div class="min-w-0">
                <div class="flex items-center gap-2">
                  <span class="text-sm font-medium text-gray-900 truncate">{{ member.name }}</span>
                  <span
                    v-if="accountLabel(member)"
                    class="rounded-full bg-gray-100 text-gray-500 px-2 py-0.5 text-xs"
                  >
                    {{ accountLabel(member) }}
                  </span>
                </div>
                <p v-if="member.email" class="text-xs text-gray-500 truncate">{{ member.email }}</p>
                <p v-if="formatDate(member.joined_at)" class="text-xs text-gray-400">
                  Added {{ formatDate(member.joined_at) }}
                </p>
              </div>

              <!-- Removal is confirmed inline rather than with a dialog, so the
                   row being removed stays visible while you decide. -->
              <div v-if="confirmingId === member.id" class="flex shrink-0 items-center gap-2">
                <span class="text-xs text-gray-600">Remove?</span>
                <button
                  type="button"
                  class="btn-secondary text-xs px-3 py-1.5 border-red-300 text-red-700 hover:bg-red-50"
                  :disabled="removingId === member.id"
                  @click="removeMember(member)"
                >
                  {{ removingId === member.id ? 'Removing…' : 'Yes, remove' }}
                </button>
                <button
                  type="button"
                  class="btn-secondary text-xs px-3 py-1.5"
                  :disabled="removingId === member.id"
                  @click="confirmingId = null"
                >
                  Cancel
                </button>
              </div>
              <button
                v-else
                type="button"
                class="btn-secondary shrink-0 text-xs px-3 py-1.5"
                :disabled="removingId !== null"
                @click="confirmRemove(member)"
              >
                Remove
              </button>
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
              Emails are stored for later — no invite is sent.
            </p>

            <button type="submit" class="btn-primary" :disabled="adding">
              {{ adding ? 'Adding…' : 'Add member' }}
            </button>
          </form>
        </section>

        <!-- Bills are Phase 3. Deliberately no link and no fabricated data. -->
        <section class="card mt-4 border-dashed">
          <h2 class="text-lg font-semibold text-gray-400">Bills</h2>
          <p class="mt-1 text-sm text-gray-500">
            Not yet implemented. Once bills land you’ll add them here and see who
            owes what across this split.
          </p>
        </section>
      </template>
    </div>
  </AppLayout>
</template>

<script setup>
import { ref, nextTick, onMounted, watch } from 'vue'
import { useRoute } from 'vue-router'
import { storeToRefs } from 'pinia'
import AppLayout from '@/components/AppLayout.vue'
import { useSplitsStore } from '@/stores/splits'
import { typeLabel, memberLabel, formatDate } from '@/lib/format'
import { useAuthStore } from '@/stores/auth'

const route = useRoute()
const store = useSplitsStore()
const auth  = useAuthStore()

// `current` is the SplitDetail; `members` is kept in sync by the store's
// add/remove actions, so the list never needs a refetch after a mutation.
const { current: split, members } = storeToRefs(store)

const loading  = ref(true)
const error    = ref('')
const notFound = ref(false)

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

// Remove member
const confirmingId = ref(null)
const removingId   = ref(null)
const memberError  = ref('')

onMounted(load)
// The route is reused when navigating from one split to another, so the id has
// to be watched rather than only read once on mount.
watch(() => route.params.id, load)

async function load() {
  loading.value  = true
  error.value    = ''
  notFound.value = false
  cancelRename()
  confirmingId.value = null
  memberError.value  = ''
  try {
    await store.fetchSplit(route.params.id)
  } catch (e) {
    // The store already normalized this into a user-facing message; the status
    // is what tells 404 apart from a real failure.
    if (e.status === 404) notFound.value = true
    else error.value = e.message
  } finally {
    loading.value = false
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

function confirmRemove(member) {
  memberError.value  = ''
  confirmingId.value = member.id
}

async function removeMember(member) {
  if (removingId.value) return
  removingId.value  = member.id
  memberError.value = ''
  try {
    await store.removeMember(split.value.id, member.id)
    confirmingId.value = null
  } catch (e) {
    memberError.value = e.message
  } finally {
    removingId.value = null
  }
}

// A member with a user_id is backed by a real account; everyone else is just a
// name on a list. Worth marking, quietly.
function accountLabel(member) {
  if (!member.user_id) return ''
  return member.user_id === auth.user?.id ? 'you' : 'account'
}

const TYPE_LABELS = { one_time: 'One-time', ongoing: 'Ongoing' }

</script>
