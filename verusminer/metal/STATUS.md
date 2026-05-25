# Metal VerusHash 2.2 — current state + roadmap

## TL;DR

CPU mining is **live and earning** on LuckPool. Metal acceleration is in
research/validation stage — kernels exist, infrastructure to validate
GPU-vs-CPU output is in place (`validate_haraka256`), but the haraka kernel
still mismatches CPU on test vectors. CL hash on GPU (needed for full
Verus mining) is unsolved because Metal has no `vmull_p64`-equivalent
polynomial multiply intrinsic.

## What's done ✅

- **Bit-sliced AES kernel benchmark**: 87.7 G AES rounds/sec on M5 GPU
  (33× single CPU core). Proves the GPU has the raw compute budget.
- **Phase 4 Metal infrastructure**: Swift dispatcher (`main.swift`), Metal
  kernel source loader, command queue setup, buffer management.
- **CPU↔GPU validation harness**: `validate_haraka256.swift` runs the
  same input through both CPU `haraka256_port` (from `cpu/haraka_portable.c`)
  and the GPU `haraka256_kernel`, then byte-diffs the 32-byte outputs on
  4 test vectors (countup, zeros, ones, pattern).
- **`uchar` shift promotion fix** in MixColumns (`X2` macro now uses
  `(int)x << 1` to avoid the 0x80-truncates-to-0 MSL quirk).

## What's NOT done ❌

### 1. haraka256 kernel correctness
Current GPU output does not match CPU for the canonical Haraka v2 test
vector (`8027ccb87949774b...`). The X2 fix is necessary but not sufficient.
Remaining suspects:
- SubBytes+ShiftRows column indexing (column-major byte assembly)
- MixColumns matrix application order (row vs col-major)
- TRUNCSTORE — current kernel writes 32 bytes, but haraka256 spec is 16
  bytes (high 64 of s0 + high 64 of s1). Kernel may be off-spec.
- Round key alignment between AES rounds (RC table loaded with right
  stride?)

### 2. haraka512 (the one VerusHash actually uses)
haraka256 is a stepping stone. Verus mining uses haraka512:
- State: 4 × 128-bit halves (s[0..3]), not 2
- Input: 64 bytes
- Output: 32 bytes (high 64 of each of 4 halves)
- 5 rounds of `AES4 + MIX4`
Code needs to be written once haraka256 is validated.

### 3. CL hash on GPU — the hard one
Verus `Finalize2b` runs `verusclhash_sv2_2` which does ~32 iterations of
carry-less polynomial multiply (`vmull_p64` / `_mm_clmulepi64_si128`).
Metal Shading Language has NO polynomial multiply intrinsic — no GF(2)
multiplication in hardware. Options:
- **Per-bit polynomial mul**: ~64 XOR-and-shift ops per CLMUL. Doable but
  ~50× slower than a hardware vmull_p64. Would dominate hash time.
- **Lookup table approach**: precompute a 256×256 byte product table on
  CPU, upload to GPU as a texture. Memory-bandwidth bound.
- **Mixed: keep CL hash on CPU, only run haraka chain on GPU**: easier
  but loses parallelism (CPU bottleneck per batch).

### 4. Batched dispatch + mining-loop integration
Once kernels are correct + fast, need:
- CPU prep loop: for each nonce in batch, set nonce → recompute blake2b
  → embed → memcpy + clear → push to GPU input buffer
- GPU dispatch: N hashes in parallel
- CPU consume loop: scan results vs target, submit if any pass
- Integration with stratum client + dev fee rotation

## Estimated remaining work

| Task | Difficulty | ETA |
|------|------------|-----|
| Fix haraka256 GPU correctness | Medium (kernel debugging) | 1–2 sessions |
| Port haraka512 (same template as 256) | Easy once 256 works | 1 session |
| CL hash on GPU (lookup table approach) | Hard (perf-critical) | 2–3 sessions |
| Mining loop + batched dispatch | Medium | 1–2 sessions |
| Tune batch size + throughput | Easy | 1 session |
| **Total** | | **~6–10 focused sessions** |

## What to try next

1. **Hash a known haraka256 test vector on GPU and CPU side-by-side, print
   intermediate state after each AES round.** This will pinpoint exactly
   which round / which sub-operation diverges. The cpu reference (`haraka256_port`)
   is correct (matches the canonical test vector); the GPU is wrong; the
   first round where they disagree tells you where the bug is.
2. **Once haraka256 matches**, write `haraka512.metal` — same algorithm
   with 4 halves instead of 2.
3. **Decide CL hash strategy**: prototype both lookup-table and per-bit
   approaches; benchmark; pick winner.
4. **End-to-end Verus hash on GPU**: validate via the same approach we
   used for canonical CVerusHashV2 — hash a known Verus block, compare to
   block hash from explorer.

## Files in this directory

| File | Purpose |
|------|---------|
| `haraka256.metal` | Per-thread Haraka256 kernel (bugs present) |
| `main.swift` | Original benchmark harness from Phase 4 |
| `aes_bench.swift` | Single-AES-round benchmark (87.7 G/s validated) |
| `validate_haraka256.swift` | CPU-vs-GPU byte-diff validator |
| `verusminer_gpu` | Compiled main.swift binary |
| `aes_bench` | Compiled aes_bench.swift binary |
| `validate_haraka256` | Compiled validator binary |
| `STATUS.md` | This file |
