import axios from 'axios'

// The full base path for the API, including the /api prefix.
//
// Default '/api' is a *relative* path, which is what you want in development
// (Vite proxies it to localhost:8080) and in the recommended deployment, where
// the frontend host rewrites /api/* to the backend. Both are same-origin, so
// the session cookie travels under SameSite=Lax and no CORS is involved.
//
// Set VITE_API_BASE_URL to an absolute URL only when the API really is on
// another domain — e.g. 'https://billsplitter-api.fly.dev/api'. That is
// cross-origin, so the backend must also run with SESSION_COOKIE_SAMESITE=None
// and APP_BASE_URL set to this site's origin, or the browser will refuse to
// send the cookie and every request will look logged out. See DEPLOYMENT.md.
const baseURL = import.meta.env.VITE_API_BASE_URL || '/api'

const api = axios.create({
  baseURL,
  withCredentials: true,   // send session cookie on every request
  headers: { 'Content-Type': 'application/json' }
})

// Redirect to /login on 401 responses (except for the /auth/* endpoints themselves)
api.interceptors.response.use(
  res => res,
  err => {
    if (
      err.response?.status === 401 &&
      !err.config.url.startsWith('/auth/')
    ) {
      window.location.href = '/login'
    }
    return Promise.reject(err)
  }
)

export default api
