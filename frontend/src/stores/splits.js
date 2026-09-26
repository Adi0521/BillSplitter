import { defineStore } from 'pinia'
import { ref } from 'vue'
import api from '@/api'

// Normalizes an axios failure into an Error whose message is safe to show a
// user. The backend always answers {"error": "..."} (see docs/api.md), so
// prefer that; fall back only when the request never reached it.
function normalize(e) {
  const msg =
    e?.response?.data?.error ??
    (e?.response ? `Request failed (${e.response.status}).` : 'Network error — is the backend running?')
  const err = new Error(msg)
  err.status = e?.response?.status
  // Some failures carry useful fields beyond `error` — the invite claim's 409
  // names the split the caller is already in. Expose the body so a view can
  // act on it without digging through axios internals.
  err.data = e?.response?.data ?? null
  err.cause = e
  return err
}

// Shared server state for splits and their members.
//
// Actions throw a normalized Error on failure rather than swallowing it, so
// each view stays in control of its own loading/error/empty rendering. The
// store owns the data; the view owns the presentation of the request.
export const useSplitsStore = defineStore('splits', () => {
  const splits  = ref([])    // [Split] — the list, as last fetched
  const current = ref(null)  // SplitDetail — includes a `members` array
  const members = ref([])    // [Member] for `current`

  async function fetchSplits({ archived = false } = {}) {
    try {
      const { data } = await api.get('/splits', { params: archived ? { archived: true } : {} })
      splits.value = data
      return data
    } catch (e) { throw normalize(e) }
  }

  async function createSplit({ name, description, type, currency }) {
    try {
      const { data } = await api.post('/splits', { name, description, type, currency })
      splits.value = [data, ...splits.value]
      return data
    } catch (e) { throw normalize(e) }
  }

  async function fetchSplit(id) {
    try {
      const { data } = await api.get(`/splits/${id}`)
      current.value = data
      members.value = data.members ?? []
      return data
    } catch (e) { throw normalize(e) }
  }

  async function updateSplit(id, patch) {
    try {
      const { data } = await api.put(`/splits/${id}`, patch)
      if (current.value?.id === id) current.value = { ...current.value, ...data }
      splits.value = splits.value.map(s => (s.id === id ? { ...s, ...data } : s))
      return data
    } catch (e) { throw normalize(e) }
  }

  // Archives rather than deletes: the row survives with archived_at set.
  async function archiveSplit(id) {
    try {
      await api.delete(`/splits/${id}`)
      splits.value = splits.value.filter(s => s.id !== id)
    } catch (e) { throw normalize(e) }
  }

  async function fetchMembers(splitId) {
    try {
      const { data } = await api.get(`/splits/${splitId}/members`)
      members.value = data
      return data
    } catch (e) { throw normalize(e) }
  }

  async function addMember(splitId, { name, email }) {
    try {
      const { data } = await api.post(`/splits/${splitId}/members`, { name, email: email || undefined })
      members.value = [...members.value, data]
      if (current.value?.id === splitId) {
        current.value = { ...current.value, member_count: (current.value.member_count ?? 0) + 1 }
      }
      return data
    } catch (e) { throw normalize(e) }
  }

  async function removeMember(splitId, memberId) {
    try {
      await api.delete(`/splits/${splitId}/members/${memberId}`)
      members.value = members.value.filter(m => m.id !== memberId)
      if (current.value?.id === splitId) {
        current.value = { ...current.value, member_count: Math.max(0, (current.value.member_count ?? 1) - 1) }
      }
    } catch (e) { throw normalize(e) }
  }

  // ── Collaboration ──────────────────────────────────────────────────────────
  // Owner-only actions return 403 for a linked member (the only 403s in the
  // API). Callers branch on err.status to explain rather than just fail.

  // Returns { invite_token, invite_path }. The token is shown once; the member
  // list only ever reports invite_pending, never the token itself.
  async function inviteMember(splitId, memberId) {
    try {
      const { data } = await api.post(`/splits/${splitId}/members/${memberId}/invite`)
      members.value = members.value.map(m => (m.id === memberId ? { ...m, invite_pending: true } : m))
      return data
    } catch (e) { throw normalize(e) }
  }

  async function revokeInvite(splitId, memberId) {
    try {
      await api.delete(`/splits/${splitId}/members/${memberId}/invite`)
      members.value = members.value.map(m => (m.id === memberId ? { ...m, invite_pending: false } : m))
    } catch (e) { throw normalize(e) }
  }

  async function previewInvite(token) {
    try {
      const { data } = await api.get(`/invites/${token}`)
      return data
    } catch (e) { throw normalize(e) }
  }

  // Resolves to { split_id }. 404 = unknown/consumed/revoked (one message for
  // all three, deliberately); 409 = the caller already holds a seat here.
  async function claimInvite(token) {
    try {
      const { data } = await api.post(`/invites/${token}/claim`)
      return data
    } catch (e) { throw normalize(e) }
  }

  // Unlinks the caller's account from their seat. The seat and its ledger
  // entries stay; the split simply disappears from this account's list.
  async function leaveSplit(splitId) {
    try {
      await api.post(`/splits/${splitId}/leave`)
      splits.value = splits.value.filter(s => s.id !== splitId)
      if (current.value?.id === splitId) { current.value = null; members.value = [] }
    } catch (e) { throw normalize(e) }
  }

  function reset() {
    splits.value = []
    current.value = null
    members.value = []
  }

  return {
    splits, current, members,
    fetchSplits, createSplit, fetchSplit, updateSplit, archiveSplit,
    fetchMembers, addMember, removeMember,
    inviteMember, revokeInvite, previewInvite, claimInvite, leaveSplit,
    reset,
  }
})
