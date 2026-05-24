export function getHashrate(log = '', algorithm = 'randomx') {
  log = log.trim()

  // VerusHash: [STATS] 1.76 MH/s | total: 8900000 hashes | uptime: 5s
  if (algorithm === 'verushash') {
    const m = /\[STATS\]\s+([\d.]+)\s+(MH|KH|GH|H)\/s/.exec(log)
    if (m) {
      const value = Number(m[1])
      const unit = m[2]
      // Normalise to H/s for the chart
      if (unit === 'GH') return value * 1e9
      if (unit === 'MH') return value * 1e6
      if (unit === 'KH') return value * 1e3
      return value
    }
    return 0
  }

  // RandomX (xmrig): [timestamp]  miner  speed 10s/60s/15m 353.6 n/a n/a H/s max 359.0 H/s
  if (log && /miner/.test(log)) {
    const [, matched] = /speed(.*)max/.exec(log)
    const [, speedPer10Second] = matched.trim().split(' ')
    return Number(speedPer10Second)
  }

  return 0
}
