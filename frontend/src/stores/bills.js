import { defineStore } from 'pinia'
import { ref } from 'vue'
import api from '@/api'

// Same normalization as the splits store: surface the backend's own
// {"error": "..."} message, and keep the status code for callers that branch
// on 404.
function normalize(e) {
  const msg =
    e?.response?.data?.error ??
    (e?.response ? `Request failed (${e.response.status}).` : 'Network error — is the backend running?')
  const err = new Error(msg)
  err.status = e?.response?.status
  err.cause = e
  return err
}

// Shared server state for bills and their line items.
//
// Note the asymmetry in the API, which this store mirrors: bills are addressed
// under /splits/:splitId/bills/:billId, but items are addressed under
// /bills/:billId/items (see docs/api.md). Item calls therefore do not take a
// splitId at all.
//
// Money arrives as strings and is kept as strings. Nothing in this store does
// arithmetic on an amount — every total a view displays comes from the server.
export const useBillsStore = defineStore('bills', () => {
  const bills   = ref([])    // [Bill] for the current split
  const current = ref(null)  // BillDetail — includes an `items` array
  const items   = ref([])    // [BillItem] for `current`

  async function fetchBills(splitId) {
    try {
      const { data } = await api.get(`/splits/${splitId}/bills`)
      bills.value = data
      return data
    } catch (e) { throw normalize(e) }
  }

  async function createBill(splitId, payload) {
    try {
      const { data } = await api.post(`/splits/${splitId}/bills`, payload)
      bills.value = [data, ...bills.value]
      return data
    } catch (e) { throw normalize(e) }
  }

  async function fetchBill(splitId, billId) {
    try {
      const { data } = await api.get(`/splits/${splitId}/bills/${billId}`)
      current.value = data
      items.value = data.items ?? []
      return data
    } catch (e) { throw normalize(e) }
  }

  async function updateBill(splitId, billId, patch) {
    try {
      const { data } = await api.put(`/splits/${splitId}/bills/${billId}`, patch)
      if (current.value?.id === billId) current.value = { ...current.value, ...data }
      bills.value = bills.value.map(b => (b.id === billId ? { ...b, ...data } : b))
      return data
    } catch (e) { throw normalize(e) }
  }

  async function deleteBill(splitId, billId) {
    try {
      await api.delete(`/splits/${splitId}/bills/${billId}`)
      bills.value = bills.value.filter(b => b.id !== billId)
      if (current.value?.id === billId) { current.value = null; items.value = [] }
    } catch (e) { throw normalize(e) }
  }

  // ── Items ──────────────────────────────────────────────────────────────────
  // Mutating an item changes the parent bill's server-derived subtotal and
  // total, so each of these refetches the bill rather than patching the
  // amounts locally. Recomputing them here would mean doing float arithmetic
  // on money, which the contract forbids.

  async function addItem(splitId, billId, payload) {
    try {
      const { data } = await api.post(`/bills/${billId}/items`, payload)
      items.value = [...items.value, data]
      await refreshBillTotals(splitId, billId)
      return data
    } catch (e) { throw normalize(e) }
  }

  async function updateItem(splitId, billId, itemId, patch) {
    try {
      const { data } = await api.put(`/bills/${billId}/items/${itemId}`, patch)
      items.value = items.value.map(i => (i.id === itemId ? data : i))
      await refreshBillTotals(splitId, billId)
      return data
    } catch (e) { throw normalize(e) }
  }

  async function removeItem(splitId, billId, itemId) {
    try {
      await api.delete(`/bills/${billId}/items/${itemId}`)
      items.value = items.value.filter(i => i.id !== itemId)
      await refreshBillTotals(splitId, billId)
    } catch (e) { throw normalize(e) }
  }

  // Pulls the authoritative subtotal/total back after an item mutation. A
  // failure here leaves the item change intact, so it must not surface as if
  // the mutation failed.
  async function refreshBillTotals(splitId, billId) {
    try {
      const { data } = await api.get(`/splits/${splitId}/bills/${billId}`)
      if (current.value?.id === billId) {
        current.value = { ...current.value, subtotal: data.subtotal, total: data.total, item_count: data.item_count }
      }
      bills.value = bills.value.map(b =>
        b.id === billId ? { ...b, subtotal: data.subtotal, total: data.total, item_count: data.item_count } : b)
    } catch {
      // Totals will be correct on the next full load; the mutation succeeded.
    }
  }

  function reset() {
    bills.value = []
    current.value = null
    items.value = []
  }

  return {
    bills, current, items,
    fetchBills, createBill, fetchBill, updateBill, deleteBill,
    addItem, updateItem, removeItem, reset,
  }
})
