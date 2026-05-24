import { writable } from '@svelte-use/shared'

// Default form values used when localStorage has no prior session.
const DEFAULT_FORM = {
  algorithm: 'randomx', // 'randomx' (working) | 'verushash' (in development)
  symbol: '',
  address: '',
  referralCode: '',
  cpuUsage: 25,
}

// Rehydrate the form from localStorage so the user's last-used algorithm,
// coin, wallet, and CPU slider persist across .app restarts. Without this,
// the mining page's `isVerus` reactive defaulted to false on every cold
// launch even if the user had previously picked VerusHash — leaving the
// page rendering an empty / generic state.
function loadInitialForm() {
  try {
    const raw = typeof localStorage !== 'undefined'
      ? localStorage.getItem('form')
      : null
    if (!raw) return { ...DEFAULT_FORM }
    const parsed = JSON.parse(raw)
    return { ...DEFAULT_FORM, ...parsed }
  } catch {
    return { ...DEFAULT_FORM }
  }
}

export const form = writable(loadInitialForm())

// Persist any future writes back to localStorage so select-coin doesn't
// have to remember to call setStorage('form', $form) on every change.
if (typeof localStorage !== 'undefined') {
  form.subscribe((val) => {
    try { localStorage.setItem('form', JSON.stringify(val)) } catch {}
  })
}

export const preparing = writable(false)

export const isMining = writable(false)

export const hashrates = writable([0, 0])

// calculate step on `FormSettings.svelte`
export const cpuCores = writable(100)

// Performance-core count (from Go side, sysctl hw.perflevel0.physicalcpu).
// 0 = unknown (Intel Mac or sysctl unavailable).
export const pCores = writable(0)

export const miningLogs = writable([])

// 'unknown' | 'checking' | 'online' | 'offline'
export const connectionStatus = writable('unknown')

// Coin list: [name, symbol, referralCode, logoUrl][]. Initialized empty;
// populated by helper/coinLoader.js (live API → cache → static fallback).
export const coins = writable([])
