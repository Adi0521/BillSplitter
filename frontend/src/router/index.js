import { createRouter, createWebHistory } from 'vue-router'
import { useAuthStore } from '@/stores/auth'

const routes = [
  {
    path: '/login',
    name: 'Login',
    component: () => import('@/views/LoginView.vue'),
    meta: { requiresAuth: false }
  },
  {
    path: '/',
    name: 'Splits',
    component: () => import('@/views/SplitsView.vue'),
    meta: { requiresAuth: true }
  },
  {
    path: '/splits/new',
    name: 'NewSplit',
    component: () => import('@/views/NewSplitView.vue'),
    meta: { requiresAuth: true }
  },
  {
    path: '/splits/:id',
    name: 'SplitDetail',
    component: () => import('@/views/SplitDetailView.vue'),
    meta: { requiresAuth: true }
  },
  {
    path: '/splits/:id/bills/new',
    name: 'NewBill',
    component: () => import('@/views/NewBillView.vue'),
    meta: { requiresAuth: true }
  },
  {
    path: '/splits/:splitId/bills/:billId',
    name: 'BillDetail',
    component: () => import('@/views/BillView.vue'),
    meta: { requiresAuth: true }
  },
  {
    path: '/splits/:id/summary',
    name: 'SplitSummary',
    component: () => import('@/views/SplitSummaryView.vue'),
    meta: { requiresAuth: true }
  },
  {
    path: '/share/:token',
    name: 'ShareView',
    component: () => import('@/views/ShareView.vue'),
    meta: { requiresAuth: false }
  },
  {
    // Catch-all → redirect home
    path: '/:pathMatch(.*)*',
    redirect: '/'
  }
]

const router = createRouter({
  history: createWebHistory(),
  routes
})

// Navigation guard: redirect unauthenticated users to /login
router.beforeEach(async (to) => {
  if (to.meta.requiresAuth === false) return true

  const authStore = useAuthStore()
  if (!authStore.user) {
    await authStore.fetchMe()
  }
  if (!authStore.user) {
    return { name: 'Login', query: { redirect: to.fullPath } }
  }
  return true
})

export default router
