<template>
  <div class="rounded-lg border border-gray-200 bg-gray-50 p-4 text-sm">
    <!-- 1. Uploading. The only thing offered here is Cancel: the request can
            take several seconds (OCR is real work) and a user who picked the
            wrong file should not have to wait it out or reload. -->
    <div v-if="uploading" aria-busy="true">
      <div class="flex items-center justify-between gap-4">
        <div class="min-w-0">
          <p class="font-medium text-gray-900">Reading {{ fileLabel }}…</p>
          <p class="mt-1 text-xs text-gray-500">
            Tesseract is looking for line items. This can take a few seconds for a photo.
          </p>
        </div>
        <button type="button" class="btn-secondary shrink-0 px-3 py-1.5 text-xs" @click="cancel">
          Cancel
        </button>
      </div>
      <div class="mt-3 h-1 overflow-hidden rounded bg-gray-200">
        <div class="h-full w-1/3 animate-pulse rounded bg-primary-500"></div>
      </div>
    </div>

    <!-- 2. Parsed. The draft itself is rendered by the parent — this strip only
            says which file it came from and offers a replacement. -->
    <div v-else-if="done" class="flex flex-wrap items-center justify-between gap-3">
      <div class="min-w-0">
        <p class="font-medium text-gray-900">Read {{ fileLabel }}</p>
        <p class="mt-1 text-xs text-gray-500">
          Nothing has been saved yet. Check every line below, then create the bill.
        </p>
      </div>
      <div class="flex shrink-0 items-center gap-2">
        <button
          type="button"
          class="btn-secondary px-3 py-1.5 text-xs"
          :disabled="disabled"
          @click="replace"
        >
          Choose a different receipt
        </button>
        <button
          type="button"
          class="px-2 py-1.5 text-xs font-medium text-gray-500 hover:text-gray-700 disabled:opacity-50"
          :disabled="disabled"
          @click="clear"
        >
          Clear
        </button>
      </div>
    </div>

    <!-- 3. Idle: the drop zone, plus whatever went wrong last time. -->
    <template v-else>
      <div
        class="rounded-lg border-2 border-dashed p-6 text-center transition-colors"
        :class="dragging
          ? 'border-primary-500 bg-primary-50'
          : 'border-gray-300 bg-white hover:border-gray-400'"
        @dragenter.prevent="onDragEnter"
        @dragover.prevent="onDragOver"
        @dragleave.prevent="onDragLeave"
        @drop.prevent="onDrop"
      >
        <p class="font-medium text-gray-900">Drag a receipt here</p>
        <p class="mt-1 text-xs text-gray-500">or</p>
        <button
          type="button"
          class="btn-secondary mt-2"
          :disabled="disabled"
          @click="openPicker"
        >
          Choose a file
        </button>
        <p class="mt-3 text-xs text-gray-500">
          PNG, JPEG, WebP or TIFF photos, or an HTML receipt. Up to 10&nbsp;MB.
          PDFs are not supported yet.
        </p>

        <!-- The file input is the real control; the zone above just drives it.
             It is kept in the DOM (not v-if'd) so openPicker() can always
             reach it, and reset after every pick so choosing the same file
             twice still fires a change event. -->
        <input
          ref="fileInput"
          type="file"
          class="sr-only"
          :accept="ACCEPT_ATTR"
          :disabled="disabled"
          @change="onPick"
        />
      </div>

      <!-- Whatever the server said, verbatim, plus a line of advice that the
           API cannot know to give. -->
      <div
        v-if="error"
        class="mt-3 rounded-lg border border-red-200 bg-red-50 px-3 py-2"
        role="alert"
      >
        <p class="font-medium text-red-800">{{ error }}</p>
        <p v-if="hint" class="mt-1 text-xs text-red-700">{{ hint }}</p>
      </div>

      <p v-else-if="notice" class="mt-3 text-xs text-gray-500">{{ notice }}</p>
    </template>

    <!-- Always visible, in every state. People are right to be wary of handing
         a receipt to a website, and the honest answer is a selling point. -->
    <p class="mt-4 border-t border-gray-200 pt-3 text-xs text-gray-500">
      <span class="font-medium text-gray-700">Nothing is uploaded to anyone else.</span>
      The file goes to your own BillSplitter server, is read there with Tesseract,
      and is never stored or sent to a third-party OCR service. No API key, no
      copy of your receipt anywhere.
    </p>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import api from '@/api'

// What the server accepts (docs/api.md → Phase 5 → Validation and limits).
// Both the MIME type and the extension are listed in `accept` because a
// browser hands back an empty type for .html often enough to matter.
const ACCEPT_ATTR =
  'image/png,image/jpeg,image/webp,image/tiff,text/html,' +
  '.png,.jpg,.jpeg,.webp,.tif,.tiff,.htm,.html'

const ACCEPTED_MIME = new Set([
  'image/png', 'image/jpeg', 'image/webp', 'image/tiff', 'image/tif', 'text/html',
])
const ACCEPTED_EXT = new Set(['png', 'jpg', 'jpeg', 'webp', 'tif', 'tiff', 'htm', 'html'])

const MAX_BYTES = 10 * 1024 * 1024   // 10 MB, same number the server enforces

// A second line of advice per failure. The first line is always the server's
// own message — these only add what it has no way to know.
const HINTS = {
  400: 'Try a sharper, straighter photo in good light, or the store’s HTML receipt export.',
  404: 'Open the split again from your splits list, then start the bill from there.',
  413: 'A phone photo re-saved as JPEG is usually well under the limit.',
  415: 'Accepted: PNG, JPEG, WebP or TIFF images, or an HTML receipt.',
  500: 'That is a fault on the server, not with your file. Try again in a moment.',
}

const props = defineProps({
  splitId:  { type: String,  required: true },
  // Set while the parent is saving, so a receipt cannot be swapped out from
  // under a bill that is halfway through being created.
  disabled: { type: Boolean, default: false },
})

// `parsed` carries the ReceiptDraft exactly as the server returned it. The
// parent decides what to do with it; this component never interprets it.
const emit = defineEmits(['parsed', 'cleared'])

const fileInput = ref(null)
const uploading = ref(false)
const done      = ref(false)
const dragging  = ref(false)
const error     = ref('')
const hint      = ref('')
const notice    = ref('')
const fileLabel = ref('')

// Kept outside reactive state: it is plumbing, and nothing renders it.
let controller = null
// dragleave fires when the pointer crosses into a *child* element, so a bare
// boolean flickers. Counting enters and leaves does not.
let dragDepth = 0

function openPicker() {
  if (props.disabled) return
  fileInput.value?.click()
}

function onPick(e) {
  const file = e.target.files?.[0]
  // Reset immediately so re-picking the same file still fires `change`.
  e.target.value = ''
  if (file) start(file)
}

function onDragEnter() {
  if (props.disabled || uploading.value) return
  dragDepth += 1
  dragging.value = true
}

function onDragOver(e) {
  if (props.disabled || uploading.value) return
  if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy'
  dragging.value = true
}

function onDragLeave() {
  dragDepth = Math.max(0, dragDepth - 1)
  if (dragDepth === 0) dragging.value = false
}

function onDrop(e) {
  dragDepth = 0
  dragging.value = false
  if (props.disabled || uploading.value) return
  const file = e.dataTransfer?.files?.[0]
  if (file) start(file)
}

// Human-readable size for the too-large message. Not money, so ordinary
// arithmetic is fine here.
function formatSize(bytes) {
  const mb = bytes / (1024 * 1024)
  return mb >= 1 ? `${mb.toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`
}

function extensionOf(name) {
  const i = String(name).lastIndexOf('.')
  return i > -1 ? name.slice(i + 1).toLowerCase() : ''
}

// A courtesy check only. The server judges a file on its magic bytes, not on
// what the browser or the filename claims, so this can never be the real
// gate — it just saves a pointless 10 MB round trip and a wait.
function precheck(file) {
  if (file.size === 0) {
    return { message: 'That file is empty.', hint: 'Pick the photo or HTML file of the receipt.' }
  }
  if (file.size > MAX_BYTES) {
    return {
      message: `That file is ${formatSize(file.size)}. The limit is 10 MB.`,
      hint: HINTS[413],
    }
  }
  const ext  = extensionOf(file.name)
  const mime = (file.type || '').toLowerCase()

  if (mime === 'application/pdf' || ext === 'pdf') {
    return {
      message: 'PDF receipts are not supported yet.',
      hint: 'Tesseract cannot read a PDF directly. Upload a photo or screenshot ' +
            '(PNG, JPEG, WebP or TIFF), or the receipt’s HTML export.',
    }
  }
  if (ACCEPTED_MIME.has(mime) || ACCEPTED_EXT.has(ext)) return null

  return {
    message: `${ext ? `.${ext}` : 'That'} files are not supported.`,
    hint: HINTS[415],
  }
}

async function start(file) {
  if (props.disabled) return

  error.value  = ''
  hint.value   = ''
  notice.value = ''
  fileLabel.value = file.name || 'the receipt'

  const problem = precheck(file)
  if (problem) {
    error.value = problem.message
    hint.value  = problem.hint
    return
  }

  // Replacing a file drops the previous draft before the new request starts,
  // so a failed second attempt cannot leave last receipt's items on screen
  // looking like they belong to this one.
  if (done.value) {
    done.value = false
    emit('cleared')
  }

  const form = new FormData()
  form.append('file', file, file.name)

  controller = new AbortController()
  uploading.value = true
  try {
    // The one call in the app that does not go through a store: no store
    // covers file upload. It still uses the shared axios instance, so the
    // session cookie travels and a 401 redirects like everywhere else.
    //
    // The explicit Content-Type is load-bearing. The shared instance defaults
    // to application/json, and axios turns FormData into a JSON body when the
    // content type says JSON — the file would arrive as "{}". Naming
    // multipart here stops that; the browser then replaces this header with
    // one carrying the real boundary.
    const { data } = await api.post(`/splits/${props.splitId}/bills/parse`, form, {
      headers: { 'Content-Type': 'multipart/form-data' },
      signal: controller.signal,
    })
    done.value = true
    emit('parsed', data, { name: file.name, size: file.size })
  } catch (e) {
    if (e?.code === 'ERR_CANCELED' || e?.name === 'CanceledError') {
      notice.value = 'Upload canceled. Nothing was sent.'
    } else {
      const status = e?.response?.status
      error.value =
        e?.response?.data?.error ??
        (e?.response
          ? `The receipt could not be read (${status}).`
          : 'Network error — is the backend running?')
      hint.value = HINTS[status] ?? ''
    }
  } finally {
    uploading.value = false
    controller = null
  }
}

function cancel() {
  controller?.abort()
}

function clear() {
  cancel()
  done.value      = false
  error.value     = ''
  hint.value      = ''
  notice.value    = ''
  fileLabel.value = ''
  emit('cleared')
}

function replace() {
  clear()
  openPicker()
}
</script>
