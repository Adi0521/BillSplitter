<template>
  <div class="min-h-screen flex items-center justify-center bg-gray-50 px-4">
    <div class="w-full max-w-sm bg-white rounded-2xl shadow-md p-8 space-y-6">
      <!-- Title -->
      <div class="text-center">
        <h1 class="text-3xl font-bold text-primary-600">BillSplitter</h1>
        <p class="mt-1 text-sm text-gray-500">Split bills, not friendships.</p>
      </div>

      <!-- Tab toggle -->
      <div class="flex rounded-lg border border-gray-200 overflow-hidden text-sm font-medium">
        <button
          class="flex-1 py-2 transition-colors"
          :class="mode === 'login'
            ? 'bg-primary-600 text-white'
            : 'bg-white text-gray-600 hover:bg-gray-50'"
          @click="mode = 'login'; error = ''"
        >
          Log in
        </button>
        <button
          class="flex-1 py-2 transition-colors"
          :class="mode === 'register'
            ? 'bg-primary-600 text-white'
            : 'bg-white text-gray-600 hover:bg-gray-50'"
          @click="mode = 'register'; error = ''"
        >
          Sign up
        </button>
      </div>

      <!-- Form -->
      <form @submit.prevent="submit" class="space-y-4">
        <div v-if="mode === 'register'">
          <label class="block text-sm font-medium text-gray-700 mb-1">
            Display name <span class="text-gray-400">(optional)</span>
          </label>
          <input
            v-model="displayName"
            type="text"
            placeholder="Alice"
            autocomplete="name"
            class="input"
            :disabled="loading"
          />
        </div>

        <div>
          <label class="block text-sm font-medium text-gray-700 mb-1">
            Email address
          </label>
          <input
            v-model="email"
            type="email"
            required
            placeholder="you@example.com"
            autocomplete="email"
            class="input"
            :disabled="loading"
          />
        </div>

        <div>
          <label class="block text-sm font-medium text-gray-700 mb-1">
            Password
          </label>
          <input
            v-model="password"
            type="password"
            required
            :minlength="mode === 'register' ? 8 : 1"
            placeholder="••••••••"
            autocomplete="current-password"
            class="input"
            :disabled="loading"
          />
          <p v-if="mode === 'register'" class="mt-1 text-xs text-gray-400">
            At least 8 characters
          </p>
        </div>

        <p v-if="error" class="text-sm text-red-600">{{ error }}</p>

        <button
          type="submit"
          :disabled="loading || googleLoading"
          class="w-full rounded-lg bg-primary-600 hover:bg-primary-700 text-white
                 font-medium py-2 text-sm transition-colors disabled:opacity-50"
        >
          {{ loading
            ? (mode === 'login' ? 'Logging in…' : 'Creating account…')
            : (mode === 'login' ? 'Log in' : 'Create account') }}
        </button>
      </form>

      <!-- Separator: the two halves are different mechanisms, not two fields of
           one form, and the divider is what says so. -->
      <div class="relative">
        <div class="absolute inset-0 flex items-center" aria-hidden="true">
          <div class="w-full border-t border-gray-200"></div>
        </div>
        <div class="relative flex justify-center">
          <span class="bg-white px-3 text-xs font-medium uppercase tracking-wider text-gray-400">
            or
          </span>
        </div>
      </div>

      <!-- Google is a full-page redirect, not an XHR: the browser leaves for
           Google's consent screen and returns to /api/auth/google/callback,
           which sets the session cookie itself. One button for both tabs —
           with Google there is no sign-in / sign-up distinction, the account is
           created on first use. -->
      <div>
        <button
          type="button"
          class="w-full flex items-center justify-center gap-3 rounded-lg border
                 border-gray-300 bg-white py-2 text-sm font-medium text-gray-700
                 shadow-sm hover:bg-gray-50 transition-colors disabled:opacity-50"
          :disabled="loading || googleLoading"
          @click="continueWithGoogle"
        >
          <svg class="h-4 w-4 shrink-0" viewBox="0 0 18 18" aria-hidden="true">
            <path fill="#4285F4" d="M17.64 9.2c0-.64-.06-1.25-.16-1.84H9v3.48h4.84a4.14 4.14 0 0 1-1.8 2.72v2.26h2.91c1.7-1.57 2.69-3.88 2.69-6.62z"/>
            <path fill="#34A853" d="M9 18c2.43 0 4.47-.8 5.96-2.18l-2.91-2.26c-.81.54-1.84.86-3.05.86-2.34 0-4.32-1.58-5.03-3.71H.96v2.33A9 9 0 0 0 9 18z"/>
            <path fill="#FBBC05" d="M3.97 10.71a5.41 5.41 0 0 1 0-3.42V4.96H.96a9 9 0 0 0 0 8.08l3.01-2.33z"/>
            <path fill="#EA4335" d="M9 3.58c1.32 0 2.5.45 3.44 1.35l2.58-2.58C13.46.89 11.43 0 9 0A9 9 0 0 0 .96 4.96l3.01 2.33C4.68 5.16 6.66 3.58 9 3.58z"/>
          </svg>
          {{ googleLoading ? 'Contacting Google…' : 'Continue with Google' }}
        </button>

        <p v-if="googleError" class="mt-2 text-sm text-red-600">{{ googleError }}</p>
        <p v-else class="mt-2 text-center text-xs text-gray-400">
          Google confirms you own the address, so you're added to any split that
          already lists it.
        </p>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import { apiBaseURL } from '@/api'

const auth    = useAuthStore()
const router  = useRouter()
const route   = useRoute()

const mode        = ref('login')
const email       = ref('')
const password    = ref('')
const displayName = ref('')
const loading     = ref(false)
const error       = ref('')

const googleLoading = ref(false)
const googleError   = ref('')

async function submit() {
  error.value   = ''
  loading.value = true
  try {
    if (mode.value === 'login') {
      await auth.login(email.value, password.value)
    } else {
      await auth.register(email.value, password.value, displayName.value)
    }
    const redirect = route.query.redirect || '/'
    router.replace(redirect)
  } catch (e) {
    error.value = e.response?.data?.error ?? 'Something went wrong. Try again.'
  } finally {
    loading.value = false
  }
}

// Only a path on this site is a safe post-sign-in destination. "//evil.com" is
// a protocol-relative URL the browser reads as another origin, so a bare
// startsWith('/') check is not enough. The backend must validate it again —
// this value survives a round trip through Google and comes back as a
// redirect target, which is exactly the shape of an open redirect.
function safeRedirect() {
  const r = route.query.redirect
  return typeof r === 'string' && r.startsWith('/') && !r.startsWith('//') ? r : ''
}

async function continueWithGoogle() {
  if (loading.value || googleLoading.value) return

  googleError.value   = ''
  googleLoading.value = true

  // ?redirect has to survive the Google round trip, or an invited user who
  // picks Google lands on the splits list instead of the invite they clicked.
  // The backend echoes it onto its final redirect back into the app.
  const redirect = safeRedirect()
  const url = apiBaseURL + '/auth/google/start' +
    (redirect ? `?redirect=${encodeURIComponent(redirect)}` : '')

  // Preflight, so a server with no Google credentials configured doesn't hand
  // the user a backend error page. redirect:'manual' stops fetch following the
  // 302 towards Google: the response comes back opaque (type 'opaqueredirect',
  // status 0), which is itself the "configured and working" signal, while a
  // 503 or 404 arrives readable. Nothing is consumed by asking — /start only
  // builds a URL and re-issues its state cookie, and the real navigation below
  // issues the state that the callback actually checks.
  try {
    const res = await fetch(url, { redirect: 'manual', credentials: 'same-origin' })
    const willRedirect =
      res.type === 'opaqueredirect' || res.status === 0 ||
      (res.status >= 300 && res.status < 400)

    if (!willRedirect) {
      googleError.value   = googleUnavailable(res.status)
      googleLoading.value = false
      return
    }
  } catch {
    // The preflight itself couldn't complete — offline, proxy down, or an
    // absolute VITE_API_BASE_URL that answered without CORS headers. Don't
    // block sign-in on a check that failed for its own reasons; navigate and
    // let the real attempt surface any problem.
  }

  window.location.href = url
}

function googleUnavailable(status) {
  if (status === 503) {
    return 'Google sign-in isn’t set up on this server. Use your email and password instead.'
  }
  if (status === 404) {
    return 'Google sign-in isn’t available on this server. Use your email and password instead.'
  }
  return `Couldn’t start Google sign-in (the server returned ${status}). ` +
         'Use your email and password instead.'
}
</script>

