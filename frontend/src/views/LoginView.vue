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
          :disabled="loading"
          class="w-full rounded-lg bg-primary-600 hover:bg-primary-700 text-white
                 font-medium py-2 text-sm transition-colors disabled:opacity-50"
        >
          {{ loading
            ? (mode === 'login' ? 'Logging in…' : 'Creating account…')
            : (mode === 'login' ? 'Log in' : 'Create account') }}
        </button>
      </form>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { useAuthStore } from '@/stores/auth'

const auth    = useAuthStore()
const router  = useRouter()
const route   = useRoute()

const mode        = ref('login')
const email       = ref('')
const password    = ref('')
const displayName = ref('')
const loading     = ref(false)
const error       = ref('')

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
</script>

<style scoped>
.input {
  @apply w-full rounded-lg border border-gray-300 px-3 py-2 text-sm
         focus:outline-none focus:ring-2 focus:ring-primary-500 focus:border-transparent
         disabled:bg-gray-100;
}
</style>
