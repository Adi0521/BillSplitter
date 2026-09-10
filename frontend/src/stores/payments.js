import { defineStore } from 'pinia'
import { ref } from 'vue'
import api from '@/api'

function normalize(e) {
  const msg =
    e?.response?.data?.error ??
    (e?.response ? `Request failed (${e.response.status}).` : 'Network error — is the backend running?')
  const err = new Error(msg)
  err.status = e?.response?.status
  err.cause = e
  return err
}

// Payments, the split-wide summary, and the public share view.
//
// Every amount here is a server-computed string. Nothing in this store adds,
// subtracts or rounds money: balances, settlements and per-currency totals all
// arrive finished. See docs/api.md, "Balances, not 'who owes the payer'".
export const usePaymentsStore = defineStore('payments', () => {
  const payments = ref([])
  const summary  = ref(null)
  const publicSplit = ref(null)

  // Recording or deleting a payment changes every balance, so the summary is
  // refetched rather than adjusted locally.
  let summarySeq = 0

  async function fetchPayments(splitId) {
    try {
      const { data } = await api.get(`/splits/${splitId}/payments`)
      payments.value = data
      return data
    } catch (e) { throw normalize(e) }
  }

  async function recordPayment(splitId, payload) {
    try {
      const { data } = await api.post(`/splits/${splitId}/payments`, payload)
      payments.value = [data, ...payments.value]
      await fetchSummary(splitId).catch(() => {})
      return data
    } catch (e) { throw normalize(e) }
  }

  async function deletePayment(splitId, paymentId) {
    try {
      await api.delete(`/splits/${splitId}/payments/${paymentId}`)
      payments.value = payments.value.filter(p => p.id !== paymentId)
      await fetchSummary(splitId).catch(() => {})
    } catch (e) { throw normalize(e) }
  }

  async function fetchSummary(splitId) {
    const seq = ++summarySeq
    try {
      const { data } = await api.get(`/splits/${splitId}/summary`)
      if (seq === summarySeq) summary.value = data
      return data
    } catch (e) { throw normalize(e) }
  }

  // No session required — this is the unauthenticated share view.
  async function fetchPublic(token) {
    try {
      const { data } = await api.get(`/splits/share/${token}`)
      publicSplit.value = data
      return data
    } catch (e) { throw normalize(e) }
  }

  async function regenerateShareToken(splitId) {
    try {
      const { data } = await api.post(`/splits/${splitId}/share/regenerate`)
      return data.share_token
    } catch (e) { throw normalize(e) }
  }

  function reset() {
    summarySeq += 1
    payments.value = []
    summary.value = null
    publicSplit.value = null
  }

  return {
    payments, summary, publicSplit,
    fetchPayments, recordPayment, deletePayment,
    fetchSummary, fetchPublic, regenerateShareToken, reset,
  }
})
