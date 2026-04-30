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
      <slot />
    </main>
  </div>
</template>

<script setup>
import { useAuthStore } from '@/stores/auth'
import { useRouter } from 'vue-router'

const auth   = useAuthStore()
const router = useRouter()

async function logout() {
  await auth.logout()
  router.push('/login')
}
</script>
