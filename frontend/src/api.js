import axios from 'axios'

const api = axios.create({
  baseURL: '/api',
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
