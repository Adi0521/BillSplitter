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

// Per-item allocations and the per-bill share breakdown.
//
// Every number here — a member's share, the unallocated remainder, each
// proportional tax slice — is computed by the server in NUMERIC and arrives as
// a string. This store does no arithmetic on any of them, and neither should
// its callers: see docs/api.md, "The rounding rule".
export const useAllocationsStore = defineStore('allocations', () => {
  // itemId -> AllocationSet. Keyed by item because a bill's items are edited
  // one at a time and each carries its own mode and remainder.
  const byItem = ref({})
  const shares = ref(null)   // BillShares for the currently viewed bill

  function setSet(set) {
    byItem.value = { ...byItem.value, [set.bill_item_id]: set }
    return set
  }

  async function fetchAllocations(billId, itemId) {
    try {
      const { data } = await api.get(`/bills/${billId}/items/${itemId}/allocations`)
      return setSet(data)
    } catch (e) { throw normalize(e) }
  }

  // Full replace: the server deletes the item's existing rows and inserts this
  // set in one transaction. Pass an empty `allocations` array to clear an item.
  async function saveAllocations(billId, itemId, { mode, allocations }) {
    try {
      const { data } = await api.put(`/bills/${billId}/items/${itemId}/allocations`, { mode, allocations })
      return setSet(data)
    } catch (e) { throw normalize(e) }
  }

  async function evenSplit(billId, itemId, memberIds) {
    try {
      const { data } = await api.post(`/bills/${billId}/items/${itemId}/even-split`, { member_ids: memberIds })
      return setSet(data)
    } catch (e) { throw normalize(e) }
  }

  // Shares are refetched on every allocation change, every item edit and every
  // tax/tip/fees save, so two requests are easily in flight at once. Without a
  // guard a slower earlier response lands last and overwrites newer data —
  // leaving the panel showing what someone owed one edit ago, which is exactly
  // the kind of quietly-wrong number this phase is built to avoid. The counter
  // makes the last request issued the only one allowed to write.
  let sharesSeq = 0

  async function fetchShares(splitId, billId) {
    const seq = ++sharesSeq
    try {
      const { data } = await api.get(`/splits/${splitId}/bills/${billId}/shares`)
      if (seq === sharesSeq) shares.value = data
      return data
    } catch (e) { throw normalize(e) }
  }

  function reset() {
    sharesSeq += 1        // invalidate in-flight requests from a previous bill
    byItem.value = {}
    shares.value = null
  }

  return { byItem, shares, fetchAllocations, saveAllocations, evenSplit, fetchShares, reset }
})
