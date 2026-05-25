// clmul_shim.cpp — extern "C" wrappers around the C++-name-mangled
// helpers in verus_clhash_portable.cpp, so Swift can call them via
// @_silgen_name without dealing with Itanium mangling.

#include <stdint.h>
#include <cstring>

// Pull in the same header chain verus_clhash_portable.cpp uses, so
// __m128i is the same opaque type the wrapped helpers expect.
#include "verus_hash.h"

extern void clmul64(uint64_t a, uint64_t b, uint64_t *r);
extern __m128i _mm_mulhrs_epi16_emu(__m128i a, __m128i b);

// Full verusclhash entry point (lives in verus_clhash_portable.cpp).
extern "C" uint64_t verusclhash_sv2_2_port(void *random, const unsigned char buf[64],
                                            uint64_t keyMask, __m128i **pMoveScratch);

// NOTE: most of the _mm_*_emu helpers in verus_clhash_portable.cpp are
// declared `inline` and have NO exported symbol — so we can't `extern`
// them and link. Instead, re-implement the small ones inline here using
// raw byte ops on the __m128i layout (which is layout-compatible with
// uint8_t[16] on both x86-64 and arm64).

extern "C" {

// clmul64: writes r[0] = low 64 bits of (a ⊗ b), r[1] = high 64 bits,
// where ⊗ is GF(2)[x] carryless multiplication.
void clmul64_wrap(uint64_t a, uint64_t b, uint64_t *r) {
    clmul64(a, b, r);
}

// mulhrs_epi16: 8-lane signed 16-bit fixed-point multiply with rounding.
// in_a, in_b, out are all 16-byte buffers (8 × int16_t LE).
void mulhrs_epi16_wrap(const uint8_t *in_a, const uint8_t *in_b, uint8_t *out) {
    __m128i a, b, r;
    std::memcpy(&a, in_a, 16);
    std::memcpy(&b, in_b, 16);
    r = _mm_mulhrs_epi16_emu(a, b);
    std::memcpy(out, &r, 16);
}

// precompReduction64: takes a 16-byte vector, returns 16 bytes where
// the low 8 are the reduced 64-bit hash (high 8 = garbage per the
// CPU reference). Re-implementation of precompReduction64_si128_port —
// the helpers it depends on are inline (no link symbol), so we inline
// the byte-level equivalents directly here. Bit-exact same algorithm.
void precomp_reduction64_wrap(const uint8_t *in_A, uint8_t *out) {
    // 1. C = 0x1B as low 64 of a 16-byte vec (high = 0)
    uint8_t C[16];
    std::memset(C, 0, 16);
    C[0] = 0x1B;

    // 2. Q2 = clmul(A.hi, C.lo) — uses imm=0x01 = pick a's HIGH u64, b's LOW u64
    uint64_t aHi = 0, cLo = 0;
    for (int i = 0; i < 8; i++) aHi |= ((uint64_t)in_A[i + 8]) << (8 * i);
    for (int i = 0; i < 8; i++) cLo |= ((uint64_t)C[i])        << (8 * i);
    uint64_t q2_r[2];
    clmul64(aHi, cLo, q2_r);
    uint8_t Q2[16];
    for (int i = 0; i < 8; i++) {
        Q2[i]     = (uint8_t)((q2_r[0] >> (8 * i)) & 0xff);
        Q2[i + 8] = (uint8_t)((q2_r[1] >> (8 * i)) & 0xff);
    }

    // 3. shifted = srli_si128(Q2, 8) — right-shift by 8 BYTES
    //    low 8 ← Q2[8..16], high 8 ← 0
    uint8_t Q2sh[16];
    for (int i = 0; i < 8; i++)  Q2sh[i]     = Q2[i + 8];
    for (int i = 8; i < 16; i++) Q2sh[i]     = 0;

    // 4. LUT lookup: for each byte of Q2sh, if MSB set output 0,
    //    else output LUT[byte & 0x0f]
    static const uint8_t LUT[16] = {
        0x00, 0x1b, 0x36, 0x2d, 0x6c, 0x77, 0x5a, 0x41,
        0xd8, 0xc3, 0xee, 0xf5, 0xb4, 0xaf, 0x82, 0x99
    };
    uint8_t Q3[16];
    for (int i = 0; i < 16; i++) {
        Q3[i] = (Q2sh[i] & 0x80) ? 0 : LUT[Q2sh[i] & 0x0f];
    }

    // 5. final = (Q2 XOR A) XOR Q3
    for (int i = 0; i < 16; i++) {
        out[i] = (Q2[i] ^ in_A[i]) ^ Q3[i];
    }
}


// Friendly wrapper around verusclhash_sv2_2_port that handles the
// pMoveScratch bookkeeping. Caller passes:
//   - key      — VERUSKEYSIZE=8832 byte buffer (mutated in place!)
//   - input    — 64 byte buffer
//   - keyMask  — usually 8192 (= VERUSKEYSIZE - 40*16 - 1 rounded down,
//                actually 0x2000 = 8192 for default Verus 2.2 config)
// Returns the 64-bit verusclhash output.
uint64_t verusclhash_sv2_2_wrap(uint8_t *key, const uint8_t *input, uint64_t keyMask) {
    // pMoveScratch records mutated key slots so caller can restore them.
    // We allocate enough for 32 iters × 2 ptrs = 64 entries.
    // pMoveScratch points to a writable array of __m128i*; the function
    // advances its local copy as it logs mutated slots. We discard.
    __m128i *scratch[80];
    __m128i **scratchPtr = scratch;
    return verusclhash_sv2_2_port(key, input, keyMask, scratchPtr);
}

} // extern "C"

// ============================================================
// CVerusHashV2 Reset + Write + Finalize2b — the full hash mining uses.
// One-shot initializer + a wrapper Swift can call directly.
// ============================================================
#include "verus_hash.h"

static bool g_vh2_inited = false;
static void ensure_vh2_inited() {
    if (!g_vh2_inited) {
        CVerusHashV2::init();
        g_vh2_inited = true;
    }
}

// Need haraka256_port + load_constants_port from haraka_portable.c
extern "C" void haraka256_port(unsigned char *out, const unsigned char *in);
extern "C" void load_constants_port();
// Both verusclhash variants — we want to compare optimized vs portable
extern "C" uint64_t verusclhash_sv2_2(void *random, const unsigned char buf[64],
                                       uint64_t keyMask, __m128i **pMoveScratch);

extern "C" {

// Fwd decls — defined below.
void verus_hash_v2_pre_verusclhash_curbuf(uint8_t *out_curBuf, const uint8_t *data, uint64_t len);
void verus_hash_v2_pre_verusclhash_key_endpoints(uint8_t *out_first32, uint8_t *out_last32, const uint8_t *data, uint64_t len);
void verus_hash_v2_custom_finalize(uint8_t *out_hash, uint8_t *out_intermediate, uint8_t *out_curBuf_pre_vclh, const uint8_t *data, uint64_t len);

// One-shot: takes input bytes + length, returns 32-byte hash. The CVerusHashV2
// object lives on the stack of this call; verusclhasher key buffer is
// thread-local in the CVerusHashV2 globals.
void verus_hash_v2_2b_wrap(uint8_t *out_hash, const uint8_t *data, uint64_t len) {
    ensure_vh2_inited();
    CVerusHashV2 vh2(SOLUTION_VERUSHHASH_V2_2);
    vh2.Reset();
    vh2.Write(data, (size_t)len);
    vh2.Finalize2b(out_hash);
}

// Trace variant: captures intermediate state to localize GPU divergence.
// trace layout (256 bytes, matches GPU layout):
//   [  0.. 64)  curBuf after Write
//   [ 64.. 96)  key[0..32] after GenNewCLKey block 0
//   [ 96..160)  curBuf after FillExtra(curBuf)
//   [160..168)  intermediate u64
//   [168..232)  curBuf right before final haraka512_keyed
//   [240..241)  curPos after Write (moved out of the [224..256] zone)
void verus_hash_v2_2b_wrap_traced(uint8_t *out_hash, const uint8_t *data,
                                   uint64_t len, uint8_t *trace) {
    ensure_vh2_inited();
    CVerusHashV2 vh2(SOLUTION_VERUSHHASH_V2_2);
    vh2.Reset();
    vh2.Write(data, (size_t)len);

    unsigned char *curBuf = vh2.CurBuffer();
    size_t curPos = (size_t)len % 32;

    // Checkpoint 1: curBuf after Write
    std::memcpy(trace + 0, curBuf, 64);
    trace[240] = (uint8_t)curPos;

    // Just run Finalize2b — after it completes we can grab the key and
    // re-derive the intermediate from the key+curBuf state if needed.
    vh2.Finalize2b(out_hash);

    // Compute PRE-verusclhash key endpoints by re-chaining haraka256 from
    // scratch. verusclhasher_key.get() after Finalize2b is POST-mutation —
    // unreliable for chain comparison.
    verus_hash_v2_pre_verusclhash_key_endpoints(trace + 64, trace + 96, data, len);

    // curBuf after Finalize2b retains the LAST FillExtra state:
    //   curBuf[0..32]  = pre-Finalize2b curBuf prefix
    //   curBuf[32..40] = intermediate u64 (LE), tiled 4× through [32..64]
    // So curBuf[32..40] IS the intermediate value.
    std::memcpy(trace + 160, curBuf + 32, 8);

    // [168..232] — curBuf PRE-verusclhash, captured INSIDE the custom Finalize2b
    // (matches real Finalize2b's view exactly)
    uint8_t cf_hash[32], cf_intermediate[8], cf_curBuf[64];
    verus_hash_v2_custom_finalize(cf_hash, cf_intermediate, cf_curBuf, data, len);
    std::memcpy(trace + 168, cf_curBuf, 64);
    // ALSO store custom_finalize's intermediate at trace[232..240] —
    // this should match Finalize2b's intermediate at trace[160..168].
    std::memcpy(trace + 232, cf_intermediate, 8);
    // And whether the custom hash matches the real Finalize2b hash
    trace[241] = (std::memcmp(cf_hash, out_hash, 32) == 0) ? 1 : 0;

    // ALSO compute intermediate via the PORTABLE verusclhash (verus_clhash_portable.cpp)
    // using the SAME inputs the GPU sees. If this matches GPU but differs from
    // the optimized intermediate at trace[160..168], the CPU optimized vs
    // portable implementations diverge — a CPU-side bug, not GPU.
    {
        load_constants_port();
        unsigned char preverus_curBuf[64];
        verus_hash_v2_pre_verusclhash_curbuf(preverus_curBuf, data, len);

        // Build the full key via haraka256_port chain
        static thread_local unsigned char portKey[17024];
        unsigned char src_chain[32];
        unsigned char tmp_chain[32];
        std::memcpy(src_chain, preverus_curBuf, 32);
        for (int b = 0; b < 276; b++) {
            haraka256_port(tmp_chain, src_chain);
            std::memcpy(portKey + b * 32, tmp_chain, 32);
            std::memcpy(src_chain, tmp_chain, 32);
        }
        // Refresh copy
        std::memcpy(portKey + 8832, portKey, 8192);

        __m128i *scratch_port[80];
        __m128i **scratchPtr_port = scratch_port;
        uint64_t portInt = verusclhash_sv2_2_port(portKey, preverus_curBuf, 8191, scratchPtr_port);
        std::memcpy(trace + 200, &portInt, 8);    // CPU PORTABLE intermediate at trace[200..208]
    }
}

// Compute what curBuf SHOULD be right before verusclhash on CPU, by manually
// doing Reset+Write+FillExtra(curBuf). Returns the post-FillExtra curBuf state.
// Used to verify the GPU wrapper produces the same input to verusclhash.
void verus_hash_v2_pre_verusclhash_curbuf(uint8_t *out_curBuf,
                                          const uint8_t *data, uint64_t len) {
    ensure_vh2_inited();
    CVerusHashV2 vh2(SOLUTION_VERUSHHASH_V2_2);
    vh2.Reset();
    vh2.Write(data, (size_t)len);

    unsigned char *curBuf = vh2.CurBuffer();
    size_t curPos = (size_t)len % 32;

    // FillExtra(curBuf) — replicate the template
    {
        size_t pos = curPos;
        size_t left = 32 - curPos;
        do {
            size_t L = left > 16 ? 16 : left;
            std::memcpy(curBuf + 32 + pos, curBuf, L);
            pos += L; left -= L;
        } while (left > 0);
    }

    std::memcpy(out_curBuf, curBuf, 64);
}

// Compute PRE-verusclhash key on CPU by chaining haraka256_port from
// post-Write+FillExtra curBuf for 276 blocks. Dumps first 32 + last 32 bytes.
// This is what the key should be BEFORE verusclhash mutates it — independent
// of what verusclhasher_key contains after Finalize2b.
// CUSTOM Finalize2b — replicates CVerusHashV2::Finalize2b body with full
// visibility into intermediates. If hash matches vh2.Finalize2b, our
// reconstruction is exact and we can trust the intermediates we capture.
// Uses the SAME haraka256/512_keyed function pointers + the SAME
// verusclhasher_key thread_local + the SAME vh2.vclh.
void verus_hash_v2_custom_finalize(uint8_t *out_hash,
                                    uint8_t *out_intermediate,  // 8 bytes
                                    uint8_t *out_curBuf_pre_vclh,  // 64 bytes
                                    const uint8_t *data, uint64_t len) {
    ensure_vh2_inited();
    CVerusHashV2 vh2(SOLUTION_VERUSHHASH_V2_2);
    vh2.Reset();
    vh2.Write(data, (size_t)len);

    unsigned char *curBuf = vh2.CurBuffer();
    size_t curPos = (size_t)len % 32;

    // Step 1: FillExtra((u128 *)curBuf) — same as Finalize2b
    {
        size_t pos = curPos;
        size_t left = 32 - pos;
        do {
            size_t L = left > 16 ? 16 : left;
            std::memcpy(curBuf + 32 + pos, curBuf, L);
            pos += L; left -= L;
        } while (left > 0);
    }

    // Snapshot curBuf pre-verusclhash
    std::memcpy(out_curBuf_pre_vclh, curBuf, 64);

    // Step 2: GenNewCLKey(curBuf)
    u128 *key = CVerusHashV2::GenNewCLKey(curBuf);

    // Step 3: intermediate = vclh(curBuf, key) — matches Finalize2b
    uint64_t intermediate = vh2.vclh(curBuf, key);
    std::memcpy(out_intermediate, &intermediate, 8);

    // Step 4: FillExtra(&intermediate)
    {
        size_t pos = curPos;
        size_t left = 32 - pos;
        do {
            size_t L = left > 8 ? 8 : left;
            std::memcpy(curBuf + 32 + pos, &intermediate, L);
            pos += L; left -= L;
        } while (left > 0);
    }

    // Step 5: haraka512_keyed
    (*CVerusHashV2::haraka512KeyedFunction)(out_hash, curBuf,
        key + vh2.IntermediateTo128Offset(intermediate));
}

void verus_hash_v2_pre_verusclhash_key_endpoints(uint8_t *out_first32,
                                                  uint8_t *out_last32,
                                                  const uint8_t *data, uint64_t len) {
    // CVerusHashV2::init() on Apple Silicon calls load_constants() (the AES-NI
    // variant) but NOT load_constants_port(). haraka256_port reads from a
    // separate `rc[]` global that load_constants_port populates. If we call
    // haraka256_port without first calling load_constants_port, we read
    // uninitialized memory and get garbage.
    load_constants_port();

    unsigned char curBuf[64];
    verus_hash_v2_pre_verusclhash_curbuf(curBuf, data, len);

    // Chain haraka256_port 276 times, only keeping first and last 32 bytes
    unsigned char src[32];
    unsigned char tmp[32];
    std::memcpy(src, curBuf, 32);

    for (int b = 0; b < 276; b++) {
        haraka256_port(tmp, src);
        if (b == 0) std::memcpy(out_first32, tmp, 32);
        if (b == 275) std::memcpy(out_last32, tmp, 32);
        std::memcpy(src, tmp, 32);
    }
}

} // extern "C"
