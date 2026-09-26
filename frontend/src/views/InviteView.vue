<template>
  <!-- The router guard guarantees a session before this renders: a signed-out
       visitor is sent to /login?redirect=/invite/<token> and comes back here
       once they have signed up or logged in. So, unlike ShareView, the normal
       logged-in chrome is correct — the visitor is about to become a member. -->
  <AppLayout>
    <div class="p-6 max-w-lg mx-auto">
      <!-- 1. Loading -->
      <div v-if="loading" class="card animate-pulse" aria-busy="true">
        <p class="sr-only">Checking this invite…</p>
        <div class="h-6 w-2/3 rounded bg-gray-200"></div>
        <div class="mt-3 h-4 w-3/4 rounded bg-gray-100"></div>
        <div class="mt-6 h-9 w-32 rounded-lg bg-gray-200"></div>
      </div>

      <!-- 2a. Dead link. The API answers 404 identically for a token that was
              never issued, has been claimed already, was revoked, or was
              replaced by a newer one for the same seat — so this copy must not
              claim to know which. It is also where a claim lands if the link
              was consumed between the preview and the click. -->
      <div v-else-if="notFound" class="card py-12 text-center">
        <h1 class="text-lg font-medium text-gray-900">This invite link isn’t valid.</h1>
        <p class="mx-auto mt-3 max-w-sm text-sm text-gray-600">
          It may have been used already, revoked, or replaced with a newer link
          for the same seat — each invite link works exactly once.
        </p>
        <p class="mx-auto mt-3 max-w-sm text-sm text-gray-500">
          The person who sent it can send another one at any time. Nothing has
          been lost; only this address stopped working.
        </p>
        <router-link to="/" class="btn-secondary mt-6 inline-block">
          Go to your splits
        </router-link>
      </div>

      <!-- 2b. Anything else: network down, server error. -->
      <div v-else-if="error" class="card border-red-200 bg-red-50">
        <h1 class="text-sm font-medium text-red-800">Couldn’t load this invite</h1>
        <p class="mt-2 text-sm text-red-700">{{ error }}</p>
        <button type="button" class="btn-secondary mt-4" :disabled="loading" @click="load">
          Try again
        </button>
      </div>

      <!-- 3a. Already in. Either the preview said so up front, or the claim
              came back 409 because the caller took a seat here some other way
              in between. Offering "Join" would only fail, so link to the split
              instead. The preview does not carry split_id, so the link is a
              best-effort match by name against the caller's own list. -->
      <div v-else-if="alreadyMember && preview" class="card">
        <h1 class="text-xl font-bold text-gray-900">You’re already in this split</h1>
        <p class="mt-3 text-sm text-gray-700">
          <span class="font-medium text-gray-900">{{ preview.invited_by }}</span>
          sent you a link to join
          <span class="font-medium text-gray-900">{{ preview.split_name }}</span>
          as
          <span class="font-medium text-gray-900">{{ preview.member_name }}</span>,
          but your account already has a seat there. An account can hold only one
          seat per split, so there is nothing to claim.
        </p>
        <p class="mt-2 text-sm text-gray-500">
          If the seat named “{{ preview.member_name }}” was meant for you, ask
          {{ preview.invited_by }} — they can remove the extra seat.
        </p>
        <div class="mt-6 flex flex-wrap gap-3">
          <router-link v-if="knownSplitId" :to="`/splits/${knownSplitId}`" class="btn-primary inline-block">
            Open {{ preview.split_name }}
          </router-link>
          <router-link v-else to="/" class="btn-primary inline-block">
            Find it in your splits
          </router-link>
        </div>
      </div>

      <!-- 3b. Preview loaded: the one thing this page is for. -->
      <div v-else-if="preview" class="card">
        <p class="text-xs font-medium uppercase tracking-wide text-primary-600">Invitation</p>
        <h1 class="mt-1 text-xl font-bold text-gray-900 break-words">
          <span>{{ preview.invited_by }}</span>
          invited you to join
          <span>{{ preview.split_name }}</span>
          as
          <span>{{ preview.member_name }}</span>.
        </h1>

        <p class="mt-4 text-sm text-gray-700">
          Joining links your account to the seat named
          <span class="font-medium text-gray-900">{{ preview.member_name }}</span>
          in this split. You’ll see it in your account and can add bills,
          allocate items and record payments like everyone else in it. Anything
          already recorded against that seat becomes yours.
        </p>

        <p v-if="claimError" class="mt-4 rounded-lg border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700">
          {{ claimError }}
        </p>

        <div class="mt-6 flex flex-wrap items-center gap-3">
          <button type="button" class="btn-primary" :disabled="claiming" @click="join">
            {{ claiming ? 'Joining…' : `Join as ${preview.member_name}` }}
          </button>
          <router-link to="/" class="text-sm text-gray-500 hover:text-gray-700">
            Not now
          </router-link>
        </div>

        <p class="mt-4 border-t border-gray-100 pt-4 text-xs text-gray-500">
          This link works once. If it wasn’t meant for you, just close it —
          nothing happens until you click Join.
        </p>
      </div>
    </div>
  </AppLayout>
</template>

<script setup>
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppLayout from '@/components/AppLayout.vue'
import { useSplitsStore } from '@/stores/splits'

const route  = useRoute()
const router = useRouter()
const splitsStore = useSplitsStore()

const token = computed(() => String(route.params.token ?? ''))

const loading  = ref(true)
const error    = ref('')
const notFound = ref(false)
const preview  = ref(null)   // InvitePreview: { split_name, member_name, invited_by, already_member, split_id }

const claiming   = ref(false)
const claimError = ref('')

// Set from the preview, or flipped on by a 409 from the claim.
const alreadyMember = ref(false)
// Best-effort id for the "already in" link. Neither the preview nor the 409
// body includes split_id, so it is resolved by name from the caller's own
// split list and left null if the name is missing or ambiguous.
const knownSplitId = ref(null)

// Guards against an out-of-order response when the token changes mid-flight.
let seq = 0

onMounted(load)
// The route component is reused when the token changes, so the param is watched
// rather than read once.
watch(token, load)

async function load() {
  const t = token.value
  const mine = ++seq

  loading.value   = true
  error.value     = ''
  notFound.value  = false
  preview.value   = null
  claimError.value = ''
  alreadyMember.value = false
  knownSplitId.value  = null

  if (!t) {
    if (mine === seq) {
      notFound.value = true
      loading.value = false
    }
    return
  }

  try {
    const data = await splitsStore.previewInvite(t)
    if (mine !== seq) return
    preview.value = data
    if (data.already_member) {
      alreadyMember.value = true
      knownSplitId.value  = data.split_id ?? null
    }
  } catch (e) {
    if (mine !== seq) return
    // 404 covers unknown, consumed, revoked and replaced tokens alike — the
    // API refuses to distinguish them, and so does this page.
    if (e.status === 404) notFound.value = true
    else error.value = e.message || 'Something went wrong loading this invite.'
  } finally {
    if (mine === seq) loading.value = false
  }
}

async function join() {
  if (claiming.value || !preview.value) return
  claiming.value   = true
  claimError.value = ''
  try {
    const { split_id } = await splitsStore.claimInvite(token.value)
    // The list is stale now — this account has a new split in it.
    splitsStore.fetchSplits().catch(() => {})
    router.replace(`/splits/${split_id}`)
  } catch (e) {
    if (e.status === 404) {
      // Consumed or revoked between the preview and the click.
      preview.value  = null
      notFound.value = true
    } else if (e.status === 409) {
      // The API names the split the caller already belongs to, so the page
      // can link straight to it rather than guessing by name.
      alreadyMember.value = true
      knownSplitId.value  = e.data?.split_id ?? preview.value?.split_id ?? null
    } else {
      claimError.value = e.message || 'Couldn’t join this split. Try again.'
    }
  } finally {
    claiming.value = false
  }
}

</script>
