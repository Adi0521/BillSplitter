<template>
  <AppLayout>
    <div class="p-6 max-w-lg mx-auto">
      <h1 class="text-2xl font-bold mb-1">New Split</h1>
      <p class="text-sm text-gray-500 mb-6">
        A split is a shared pot of bills — a trip, a household, a dinner. You can
        add members and bills once it exists.
      </p>

      <form class="card space-y-5" novalidate @submit.prevent="submit">
        <!-- Name -->
        <div>
          <label class="label" for="split-name">Name</label>
          <input
            id="split-name"
            v-model="name"
            type="text"
            class="input"
            maxlength="200"
            placeholder="Tahoe trip"
            autocomplete="off"
            :disabled="saving"
          />
          <p v-if="fieldErrors.name" class="mt-1 text-xs text-red-600">
            {{ fieldErrors.name }}
          </p>
        </div>

        <!-- Description -->
        <div>
          <label class="label" for="split-description">
            Description <span class="text-gray-400 font-normal">(optional)</span>
          </label>
          <textarea
            id="split-description"
            v-model="description"
            rows="3"
            class="input resize-y"
            maxlength="2000"
            placeholder="Cabin, lift tickets and groceries for the long weekend."
            :disabled="saving"
          ></textarea>
          <div class="mt-1 flex items-start justify-between gap-4">
            <p v-if="fieldErrors.description" class="text-xs text-red-600">
              {{ fieldErrors.description }}
            </p>
            <p v-else class="text-xs text-gray-400">Up to 2000 characters.</p>
            <p class="text-xs shrink-0" :class="descriptionOver ? 'text-red-600' : 'text-gray-400'">
              {{ description.length }}/2000
            </p>
          </div>
        </div>

        <!-- Type. Preselected to one-time on purpose: it is the common case and
             an empty string is not a valid value to submit. -->
        <fieldset :disabled="saving">
          <legend class="label">Type</legend>
          <div class="space-y-2">
            <label
              v-for="option in TYPE_OPTIONS"
              :key="option.value"
              class="flex items-start gap-3 rounded-lg border p-3 cursor-pointer transition-colors"
              :class="type === option.value
                ? 'border-primary-500 bg-primary-50'
                : 'border-gray-300 hover:bg-gray-50'"
            >
              <input
                v-model="type"
                type="radio"
                name="split-type"
                class="mt-0.5 h-4 w-4 text-primary-600 focus:ring-primary-500"
                :value="option.value"
              />
              <span>
                <span class="block text-sm font-medium text-gray-800">{{ option.label }}</span>
                <span class="block text-xs text-gray-500">{{ option.hint }}</span>
              </span>
            </label>
          </div>
          <p v-if="fieldErrors.type" class="mt-1 text-xs text-red-600">
            {{ fieldErrors.type }}
          </p>
        </fieldset>

        <!-- Currency -->
        <div>
          <label class="label" for="split-currency">
            Currency <span class="text-gray-400 font-normal">(optional)</span>
          </label>
          <input
            id="split-currency"
            v-model="currency"
            type="text"
            class="input w-28 uppercase tracking-wider"
            maxlength="3"
            placeholder="USD"
            autocapitalize="characters"
            autocomplete="off"
            spellcheck="false"
            :disabled="saving"
          />
          <p v-if="fieldErrors.currency" class="mt-1 text-xs text-red-600">
            {{ fieldErrors.currency }}
          </p>
          <p v-else class="mt-1 text-xs text-gray-400">
            Three-letter code. Defaults to USD.
          </p>
        </div>

        <!-- Whatever the server rejected, verbatim. -->
        <p v-if="error" class="text-sm text-red-600">{{ error }}</p>

        <div class="flex items-center gap-3 pt-1">
          <button type="submit" class="btn-primary" :disabled="saving">
            {{ saving ? 'Creating…' : 'Create split' }}
          </button>
          <button type="button" class="btn-secondary" :disabled="saving" @click="cancel">
            Cancel
          </button>
        </div>
      </form>
    </div>
  </AppLayout>
</template>

<script setup>
import { computed, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import AppLayout from '@/components/AppLayout.vue'
import { useSplitsStore } from '@/stores/splits'

const TYPE_OPTIONS = [
  { value: 'one_time', label: 'One-time', hint: 'A trip, a dinner, a one-off event.' },
  { value: 'ongoing',  label: 'Ongoing',  hint: 'A household or anything that keeps accruing bills.' },
]
const TYPE_VALUES = TYPE_OPTIONS.map(o => o.value)

const router = useRouter()
const splits = useSplitsStore()

const name        = ref('')
const description = ref('')
const type        = ref('one_time')
const currency    = ref('USD')

const saving      = ref(false)
const error       = ref('')          // server-side / transport failure
const submitted   = ref(false)       // only nag inline after a first attempt
const fieldErrors = ref({})

const descriptionOver = computed(() => description.value.length > 2000)

// Currency is normalized as the user types, so what they see is what is sent.
watch(currency, v => {
  const upper = v.toUpperCase()
  if (upper !== v) currency.value = upper
})

// Re-validate live once they've tried to submit, so errors clear as they fix
// them rather than lingering until the next submit.
watch([name, description, type, currency], () => {
  error.value = ''
  if (submitted.value) validate()
})

// Mirrors the server's rules in docs/api.md → Phase 2 → Validation.
function validate() {
  const errors = {}

  const trimmedName = name.value.trim()
  if (!trimmedName) {
    errors.name = 'Give the split a name.'
  } else if (trimmedName.length > 200) {
    errors.name = 'Name must be 200 characters or fewer.'
  }

  if (description.value.length > 2000) {
    errors.description = 'Description must be 2000 characters or fewer.'
  }

  if (!TYPE_VALUES.includes(type.value)) {
    errors.type = 'Choose whether this split is one-time or ongoing.'
  }

  const trimmedCurrency = currency.value.trim()
  if (trimmedCurrency && !/^[A-Za-z]{3}$/.test(trimmedCurrency)) {
    errors.currency = 'Currency must be a 3-letter code, like USD.'
  }

  fieldErrors.value = errors
  return Object.keys(errors).length === 0
}

async function submit() {
  submitted.value = true
  error.value = ''
  if (!validate()) return

  const trimmedCurrency = currency.value.trim().toUpperCase()

  saving.value = true
  try {
    const split = await splits.createSplit({
      name: name.value.trim(),
      description: description.value.trim(),
      type: type.value,
      // Omitted rather than blank so the server applies its own USD default.
      currency: trimmedCurrency || undefined,
    })
    if (split?.id) {
      router.push({ name: 'SplitDetail', params: { id: split.id } })
    } else {
      // Created, but nothing to navigate to — fall back to the list.
      router.push('/')
    }
  } catch (e) {
    // createSplit() already normalized this to the backend's {"error": "..."}.
    error.value = e.message || 'Could not create the split. Try again.'
  } finally {
    saving.value = false
  }
}

function cancel() {
  router.push('/')
}
</script>
