import { defineStore } from 'pinia'
import { ref } from 'vue'
import api from '@/api'

export const useAuthStore = defineStore('auth', () => {
  const user = ref(null)

  async function fetchMe() {
    try {
      const { data } = await api.get('/auth/me')
      user.value = data
    } catch {
      user.value = null
    }
  }

  async function register(email, password, displayName = '') {
    const { data } = await api.post('/auth/register', {
      email,
      password,
      display_name: displayName
    })
    user.value = data
  }

  async function login(email, password) {
    const { data } = await api.post('/auth/login', { email, password })
    user.value = data
  }

  async function logout() {
    await api.post('/auth/logout')
    user.value = null
  }

  return { user, fetchMe, register, login, logout }
})
