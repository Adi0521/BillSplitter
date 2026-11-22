<template>
  <div class="min-h-screen flex flex-col">
    <!-- Top nav -->
    <nav class="bg-white border-b border-gray-200 px-6 py-3 flex items-center justify-between">
      <router-link to="/" class="text-xl font-bold text-primary-600">
        BillSplitter
      </router-link>
      <div class="flex items-center gap-4">
        <span class="text-sm text-gray-600">{{ auth.user?.email }}</span>
        <button
          class="text-sm text-gray-500 hover:text-gray-700"
          @click="logout"
        >
          Log out
        </button>
      </div>
    </nav>

    <!-- Page content -->
    <main class="flex-1">
      <!-- Post-sign-in notices render above whatever page the Google callback
           landed on, and nothing at all when there are no flags to report. -->
      <div class="mx-auto max-w-3xl px-6 pt-6 empty:hidden">
        <SignInNotices />
      </div>
      <slot />
    </main>
  </div>
</template>

<script setup>
import SignInNotices from '@/components/SignInNotices.vue'
import { useAuthStore } from '@/stores/auth'
import { useRouter } from 'vue-router'

const auth   = useAuthStore()
const router = useRouter()

async function logout() {
  await auth.logout()
  router.push('/login')
}
</script>
