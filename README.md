# UnminerMac

**First native ARM64 + Metal VerusHash 2.2 miner for Apple Silicon (M1–M5+).** Also supports RandomX/XMR via unMineable. macOS 11+ including Tahoe 26.x.

Built by **[Verox Studio](https://veroxstudio.com)**.

---

## What's in this app

- **VerusHash 2.2 + PBaaS miner** — native ARM64 NEON + ARMv8 AES + CL hash via `vmull_p64`, blake2b preHeader embedding via libsodium. Connects directly to LuckPool, shares accepted in production. First public arm64-native VerusHash 2.2 miner with full PBaaS preprocessing.
- **RandomX miner** — xmrig 6.26.0 (March 2026) via unMineable pool. Mines XMR + auto-converts to 35+ coins.
- **Metal GPU mining** — Phase 4 in development. Bit-sliced AES at 87.7 G rounds/sec on M5 GPU (33× CPU throughput).

---

## What makes this different

- No hidden referral codes, no phone-home telemetry, no silent extraction
- Every binary updated (xmrig 6.26.0 arm64, verusminer native)
- Auto P-core detection + CPU slider that persists across launches
- Live connection status indicator + DNS-blocking workarounds
- Glass-morphism UI with dark/light mode

---

## Install

Download the latest release from [Releases](https://github.com/helloworldxdwastaken/UnminerMac/releases) → drag to Applications → launch.

macOS Tahoe Gatekeeper: System Settings → Privacy & Security → "Open Anyway".

## Building from source

```bash
npm install --legacy-peer-deps
npm run build              # builds Svelte frontend → dist/
bash build.sh              # builds Go app + packages into out/UnminerMac.app
```

---

## VerusHash 2.2 — our research & first implementation

The full research is documented on the [research page](https://helloworldxdwastaken.github.io/UnminerMac/research.html).

**CPU pipeline (Phase 1–3, shipping):**
- Haraka256 NEON: 68 MH/s (1 P-core)
- Full VerusHash 2.2 mining with CL hash + key caching: 1.82 MH/s (1 P-core) / 7.3 MH/s (4 P-cores)
- ARMv8 `vmull_p64` polynomial multiply for CL hash (2.2× vs portable)
- Stratum v1 client → LuckPool direct mining

**Metal GPU pipeline (Phase 4, in development):**
- Bit-sliced AES kernel: 87.7 G rounds/sec on M5 GPU (33× single CPU core)
- Target: 8–15 MH/s VerusHash on M5 GPU
- Architecture: bit-sliced AES (pure ALU, no memory lookups) via Metal Shading Language

---

## What we discovered while building this

Getting a VerusHash 2.2 + PBaaS share **accepted** by LuckPool turned out to be far harder than implementing the hash algorithm itself. Every byte-level decision had to match what the pool's server-side code expects — and the pool's verushash-node fork is not what most public references describe. Sharing what we learned in case it saves someone else weeks of debugging.

### 1. LuckPool uses VerusCoin's verushash-node, NOT hellcatz's simpler fork

`hellcatz/node-stratum-pool/package.json` line 8: `"verushash": "git+https://github.com/VerusCoin/verushash-node"`. The official fork does **full PBaaS preprocessing** before hashing. The hellcatz fork of verushash-node is a 3-line stub (just `Reset/Write/Finalize2b`). If you wire against hellcatz's version, your hash will diverge from pool's on every single share.

### 2. Pool's PBaaS preprocessing (the part nobody documents)

When `sol_ver > 6` AND `numPBaaSHeaders > 0`, pool's `vh.hash2b2` does this BEFORE Reset/Write/Finalize2b:

```
1. Build a 196-byte preHeader from the buffer:
   preHeader[0..32]    = buf[4..36]    (hashPrevBlock)
   preHeader[32..64]   = buf[36..68]   (hashMerkleRoot)
   preHeader[64..96]   = buf[68..100]  (hashFinalSaplingRoot)
   preHeader[96..128]  = buf[108..140] (nNonce — note: skips nTime!)
   preHeader[128..132] = buf[104..108] (nBits)
   preHeader[132..196] = solution[8..72]  (hashPrevMMRRoot + hashBlockMMRRoot)

2. blake2b-256 the preHeader with personalization "VerusDefaultHash" (libsodium
   crypto_generichash_blake2b_init_salt_personal)

3. Compare the 32-byte blake2b to solution[124..156]:
   - If MATCH: zero out [4..100], [104..108], [108..140], and solution[8..72],
     then hash the cleared buffer
   - If NO MATCH: return 0xff..ff (which guarantees "low difficulty share"
     rejection no matter what)
   - If preHeader is already all-zero (matched_zeros == 196): skip the check,
     hash directly
```

This means: **every time the miner mutates the header nonce, it MUST recompute blake2b(preHeader) and write it into solution[124..156] before submitting.** Otherwise pool's match fails and the share is rejected. The miner-side hash function must also mirror the same clear locally so its hash matches what the pool computes on the cleared buffer.

### 3. extraNonce2 framing (off-by-en1)

LuckPool's `stratum.js` `handleSubmit` builds the full nonce as `extraNonce1 + message.params[3]` (string concat, not your full 32-byte nonce). So your `mining.submit` params[3] should be the **28 bytes AFTER extranonce1**, not the full 32-byte nonce. Pool's `serializeHeader` then writes the concatenated 32 bytes to the header. If you submit 32 bytes, pool ends up with `en1 + your_nonce[0..28]`, your hash buffer has different bytes than pool's, hashes mismatch, share rejected with the catch-all "low difficulty share."

### 4. mining.notify is the 9-field PBaaS form, NOT classic Equihash

`[job_id, version, prevhash_reversed, merkleroot_reversed, hashfinalsaplingroot_reversed, ntime, nbits, clean_jobs, solution]` — no nonce, no coinbase1/2, no merkle branches. The solution field is the **full daemon solution with trailing zeros trimmed** (so you may receive 229 bytes for a job whose final body has to be 1344 bytes; you re-pad with zeros).

### 5. The solution-size validator uses Equihash 200_9 dimensions

`SOLUTION_LENGTH = 2694` hex chars (1347 bytes total), `SOLUTION_SLICE = 6` (3-byte CompactSize varint `fd 40 05`, then 1344-byte body). The body must contain `extranonce1` somewhere in its last 15 bytes (the `solExtraData.indexOf(extraNonce1)` check). The body's first 4 bytes must equal notify_solution[0..3] (`07000000`). Get any of these wrong and you get `[20, "invalid solution size"]` or `[20, "invalid solution, pool nonce missing"]`.

### 6. Hash bytes are LE-stored, target bytes are BE-display, compared as bignums

Pool: `bignum.fromBuffer(headerHash, {endian:'little'})` for the hash, `parseInt(target_hex, 16)` for the target (BE). Compare numerically. In our `hash_below_target_pool` we walk `hash[31] vs target[0]`, then `hash[30] vs target[1]`, etc. — both MSB-first from opposite ends of the byte arrays.

### 7. The ARM port

`verus_clhash.cpp` includes `crypto/SSE2NEON.h` on ARM. Modern SSE2NEON.h already provides the intrinsics that older copies of `verus_clhash.cpp` define inline at the top — those in-file ARM fallbacks now duplicate and break the build. Remove them. Also patch `verus_clhash.h` to skip `<cpuid.h>` / `<x86intrin.h>` on ARM, replace `getauxval(AT_HWCAP)` (Linux-only) with a hard `true` on Apple Silicon, drop the `__tls_init()` call (libcxx-internal), and shim `<endian.h>` to `<libkern/OSByteOrder.h>` in `common.h` for macOS.

### 8. Code in `verusminer/cpu/canonical/`

We ship the unmodified VerusCoin `verus_hash.cpp`, `verus_clhash.cpp`, `verus_clhash_portable.cpp`, plus the patched headers and `sse2neon.h`. Total ~80KB of source. Builds clean on Apple Silicon with `clang++ -march=armv8-a+crypto+sha2+aes -std=c++17` and links against `libsodium`.

If you're building a Verus miner for ARM and shares are coming back as "low difficulty share" no matter what you do — it's almost certainly the blake2b preHeader embed. That was 6 distinct theories and most of a week to isolate.

---

## Attribution & references

- **[2nthony/macmineable](https://github.com/2nthony/macmineable)** — original project that this was built from. Rewritten and extended significantly.
- **xmrig** — [GPL v3](https://github.com/xmrig/xmrig), RandomX miner bundled in `assets/miner/`
- **VerusCoin source** — [MIT](https://github.com/VerusCoin/VerusCoin), algorithm reference for haraka + verus_clhash
- **Haraka v2 paper** (ePrint 2016/098) — algorithm spec and test vectors
- **MacMetal Miner** — [MIT](https://github.com/MacMetalMiner/MacMetal-Miner), Metal kernel architecture reference (SHA-256d, not VerusHash)
- **Käsper-Schwabe bit-sliced AES** (ePrint 2009/129) — algorithm reference for GPU AES

---

## License

**Elastic License 2.0 (ELv2)** — see [LICENSE](./LICENSE).

Non-commercial use, research, education, and personal mining are free. Commercial or hosted use requires a paid license. Contact via [GitHub](https://github.com/helloworldxdwastaken/UnminerMac).

© 2026 [tokyo](https://github.com/helloworldxdwastaken). All rights reserved.
