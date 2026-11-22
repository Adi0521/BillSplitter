<template>
  <!-- Renders nothing unless the current URL carries post-sign-in flags, so it
       is safe to mount on every page. It lives in AppLayout rather than in one
       view because the Google callback can land anywhere: a user who opened an
       invite link and chose Google is redirected to /invite/<token>, and the
       notice that their password was cleared must not be lost because they
       happened not to pass through the splits list. -->
  <div v-if="passwordCleared || showWelcome">
  <!-- Post-sign-in notices. Google sign-in is a full-page redirect that
       lands here carrying what it did to this account in the query string;
       the flags are read once on mount and then stripped from the URL. -->

  <!-- The destructive one. It reports that a credential was deleted, so it
       gets a heading, an explanation and a deliberate acknowledgement —
       not an X in a corner that a stray click dismisses forever. -->
  <section
    v-if="passwordCleared && !passwordNoticeAck"
    role="alert"
    class="mb-6 rounded-xl border-2 border-amber-300 bg-amber-50 p-5"
  >
    <h2 class="text-base font-semibold text-amber-900">
      The password on this account was removed
    </h2>
    <div class="mt-2 space-y-2 text-sm text-amber-900">
      <p>
        This account already existed with a password — but it was set by
        someone who had never proved they own
        <span class="font-medium">{{ auth.user?.email }}</span>. Google has
        now confirmed that you do, so that password was cleared.
      </p>
      <p>
        Nothing else changed: your splits, bills and payments are all still
        here. <span class="font-medium">Sign in with Google from now on</span> —
        this account has no password any more, and there is no password
        reset. If you never set a password here, someone else had claimed
        your address and no longer can.
      </p>
    </div>
    <button
      type="button"
      class="btn-secondary mt-4 border-amber-300 text-amber-900 hover:bg-amber-100"
      @click="passwordNoticeAck = true"
    >
      I understand
    </button>
  </section>

  <!-- The friendly one. -->
  <div
    v-if="showWelcome"
    class="mb-6 flex items-start gap-3 rounded-lg border border-primary-100 bg-primary-50 px-4 py-3"
  >
    <p class="min-w-0 flex-1 text-sm text-primary-700">
      <template v-if="seatsLinked > 0">
        Signed in with Google. You were added to
        {{ pluralize(seatsLinked, 'split') }} that already had your email
        address — {{ seatsLinked === 1 ? "it's" : "they're" }} in the list below.
      </template>
      <template v-else>
        Signed in with Google.
      </template>
    </p>
    <button
      type="button"
      class="shrink-0 text-xs font-medium text-primary-700 underline hover:text-primary-600"
      @click="welcomeDismissed = true"
    >
      Dismiss
    </button>
  </div>  </div>
</template>

<script setup>
import { ref, computed, onMounted, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import { pluralize } from '@/lib/format'

const route  = useRoute()
const router = useRouter()
const auth   = useAuthStore()

const googleSignIn     = ref(false)
const seatsLinked      = ref(0)
const passwordCleared  = ref(false)
const passwordNoticeAck = ref(false)
const welcomeDismissed = ref(false)

const showWelcome = computed(
  () => (googleSignIn.value || seatsLinked.value > 0) && !welcomeDismissed.value
)

onMounted(read)
// The callback may land on any route, so re-read when one appears mid-session.
watch(() => route.query.signed_in, (v) => { if (v !== undefined) read() })

// Read the flags once, then strip them from the URL with replace (not push) so
// Back leaves the app instead of re-showing the notice, and a refresh does not
// repeat it. Only these three keys are removed; anything else survives.
function read() {
  const { signed_in, seats, password_cleared, ...rest } = route.query
  if (signed_in === undefined && seats === undefined && password_cleared === undefined) return

  googleSignIn.value    = signed_in === 'google'
  seatsLinked.value     = Math.max(0, Number.parseInt(seats, 10) || 0)
  passwordCleared.value = password_cleared === '1' || password_cleared === 'true'
  passwordNoticeAck.value = false
  welcomeDismissed.value  = false

  router.replace({ path: route.path, query: rest })
}
</script>
