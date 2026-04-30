<template>
  <AppLayout>
    <div class="p-6">
      <div class="flex items-center justify-between mb-6">
        <h1 class="text-2xl font-bold">My Splits</h1>
        <router-link
          to="/splits/new"
          class="rounded-lg bg-primary-600 hover:bg-primary-700 text-white
                 font-medium px-4 py-2 text-sm transition-colors"
        >
          + New Split
        </router-link>
      </div>
      <!-- Splits list — implemented in Phase 2 -->
      <p class="text-gray-500 text-sm">No splits yet. Create one to get started.</p>
    </div>
  </AppLayout>
</template>

<script setup>
import AppLayout from '@/components/AppLayout.vue'
</script>
