import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { fileURLToPath, URL } from 'node:url'

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url))
    }
  },
  server: {
    port: 5173,
    // Bind to all interfaces so a phone on the same Wi-Fi can reach the dev
    // server at http://<your-mac-lan-ip>:5173. Testing mobile layout on a real
    // device catches what a narrow browser window does not: touch target size,
    // the virtual keyboard covering an input, iOS Safari's viewport behaviour,
    // and safe-area insets. The /api proxy below works the same way from the
    // phone, so no backend change is needed.
    host: true,
    proxy: {
      '/api': {
        target: 'http://localhost:8080',
        changeOrigin: true
      }
    }
  }
})
