// LuckPool VRSC (Verus) JSON API client.
//
// All endpoints discovered by reverse-engineering luckpool.net/verus/miner.html
// + assets/js/minerstats.js (jQuery $.getJSON calls). The base path is
// https://luckpool.net/verus/ — each per-miner endpoint takes the wallet
// address as a path segment.
//
// Endpoints used here:
//   GET /verus/earningstats/<addr>  →  { lastDay, lastTwo, lastSeven, lastTen, lastFifteen }
//                                     (VRSC mined in that many days)
//   GET /verus/miner/<addr>         →  live miner stats (hashrate, immature,
//                                     balance, paid, workers, etc.) — returns
//                                     {"error":"not found"} until first share
//   GET /verus/settings/<addr>      →  { minPayment, minerIP, stake }
//   GET /verus/earnings/<addr>      →  array of recent payments
//
// Everything is open / unauthenticated. Caller should debounce.

const POOL_BASE = 'https://luckpool.net/verus'

async function fetchJsonSafe(url) {
  try {
    const res = await fetch(url, { headers: { Accept: 'application/json' } })
    if (!res.ok) return null
    const text = await res.text()
    if (!text || !text.trim()) return null
    try {
      return JSON.parse(text)
    } catch {
      return null
    }
  } catch {
    return null
  }
}

// Returns null until the wallet has shares on the pool.
// Shape on success (from live observation):
//   {
//     lastDay: 0.0001234,   // VRSC in last 24h
//     lastTwo: 0.000252,
//     lastSeven: 0.000900,
//     lastTen: ...,
//     lastFifteen: ...
//   }
export async function fetchEarningStats(address) {
  if (!address) return null
  return fetchJsonSafe(`${POOL_BASE}/earningstats/${address}`)
}

// Returns the rich miner object once the wallet is recognised by the pool
// (after the first accepted share). Until then returns `{ error: 'not found' }`.
// Real shape includes:
//   { hashrate, immature, balance, paid, workers: [...], ... }
export async function fetchMiner(address) {
  if (!address) return null
  return fetchJsonSafe(`${POOL_BASE}/miner/${address}`)
}

// Recent payment history (array). Empty until first payout.
export async function fetchEarnings(address) {
  if (!address) return null
  return fetchJsonSafe(`${POOL_BASE}/earnings/${address}`)
}

// Account settings (min payout, IP, stake). Always available.
export async function fetchSettings(address) {
  if (!address) return null
  return fetchJsonSafe(`${POOL_BASE}/settings/${address}`)
}

// Pool-wide stats — includes `marketStats.price_usd` and network info.
// Shape (from live observation 2026-05):
//   { poolStats: { hashrate, hashrateSols, minerCount, blocksLast24,
//                  lastBlockReward, ... },
//     networkStats: { height, sols, hashrateString, diff, ... },
//     marketStats: { price_usd, price_btc, percent_change_24h, ... } }
let _poolStatsCache = { at: 0, data: null }
export async function fetchPoolStats({ maxAgeMs = 60_000 } = {}) {
  const now = Date.now()
  if (_poolStatsCache.data && now - _poolStatsCache.at < maxAgeMs) {
    return _poolStatsCache.data
  }
  const data = await fetchJsonSafe(`${POOL_BASE}/stats`)
  if (data) _poolStatsCache = { at: now, data }
  return data
}

// Just the USD price (cached separately for hot-paths). Falls back to a
// recent observation if the pool stats endpoint is unreachable.
const VRSC_USD_FALLBACK = 0.93
export async function fetchVrscPriceUSD() {
  const stats = await fetchPoolStats()
  const p = stats?.marketStats?.price_usd
  return (typeof p === 'number' && p > 0) ? p : VRSC_USD_FALLBACK
}

// Live network parameters used to compute realistic VRSC/day estimates.
// Returns:
//   { networkSols, blockReward, blockTimeSec, priceUSD }
// `networkSols` is the network hashrate in sols/sec (= ~ MH/s × 1e6 for VerusHash).
// `blockReward` is the LAST block reward observed (VRSC). For VerusHash 2.2
// this is ~3 VRSC right now (post-halvings).
// `blockTimeSec` is the avg minutes-to-block × 60. Falls back to 60 (the
// Verus target). For the realistic estimate we use the AVG, not the target.
const NETWORK_FALLBACKS = {
  networkSols: 1.34e12,   // ~1.34 TS/s, current observation
  blockReward: 3.0,        // VRSC per block
  blockTimeSec: 60,        // target
  priceUSD: VRSC_USD_FALLBACK,
}
export async function fetchNetworkParams() {
  const stats = await fetchPoolStats()
  const network = stats?.networkStats
  const pool = stats?.poolStats
  return {
    networkSols: (typeof network?.sols === 'number' && network.sols > 0)
      ? network.sols : NETWORK_FALLBACKS.networkSols,
    blockReward: (typeof pool?.lastBlockReward === 'number' && pool.lastBlockReward > 0)
      ? pool.lastBlockReward : NETWORK_FALLBACKS.blockReward,
    // avgBlockTimeMin is the pool's OBSERVED block time, but for the network-
    // wide estimate the target (60s) is more representative. We use 60s.
    blockTimeSec: NETWORK_FALLBACKS.blockTimeSec,
    priceUSD: (typeof stats?.marketStats?.price_usd === 'number' && stats.marketStats.price_usd > 0)
      ? stats.marketStats.price_usd : NETWORK_FALLBACKS.priceUSD,
  }
}

// Compute realistic VRSC/day given your hashrate in HASHES/SEC (NOT MH/s).
// Formula: (yourHashes/sec / networkHashes/sec) × (86400 / blockTimeSec) × blockReward
// Returns { vrscPerDay, usdPerDay, networkSols, blockReward, priceUSD }.
export async function estimateVrscPerDay(yourHashesPerSec) {
  const np = await fetchNetworkParams()
  const blocksPerDay = 86400 / np.blockTimeSec
  const yourShare = yourHashesPerSec / np.networkSols
  const vrscPerDay = yourShare * blocksPerDay * np.blockReward
  return {
    vrscPerDay,
    usdPerDay: vrscPerDay * np.priceUSD,
    networkSols: np.networkSols,
    blockReward: np.blockReward,
    priceUSD: np.priceUSD,
  }
}

// Single-shot combined fetch — pulls everything in parallel and returns a
// flat object the UI can spread into state. Missing endpoints are silently
// dropped (set to undefined) so the UI just won't render those fields.
export async function fetchLuckPoolLive(address) {
  if (!address) return null
  const [earningStats, miner, settings, networkParams] = await Promise.all([
    fetchEarningStats(address),
    fetchMiner(address),
    fetchSettings(address),
    fetchNetworkParams(),
  ])
  const minerOk = miner && !miner.error
  return {
    // Earnings (VRSC) — these are TOTAL VRSC mined to the address over the
    // window, not USD. Pool also auto-pays at the min threshold.
    vrscLast24h: earningStats?.lastDay ?? 0,
    vrscLast7d: earningStats?.lastSeven ?? 0,
    vrscLast15d: earningStats?.lastFifteen ?? 0,
    // Live status (after first share)
    miner: minerOk ? miner : null,
    minerKnown: !!minerOk,
    // Account config
    minPayment: settings?.minPayment ?? 0.0001,
    minerIP: settings?.minerIP ?? null,
    // Network params for live VRSC/day estimate. The miner C++ used to compute
    // this with a hardcoded constant (VRSC_PER_MHS_DAY = 0.254) based on a
    // 136 GH/s network — wildly off today (network is ~1.34 TH/s now).
    networkSols:   networkParams.networkSols,
    blockReward:   networkParams.blockReward,
    blockTimeSec:  networkParams.blockTimeSec,
    priceUSD:      networkParams.priceUSD,
    // Timestamp for cache busting in the UI
    fetchedAt: Date.now(),
  }
}
