<template>
  <AppLayout>
    <div class="p-6 max-w-3xl mx-auto">
      <!-- Header -->
      <div class="flex items-center justify-between mb-6 gap-4">
        <h1 class="text-2xl font-bold">My Splits</h1>
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

      <!-- An archive that failed shouldn't wipe out the list we already have. -->
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

      <!-- 3. Empty -->
      <div v-else-if="splits.length === 0" class="card text-center py-12">
        <h2 class="text-lg font-medium text-gray-900">No splits yet</h2>
        <p class="mt-2 text-sm text-gray-500 max-w-sm mx-auto">
          A split is a shared pot — a trip, a house, a dinner. Create one, add the
          people in it, and start tracking who owes what.
        </p>
        <router-link to="/splits/new" class="btn-primary inline-block mt-6">
          Create your first split
        </router-link>
        <p v-if="!showArchived" class="mt-4 text-xs text-gray-400">
          Archived one already? Tick “Show archived” above.
        </p>
      </div>

      <!-- 4. Loaded -->
      <ul v-else class="space-y-3">
        <li
          v-for="split in splits"
          :key="split.id"
          class="card relative flex items-start gap-4 transition-colors"
          :class="isArchived(split)
            ? 'bg-gray-50 border-dashed'
            : 'hover:border-primary-500'"
        >
          <div class="min-w-0 flex-1" :class="isArchived(split) && 'opacity-60'">
            <div class="flex flex-wrap items-center gap-2">
              <h2 class="text-base font-semibold text-gray-900 truncate">
                <!-- Stretched link: the whole card is clickable, but the Archive
                     button below sits above it and swallows its own click. -->
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

          <button
            v-if="!isArchived(split)"
            type="button"
            class="btn-secondary relative shrink-0 text-xs px-3 py-1.5"
            :disabled="archivingId === split.id"
            @click.stop.prevent="archive(split)"
          >
            {{ archivingId === split.id ? 'Archiving…' : 'Archive' }}
          </button>
        </li>
      </ul>
    </div>
  </AppLayout>
</template>

<script setup>
import { ref, watch, onMounted } from 'vue'
import { storeToRefs } from 'pinia'
import AppLayout from '@/components/AppLayout.vue'
import { useSplitsStore } from '@/stores/splits'
import { typeLabel, memberLabel, formatDate } from '@/lib/format'

const store = useSplitsStore()
const { splits } = storeToRefs(store)

const showArchived = ref(false)
const loading      = ref(true)
const error        = ref('')
const actionError  = ref('')
const archivingId  = ref(null)

onMounted(load)
watch(showArchived, load)

async function load() {
  loading.value = true
  error.value = ''
  actionError.value = ''
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

async function archive(split) {
  if (archivingId.value) return
  if (!window.confirm(`Archive “${split.name}”? You can still find it under “Show archived”.`)) return

  archivingId.value = split.id
  actionError.value = ''
  try {
    await store.archiveSplit(split.id)
    // archiveSplit drops the row from the list. That is right for the active
    // view, but when archived rows are being shown it should stay put with an
    // Archived badge — so refetch to get its archived_at.
    if (showArchived.value) await store.fetchSplits({ archived: true })
  } catch (e) {
    actionError.value = e.message
  } finally {
    archivingId.value = null
  }
}

const TYPE_LABELS = { one_time: 'One-time', ongoing: 'Ongoing' }

</script>
